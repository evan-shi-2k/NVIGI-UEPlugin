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
    static constexpr double kStartupTimeoutSeconds = 10.0;
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

static FString DefaultPythonExe()
{
#if PLATFORM_WINDOWS
    // <ProjectDir>/ACE/ace_env/Scripts/python.exe
    return FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("ace_env"), TEXT("Scripts"), TEXT("python.exe"));
#else
    // <ProjectDir>/ACE/ace_env/bin/python3
    return FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("ace_env"), TEXT("bin"), TEXT("python3"));
#endif
}

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
        GrammarPath = GetEnvOrDefault(TEXT("IGI_NIM_GRAMMAR_PATH"),
            FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("command_schema.ebnf"))));
        JsonSchemaPath = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_JSON_PATH"));

        PythonExe = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_PYTHON_EXE"));
        if (PythonExe.IsEmpty()) PythonExe = DefaultPythonExe();
        if (!FPaths::FileExists(PythonExe))
        {
            UE_LOG(LogIGISDK, Warning, TEXT("Python not found at %s; will try 'python' in PATH"), *PythonExe);
            PythonExe = TEXT("python");
        }
    }

    FString RequestSingleShotJSON(const FString& UserJsonOneLine, double TimeoutSec = 30.0)
    {
        TArray<FString> Args;
        Args.Add(TEXT("-u"));
        Args.Add(Quote(ScriptPath));
        Args.Add(TEXT("--base-url"));      Args.Add(Quote(BaseUrl));
        if (!ApiKey.IsEmpty()) { Args.Add(TEXT("--api-key")); Args.Add(Quote(ApiKey)); }
        Args.Add(TEXT("--model"));         Args.Add(Quote(Model));
        Args.Add(TEXT("--mode"));          Args.Add(Quote(Mode));
        if (!SystemPromptPath.IsEmpty()) { Args.Add(TEXT("--system"));    Args.Add(Quote(SystemPromptPath)); }
        if (!AssistantPromptPath.IsEmpty()) { Args.Add(TEXT("--assistant")); Args.Add(Quote(AssistantPromptPath)); }
        if (Mode.Equals(TEXT("grammar"), ESearchCase::IgnoreCase))
        {
            if (!GrammarPath.IsEmpty()) { Args.Add(TEXT("--grammar"));    Args.Add(Quote(GrammarPath)); }
        }
        else
        {
            if (!JsonSchemaPath.IsEmpty()) { Args.Add(TEXT("--json-schema")); Args.Add(Quote(JsonSchemaPath)); }
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
    FString GrammarPath, JsonSchemaPath;
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
            UE_LOG(LogIGISDK, Error, TEXT("Unable to chain common parameters; cannot use CiG: %s"), *GetIGIStatusString(Result));
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
                        UE_LOG(LogIGISDK, Error, TEXT("Unable to chain D3D12 parameters; cannot use CiG: %s"), *GetIGIStatusString(Result));
                        GPTInstance = nullptr;
                    }
                }
                else
                {
                    UE_LOG(LogIGISDK, Error, TEXT("Unable to retrieve D3D12 device and command queue from UE; cannot use CiG: %s"), *GetIGIStatusString(Result));
                    GPTInstance = nullptr;
                }
            }
            else
            {
                UE_LOG(LogIGISDK, Error, TEXT("Unable to retrieve RHI instance from UE; cannot use CiG: %s"), *GetIGIStatusString(Result));
                GPTInstance = nullptr;
            }
        }
        else
        {
            UE_LOG(LogIGISDK, Log, TEXT("UE not using D3D12; cannot use CiG: %s"), *GetIGIStatusString(Result));
            GPTInstance = nullptr;
        }

        Result = GPTInterface->createInstance(params, &GPTInstance);
        if (Result != nvigi::kResultOk)
        {
            UE_LOG(LogIGISDK, Fatal, TEXT("Unable to create gpt.ggml.cuda instance: %s"), *GetIGIStatusString(Result));
            GPTInstance = nullptr;
        }

        PythonClient = MakeUnique<FPythonMonitoredSingleShot>();
        PythonClient->ConfigureFromEnv();
    }

    virtual ~Impl()
    {
        PythonClient.Reset();

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

    void WarmUpPython(double TimeoutSec)
    {
        if (!PythonClient.IsValid())
        {
            PythonClient = MakeUnique<FPythonMonitoredSingleShot>();
            PythonClient->ConfigureFromEnv();
        }

        UE_LOG(LogIGISDK, Log, TEXT("[warmup] Starting Python warm-up (timeout=%.1fs)"), TimeoutSec);

        const FString WarmupReq = TEXT("{\"user\":\"__warmup__\"}");
        const FString Resp = PythonClient->RequestSingleShotJSON(WarmupReq, TimeoutSec);

        if (Resp.StartsWith(TEXT("{\"error\"")))
        {
            UE_LOG(LogIGISDK, Warning, TEXT("[warmup] Python warm-up returned: %s"), *Resp);
        }
        else
        {
            UE_LOG(LogIGISDK, Log, TEXT("[warmup] Python warm-up OK"));
        }
    }

    FString Evaluate(const FString& UserPrompt)
    {
        // TODO: think about NVIGI+ACE for this porject
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

        // TODO: Read system/assistant prompt from disk

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

    FString EvaluateStructured(const FString& UserPrompt)
    {
        const FString Escaped = UserPrompt.ReplaceCharWithEscapedChar();
        const FString OneLine = FString::Printf(TEXT("{\"user\":\"%s\"}"), *Escaped);

        return PythonClient->RequestSingleShotJSON(OneLine, /*TimeoutSec=*/30.0);
    }

private:
    // Non-owning ptr
    FIGIModule* IGIModulePtr;

    nvigi::IGeneralPurposeTransformer* GPTInterface{ nullptr };
    nvigi::InferenceInstance* GPTInstance{ nullptr };
    FCriticalSection CS_ACE;

    TUniquePtr<FPythonMonitoredSingleShot> PythonClient;
    FString TempOut;
};

FIGIGPT::FIGIGPT(FIGIModule* IGIModule)
{
    Pimpl = MakePimpl<FIGIGPT::Impl>(IGIModule);
}

FIGIGPT::~FIGIGPT() {}

void FIGIGPT::WarmUpPython(double TimeoutSec)
{
    Pimpl->WarmUpPython(TimeoutSec);
}

FString FIGIGPT::Evaluate(const FString& UserPrompt)
{
    return Pimpl->Evaluate(UserPrompt);
}

FString FIGIGPT::EvaluateStructured(const FString& UserPrompt)
{
    return Pimpl->EvaluateStructured(UserPrompt);
}