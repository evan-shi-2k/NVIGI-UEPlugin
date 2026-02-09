// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "IGIGPT.h"

#include "CoreMinimal.h"
#include "ID3D12DynamicRHI.h"

#include "IGIModule.h"
#include "IGILog.h"

#include "nvigi.h"
#include "nvigi_ai.h"
#include "nvigi_gpt.h"
#include "nvigi_stl_helpers.h"
#include "nvigi_struct.h"

#pragma warning( push )
#pragma warning( disable : 5257 )
#include "nvigi_d3d12.h"
#pragma warning( pop )

#include <condition_variable>
#include <thread>
#include <mutex>
#include <cstdlib>

#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/InteractiveProcess.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"

namespace
{
    constexpr const char* const GGUF_MODEL_MINITRON{ "{8E31808B-C182-4016-9ED8-64804FF5B40D}" };

    constexpr std::size_t VRAM_BUDGET_RECOMMENDATION{ 1024 * 12 };
    constexpr std::size_t THREAD_NUM_RECOMMENDATION{ 1 }; // Recommended number of threads for CiG
    constexpr std::size_t CONTEXT_SIZE_RECOMMENDATION{ 4096 };

    static constexpr double kRequestTimeoutSeconds = 30.0;
    static constexpr double kStartupTimeoutSeconds = 30.0;
    static constexpr double kReadPollIntervalSeconds = 0.01;
}

static FString Quote(const FString& S)
{
#if PLATFORM_WINDOWS
    return FString::Printf(TEXT("\"%s\""), *S.Replace(TEXT("\""), TEXT("\\\"")));
#else
    return FString::Printf(TEXT("'%s'"), *S.Replace(TEXT("'"), TEXT("'\"'\"'")));
#endif
}

static int32 FindJsonEndIndex(const FString& S)
{
    bool bStarted = false;
    bool bInString = false;
    bool bEscape = false;
    int32 ObjDepth = 0;
    int32 ArrDepth = 0;

    for (int32 i = 0; i < S.Len(); ++i)
    {
        const TCHAR c = S[i];

        if (!bStarted)
        {
            if (c == '{') { bStarted = true; ObjDepth = 1; continue; }
            if (c == '[') { bStarted = true; ArrDepth = 1; continue; }
            continue; // skip any junk before JSON starts
        }

        if (bInString)
        {
            if (bEscape) { bEscape = false; continue; }
            if (c == '\\') { bEscape = true; continue; }
            if (c == '"') { bInString = false; continue; }
            continue;
        }

        if (c == '"') { bInString = true; continue; }
        if (c == '{') ++ObjDepth;
        else if (c == '}') --ObjDepth;
        else if (c == '[') ++ArrDepth;
        else if (c == ']') --ArrDepth;

        if (bStarted && ObjDepth == 0 && ArrDepth == 0)
        {
            return i; // inclusive
        }
    }

    return INDEX_NONE;
}

static FString SanitizeForLog(const FString& In, int32 MaxLen = 512)
{
    FString Out;
    Out.Reserve(FMath::Min(In.Len(), MaxLen));

    for (int32 i = 0; i < In.Len() && Out.Len() < MaxLen; ++i)
    {
        const TCHAR C = In[i];

        if (C == TEXT('\r') || C == TEXT('\n') || C == TEXT('\t'))
        {
            Out.AppendChar(TEXT(' '));
            continue;
        }

        if (C >= 32 && C < 127)
        {
            Out.AppendChar(C);
        }
        else
        {
            Out.AppendChar(TEXT('?'));
        }
    }

    if (In.Len() > MaxLen)
    {
        Out += TEXT("...");
    }
    return Out;
}

static bool ExtractJsonPayload(const FString& InLine, FString& OutJson)
{
    FString S = InLine;
    S.ReplaceInline(TEXT("\r"), TEXT(""));
    S.TrimStartAndEndInline();
    if (S.IsEmpty())
    {
        return false;
    }

    const int32 ObjIdx = S.Find(TEXT("{"), ESearchCase::CaseSensitive);
    const int32 ArrIdx = S.Find(TEXT("["), ESearchCase::CaseSensitive);
    int32 StartIdx = INDEX_NONE;

    if (ObjIdx != INDEX_NONE && (ArrIdx == INDEX_NONE || ObjIdx < ArrIdx))
    {
        StartIdx = ObjIdx;
    }
    else if (ArrIdx != INDEX_NONE)
    {
        StartIdx = ArrIdx;
    }

    if (StartIdx == INDEX_NONE)
    {
        return false;
    }

    S = S.Mid(StartIdx);

    const int32 EndIdx = FindJsonEndIndex(S);
    if (EndIdx == INDEX_NONE)
    {
        return false; // incomplete JSON (shouldn't happen for line-based protocol)
    }

    OutJson = S.Left(EndIdx + 1);
    return true;
}


static FString DefaultPythonExe()
{
#if PLATFORM_WINDOWS
    // <ProjectDir>/ACE/ace_venv/Scripts/python.exe
    return FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("ace_venv"), TEXT("Scripts"), TEXT("python.exe"));
#else
    // <ProjectDir>/ACE/ace_venv/bin/python3
    return FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("ace_venv"), TEXT("bin"), TEXT("python3"));
#endif
}

// TODO: Remove
class FPythonMonitoredSingleShot
{
public:
    FPythonMonitoredSingleShot() { ConfigureFromEnv(); }
    ~FPythonMonitoredSingleShot() { /* nothing persistent to stop */ }

    void ConfigureFromEnv()
    {
        FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONIOENCODING"), TEXT("utf-8"));

        BaseUrl = GetEnvOrDefault(TEXT("NIM_BASE_URL"), TEXT("http://127.0.0.1:8000/v1"));
        ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("NIM_API_KEY"));
        Model = GetEnvOrDefault(TEXT("NIM_MODEL_NAME"), TEXT("meta/llama-3.2-3b-instruct"));
        Mode = GetEnvOrDefault(TEXT("IGI_NIM_MODE"), TEXT("grammar"));

        ScriptPath = GetEnvOrDefault(TEXT("IGI_NIM_SCRIPT_PATH"),
            FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("nim_structured.py"))));
        SystemPromptPath = GetEnvOrDefault(TEXT("IGI_NIM_SYSTEM_PROMPT_PATH"),
            FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("system_prompt.txt"))));
        AssistantPromptPath = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_ASSISTANT_PROMPT_PATH"));
        DefaultGrammarPath = GetEnvOrDefault(TEXT("IGI_NIM_GRAMMAR_PATH"),
            FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("command_schema.ebnf"))));
        DefaultJsonSchemaPath = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_JSON_PATH"));

        PythonExe = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_PYTHON_EXE"));
        if (PythonExe.IsEmpty()) PythonExe = DefaultPythonExe();
        if (!FPaths::FileExists(PythonExe))
        {
            UE_LOG(LogIGISDK, Warning, TEXT("Python not found at %s; will try 'python' in PATH"), *PythonExe);
            PythonExe = TEXT("python");
        }
    }

    FString RequestSingleShotJSON(const FString& UserJsonOneLine, const FString& GrammarPath, double TimeoutSec = 30.0)
    {
        TArray<FString> Args;
        Args.Add(TEXT("-u"));
        Args.Add(Quote(ScriptPath));
        Args.Add(TEXT("--base-url")); Args.Add(Quote(BaseUrl));
        if (!ApiKey.IsEmpty())
        {
            Args.Add(TEXT("--api-key")); Args.Add(Quote(ApiKey));
        }
        Args.Add(TEXT("--model")); Args.Add(Quote(Model));
        Args.Add(TEXT("--mode"));  Args.Add(Quote(Mode));
        if (!SystemPromptPath.IsEmpty())
        {
            Args.Add(TEXT("--system")); Args.Add(Quote(SystemPromptPath));
        }
        if (!AssistantPromptPath.IsEmpty())
        {
            Args.Add(TEXT("--assistant")); Args.Add(Quote(AssistantPromptPath));
        }
        if (Mode.Equals(TEXT("grammar"), ESearchCase::IgnoreCase))
        {
            if (!GrammarPath.IsEmpty())
            {
                Args.Add(TEXT("--grammar")); Args.Add(Quote(GrammarPath));
            }
            else if (!DefaultGrammarPath.IsEmpty())
            {
                Args.Add(TEXT("--grammar")); Args.Add(Quote(DefaultGrammarPath));
            }
        }
        else
        {
            if (!DefaultJsonSchemaPath.IsEmpty())
            {
                Args.Add(TEXT("--json-schema")); Args.Add(Quote(DefaultJsonSchemaPath));
            }
        }
        Args.Add(TEXT("--user"));
        Args.Add(Quote(UserJsonOneLine));

        const FString ArgLine = FString::Join(Args, TEXT(" "));
        UE_LOG(LogIGISDK, Log, TEXT("[monitored] Launch: %s %s"), *PythonExe, *ArgLine);

        TSharedPtr<FMonitoredProcess> Proc = MakeShared<FMonitoredProcess>(
            PythonExe, ArgLine, /*bHidden=*/true, /*bCreatePipes=*/true);

        FString LastNonEmpty;
        Proc->OnOutput().BindLambda([&LastNonEmpty](const FString& Line)
            {
                const FString Trimmed = Line.TrimStartAndEnd();
                if (!Trimmed.IsEmpty())
                {
                    LastNonEmpty = Trimmed;
                }
                UE_LOG(LogIGISDK, Verbose, TEXT("[monitored][out] %s"), *Line);
            });

        bool bLaunched = Proc->Launch();
        if (!bLaunched)
        {
            UE_LOG(LogIGISDK, Error, TEXT("[monitored] launch failed"));
            return TEXT("{\"error\":\"launch_failed\"}");
        }

        const double T0 = FPlatformTime::Seconds();
        bool bRunning = true;
        while (bRunning && (FPlatformTime::Seconds() - T0) < TimeoutSec)
        {
            bRunning = Proc->Update();
            FPlatformProcess::Sleep(0.01);
        }

        if (bRunning)
        {
            UE_LOG(LogIGISDK, Warning, TEXT("[monitored] timeout; terminating child"));
            Proc->Cancel(/*KillTree=*/true);
            return TEXT("{\"error\":\"timeout\"}");
        }

        const int32 ReturnCode = Proc->GetReturnCode();

        if (LastNonEmpty.IsEmpty())
        {
            return FString::Printf(TEXT("{\"error\":\"empty_stdout\",\"exit\":%d}"), ReturnCode);
        }

        return LastNonEmpty;
    }

private:
    static FString GetEnvOrDefault(const TCHAR* Name, const FString& Fallback)
    {
        const FString V = FPlatformMisc::GetEnvironmentVariable(Name);
        return V.IsEmpty() ? Fallback : V;
    }

private:
    FString PythonExe;
    FString ScriptPath;
    FString BaseUrl, ApiKey, Model, Mode;
    FString SystemPromptPath, AssistantPromptPath;
    FString DefaultGrammarPath, DefaultJsonSchemaPath;
};

class FPythonPersistentClient
{
public:
    FPythonPersistentClient()
    {
        ConfigureFromEnv();
    }

    ~FPythonPersistentClient()
    {
        Stop();
    }

    void ConfigureFromEnv()
    {
        FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONIOENCODING"), TEXT("utf-8"));

        BaseUrl = GetEnvOrDefault(TEXT("NIM_BASE_URL"), TEXT("http://127.0.0.1:8000/v1"));
        ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("NIM_API_KEY"));
        Model = GetEnvOrDefault(TEXT("NIM_MODEL_NAME"), TEXT("meta/llama-3.2-3b-instruct"));
        Mode = GetEnvOrDefault(TEXT("IGI_NIM_MODE"), TEXT("grammar"));

        ScriptPath = GetEnvOrDefault(TEXT("IGI_NIM_SCRIPT_PATH"),
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("nim_structured.py"))));

        SystemPromptPath = GetEnvOrDefault(TEXT("IGI_NIM_SYSTEM_PROMPT_PATH"),
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("system_prompt.txt"))));

        GrammarPath = GetEnvOrDefault(TEXT("IGI_NIM_GRAMMAR_PATH"),
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("command_schema.ebnf"))));

        PythonExe = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_PYTHON_EXE"));
        if (PythonExe.IsEmpty())
        {
            PythonExe = DefaultPythonExe();
        }
        if (!FPaths::FileExists(PythonExe))
        {
            UE_LOG(LogIGISDK, Warning,
                TEXT("[persist] Python not found at %s; will try 'python' in PATH"), *PythonExe);
            PythonExe = TEXT("python");
        }
    }

    bool StartAndPing(double TimeoutSec)
    {
        FScopeLock Lock(&Mutex);

        if (Interactive && Interactive->IsRunning())
        {
            return true;
        }

        bSawPong.store(false, std::memory_order_relaxed);
        OutputBuffer.Empty();
        {
            FString Dummy;
            while (PendingLines.Dequeue(Dummy)) {}
        }

        FString Args = FString::Printf(
            TEXT("-u %s --serve-stdin --base-url %s --model %s --mode %s"),
            *Quote(ScriptPath), *Quote(BaseUrl), *Quote(Model), *Quote(Mode));

        if (!SystemPromptPath.IsEmpty())
        {
            Args += FString::Printf(TEXT(" --system %s"), *Quote(SystemPromptPath));
        }
        if (!GrammarPath.IsEmpty() && Mode == TEXT("grammar"))
        {
            Args += FString::Printf(TEXT(" --grammar %s"), *Quote(GrammarPath));
        }

        if (!ApiKey.IsEmpty())
        {
            FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_API_KEY"), *ApiKey);
        }

        UE_LOG(LogIGISDK, Log, TEXT("[persist] Launch: %s %s"), *PythonExe, *Args);

        FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONUTF8"), TEXT("1"));
        FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONIOENCODING"), TEXT("utf-8"));
        FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONUNBUFFERED"), TEXT("1"));

        Interactive = MakeUnique<FInteractiveProcess>(
            PythonExe,
            Args,
            /*InHidden*/ true,
            /*LongTime*/ true);

        Interactive->OnOutput().BindLambda([this](const FString& OutputChunk)
        {
            TArray<FString> Lines;
            OutputChunk.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
            if (Lines.Num() == 0)
            {
                Lines.Add(OutputChunk);
            }

            for (const FString& Line : Lines)
            {
                FString JsonLine;
                if (ExtractJsonPayload(Line, JsonLine))
                {
                    UE_LOG(LogIGISDK, Verbose, TEXT("[persist][json] %s"), *JsonLine);

                    if (JsonLine.Contains(TEXT("\"pong\"")))
                    {
                        bSawPong.store(true, std::memory_order_relaxed);
                        continue;
                    }

                    {
                        FScopeLock OutLock(&OutputMutex);
                        OutputBuffer += JsonLine;
                        OutputBuffer += TEXT("\n");
                        PendingLines.Enqueue(JsonLine);
                    }
                }
                else
                {
                    UE_LOG(LogIGISDK, VeryVerbose, TEXT("[persist][nonjson] %s"), *SanitizeForLog(Line));
                }
            }
        });

        Interactive->OnCompleted().BindLambda(
        [this](int32 ReturnCode, bool bCanceling)
        {
            UE_LOG(LogIGISDK,
                Error,
                TEXT("[persist] Python process completed with code %d (Canceled=%s)"),
                ReturnCode,
                bCanceling ? TEXT("true") : TEXT("false")
                );
        });

        if (!Interactive->Launch())
        {
            UE_LOG(LogIGISDK, Error, TEXT("[persist] Failed to launch python process"));
            Interactive.Reset();
            return false;
        }

        SendLine(TEXT("{\"__cmd\":\"ping\"}"));

        const double T0 = FPlatformTime::Seconds();
        while ((FPlatformTime::Seconds() - T0) < TimeoutSec)
        {
            if (!Interactive->IsRunning())
            {
                UE_LOG(LogIGISDK, Error, TEXT("[persist] Python exited during ping"));
                return false;
            }

            if (bSawPong.load(std::memory_order_relaxed))
            {
                // Drain any queued pong/banner lines so the first real request won't pop them.
                {
                    FScopeLock OutLock(&OutputMutex);
                    OutputBuffer.Empty();
                    FString Dummy;
                    while (PendingLines.Dequeue(Dummy)) {}
                }

                UE_LOG(LogIGISDK, Log, TEXT("[persist] Python ready"));
                return true;
            }

            FPlatformProcess::SleepNoStats(0.01f);
        }

        UE_LOG(LogIGISDK, Error, TEXT("[persist] ping timeout (no pong seen in output)"));
        Stop();
        return false;
    }

    void Stop()
    {
        FScopeLock Lock(&Mutex);

        if (Interactive)
        {
            Interactive->Cancel(/*KillTree*/ true);
            Interactive.Reset();
        }

        OutputBuffer.Empty();
        FString Dummy;
        while (PendingLines.Dequeue(Dummy)) {}
    }

    bool IsRunning() const
    {
        return Interactive && Interactive->IsRunning();
    }

    FString RequestJSON(const FString& UserJsonOneLine, double TimeoutSec)
    {
        FScopeLock Lock(&Mutex);

        if (!Interactive || !Interactive->IsRunning())
        {
            return TEXT("{\"error\":\"not_running\"}");
        }

        {
            FScopeLock OutLock(&OutputMutex);
            OutputBuffer.Empty();
            FString Dummy;
            while (PendingLines.Dequeue(Dummy)) {}
        }

        SendLine(UserJsonOneLine);
        FString Line;
        if (PopLine(Line, TimeoutSec))
        {
            return Line;
        }

        return TEXT("{\"error\":\"timeout\"}");
    }

private:
    void SendLine(const FString& Line)
    {
        if (!Interactive) return;
        const FString WithNL = Line + TEXT("\n");
        Interactive->SendWhenReady(WithNL);
    }

    bool PopLine(FString& Out, double TimeoutSec)
    {
        const double T0 = FPlatformTime::Seconds();
        for (;;)
        {
            {
                FScopeLock OutLock(&OutputMutex);
                if (PendingLines.Dequeue(Out))
                {
                    return true;
                }
            }

            if ((FPlatformTime::Seconds() - T0) >= TimeoutSec)
            {
                return false;
            }

            FPlatformProcess::SleepNoStats(0.005f);
        }
    }

    static FString Quote(const FString& S)
    {
        return FString::Printf(TEXT("\"%s\""), *S.Replace(TEXT("\""), TEXT("\\\"")));
    }

    static FString GetEnvOrDefault(const TCHAR* Name, const FString& Fallback)
    {
        const FString V = FPlatformMisc::GetEnvironmentVariable(Name);
        return V.IsEmpty() ? Fallback : V;
    }

private:
    std::atomic<bool> bSawPong{ false };
    mutable FCriticalSection Mutex;

    FString BaseUrl, ApiKey, Model, Mode;
    FString ScriptPath, SystemPromptPath, GrammarPath, JsonSchemaPath, PythonExe;

    TUniquePtr<FInteractiveProcess> Interactive;

    mutable FCriticalSection OutputMutex;
    FString OutputBuffer;
    TQueue<FString> PendingLines;
};

class FIGIGPT::Impl
{
public:
    Impl(FIGIModule* IGIModule)
        : IGIModulePtr(IGIModule)
    {
        nvigi::Result Result = nvigi::kResultOk;

        IGIModulePtr->LoadIGIFeature(nvigi::plugin::gpt::ggml::cuda::kId, &GPTInterface, nullptr);

        nvigi::GPTCreationParameters params{};

        nvigi::CommonCreationParameters common{};
        auto ConvertedString = StringCast<UTF8CHAR>(*IGIModulePtr->GetModelsPath());
        common.utf8PathToModels = reinterpret_cast<const char*>(ConvertedString.Get());
        common.numThreads = THREAD_NUM_RECOMMENDATION;
        common.vramBudgetMB = VRAM_BUDGET_RECOMMENDATION;
        common.modelGUID = GGUF_MODEL_MINITRON;
        Result = params.chain(common);
        if (Result != nvigi::kResultOk)
        {
            UE_LOG(LogIGISDK, Error, TEXT("[GPT] Unable to chain common parameters; cannot use CiG: %s"), *GetIGIStatusString(Result));
            GPTInstance = nullptr;
        }

        nvigi::D3D12Parameters d3d12Params{};
        if (GDynamicRHI &&
            GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::D3D12)
        {
            ID3D12DynamicRHI* RHI = static_cast<ID3D12DynamicRHI*>(GDynamicRHI);
            if (RHI)
            {
                ID3D12CommandQueue* CmdQ = RHI->RHIGetCommandQueue();
                constexpr uint32 RHI_DEVICE_INDEX = 0u;
                ID3D12Device* D3D12Device = RHI->RHIGetDevice(RHI_DEVICE_INDEX);

                if (CmdQ && D3D12Device)
                {
                    d3d12Params.device = D3D12Device;
                    d3d12Params.queue = CmdQ;

                    Result = params.chain(d3d12Params);
                    if (Result != nvigi::kResultOk)
                    {
                        UE_LOG(LogIGISDK, Error, TEXT("[GPT] Unable to chain D3D12 parameters; cannot use CiG: %s"), *GetIGIStatusString(Result));
                        GPTInstance = nullptr;
                    }
                }
                else
                {
                    UE_LOG(LogIGISDK, Error, TEXT("[GPT] Unable to retrieve D3D12 device and command queue from UE; cannot use CiG: %s"), *GetIGIStatusString(Result));
                    GPTInstance = nullptr;
                }
            }
            else
            {
                UE_LOG(LogIGISDK, Error, TEXT("[GPT] Unable to retrieve RHI instance from UE; cannot use CiG: %s"), *GetIGIStatusString(Result));
                GPTInstance = nullptr;
            }
        }
        else
        {
            UE_LOG(LogIGISDK, Log, TEXT("[GPT] UE not using D3D12; cannot use CiG: %s"), *GetIGIStatusString(Result));
            GPTInstance = nullptr;
        }

        Result = GPTInterface->createInstance(params, &GPTInstance);
        if (Result != nvigi::kResultOk)
        {
            UE_LOG(LogIGISDK, Fatal, TEXT("[GPT] Unable to create gpt.ggml.cuda instance: %s"), *GetIGIStatusString(Result));
            GPTInstance = nullptr;
        }

        PythonClient = MakeUnique<FPythonMonitoredSingleShot>();
        PythonClient->ConfigureFromEnv();

        PythonPersistent = MakeUnique<FPythonPersistentClient>();
        PythonPersistent->StartAndPing(30.0f);
    }

    virtual ~Impl()
    {
        PythonClient.Reset();

        if (PythonPersistent.IsValid())
            PythonPersistent->Stop();

        if (GPTInstance != nullptr)
        {
            GPTInterface->destroyInstance(GPTInstance);
            GPTInstance = nullptr;
        }

        if (IGIModulePtr)
        {
            IGIModulePtr->UnloadIGIFeature(nvigi::plugin::gpt::ggml::cuda::kId, GPTInterface);
            IGIModulePtr = nullptr;
        }
    }

    void StartPersistentPython(double TimeoutSec)
    {
        if (!PythonPersistent.IsValid())
            PythonPersistent = MakeUnique<FPythonPersistentClient>();
        PythonPersistent->StartAndPing(TimeoutSec);
    }

    void StopPersistentPython()
    {
        if (PythonPersistent.IsValid())
            PythonPersistent->Stop();
    }

    FString Evaluate(const FString& UserPrompt)
    {
        FScopeLock Lock(&CS_ACE);

        struct BasicCallbackCtx
        {
            std::mutex callbackMutex;
            std::condition_variable callbackCV;
            std::atomic<nvigi::InferenceExecutionState> callbackState = nvigi::kInferenceExecutionStateDataPending;
            FString gptOutput;
        };
        BasicCallbackCtx cbkCtx;

        auto completionCallback = [](const nvigi::InferenceExecutionContext* ctx, nvigi::InferenceExecutionState state, void* data) -> nvigi::InferenceExecutionState
            {
                if (!data)
                    return nvigi::kInferenceExecutionStateInvalid;

                auto cbkCtx = (BasicCallbackCtx*)data;
                std::scoped_lock lck(cbkCtx->callbackMutex);

                // Outputs from GPT
                auto slots = ctx->outputs;
                const nvigi::InferenceDataText* text{};
                slots->findAndValidateSlot(nvigi::kGPTDataSlotResponse, &text);
                auto response = FString(StringCast<UTF8CHAR>(text->getUTF8Text()));
                if (response.Contains("<JSON>") && response.Contains("</JSON>"))
                {
                    const int32 Start = response.Find("<JSON>") + 6;
                    const int32 End = response.Find("</JSON>");
                    const FString JsonSlice = response.Mid(Start, End - Start);
                    cbkCtx->gptOutput += TEXT("\n{JSON}") + JsonSlice + TEXT("{/JSON}\n");

                    auto cpuBuffer = castTo<nvigi::CpuData>(text->utf8Text);
                    if (cpuBuffer) { ((uint8_t*)cpuBuffer->buffer)[0] = 0; cpuBuffer->sizeInBytes = 0; }
                }
                else
                {
                    cbkCtx->gptOutput += response;
                }

                cbkCtx->callbackState = state;
                cbkCtx->callbackCV.notify_one();

                return state;
            };

        auto UserPromptUTF = StringCast<UTF8CHAR>(*UserPrompt);
        nvigi::InferenceDataTextSTLHelper UserPromptData(reinterpret_cast<const char*>(UserPromptUTF.Get()));

        TArray<nvigi::InferenceDataSlot> inSlots = {
            {nvigi::kGPTDataSlotUser, UserPromptData}
        };

        nvigi::InferenceDataSlotArray inputs = { static_cast<size_t>(inSlots.Num()), inSlots.GetData() };

        // Parameters
        nvigi::GPTRuntimeParameters runtime{};
        runtime.seed = -1;
        runtime.tokensToPredict = 200;
        runtime.interactive = false;

        nvigi::InferenceExecutionContext gptCtx{};
        nvigi::InferenceInstance* instance = GPTInstance;
        gptCtx.instance = instance;
        gptCtx.callback = completionCallback;
        gptCtx.callbackUserData = &cbkCtx;
        gptCtx.inputs = &inputs;
        gptCtx.runtimeParameters = runtime;

        cbkCtx.callbackState = nvigi::kInferenceExecutionStateDataPending;

        instance->evaluateAsync(&gptCtx);

        {
            std::unique_lock lck(cbkCtx.callbackMutex);
            cbkCtx.callbackCV.wait(lck, [&cbkCtx]()
                {
                    return cbkCtx.callbackState != nvigi::kInferenceExecutionStateDataPending;
                });
        }

        FString response(cbkCtx.gptOutput);

        return response;
    }

    FString EvaluateStructuredWithGrammar(const FString& UserPrompt, const FString& GrammarPath)
    {
        TSharedPtr<FJsonObject> Obj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UserPrompt);
        FJsonSerializer::Deserialize(Reader, Obj);

        if (!GrammarPath.IsEmpty())
        {
            Obj->SetStringField(TEXT("grammar_path"), GrammarPath);
        }

        FString OneLine;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OneLine);
        FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
        OneLine.ReplaceInline(TEXT("\r"), TEXT(""));
        OneLine.ReplaceInline(TEXT("\n"), TEXT(""));

        if (PythonPersistent.IsValid() && PythonPersistent->IsRunning())
        {
            FString Resp = PythonPersistent->RequestJSON(OneLine, 60.0);
            if (!Resp.IsEmpty() && !Resp.StartsWith(TEXT("{\"error\"")))
            {
                return Resp;
            }
            UE_LOG(LogIGISDK, Warning, TEXT("[gpt] persistent failed, falling back: %s"), *Resp);
        }

        if (!PythonClient.IsValid())
        {
            PythonClient = MakeUnique<FPythonMonitoredSingleShot>();
            PythonClient->ConfigureFromEnv();
        }

        return PythonClient->RequestSingleShotJSON(OneLine, GrammarPath, /*TimeoutSec=*/60.0);
    }

private:
    // Non-owning ptr
    FIGIModule* IGIModulePtr;

    nvigi::IGeneralPurposeTransformer* GPTInterface{ nullptr };
    nvigi::InferenceInstance* GPTInstance{ nullptr };
    FCriticalSection CS_ACE;

    TUniquePtr<FPythonMonitoredSingleShot> PythonClient;
    TUniquePtr<FPythonPersistentClient> PythonPersistent;

    FString TempOut;
};

FIGIGPT::FIGIGPT(FIGIModule* IGIModule)
{
    Pimpl = MakePimpl<FIGIGPT::Impl>(IGIModule);
}

FIGIGPT::~FIGIGPT() {}

FString FIGIGPT::Evaluate(const FString& UserPrompt)
{
    return Pimpl->Evaluate(UserPrompt);
}

FString FIGIGPT::EvaluateStructuredWithGrammar(const FString& UserPrompt, const FString& GrammarPath)
{
    return Pimpl->EvaluateStructuredWithGrammar(UserPrompt, GrammarPath);
}