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

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Http.h"

#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

namespace
{
    constexpr const char* const GGUF_MODEL_MINITRON{ "{8E31808B-C182-4016-9ED8-64804FF5B40D}" };

    constexpr std::size_t VRAM_BUDGET_RECOMMENDATION{ 1024 * 12 };
    constexpr std::size_t THREAD_NUM_RECOMMENDATION{ 1 }; // Recommended number of threads for CiG
    constexpr std::size_t CONTEXT_SIZE_RECOMMENDATION{ 4096 };
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
    }

    virtual ~Impl()
    {
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

    FString Evaluate(const FString& UserPrompt)
    {
        FScopeLock Lock(&CS);

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
        const FString BaseUrl = FPlatformMisc::GetEnvironmentVariable(TEXT("NIM_BASE_URL")).IsEmpty()
            ? TEXT("http://127.0.0.1:8000/v1")
            : FPlatformMisc::GetEnvironmentVariable(TEXT("NIM_BASE_URL"));

        const FString ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("NIM_API_KEY"));
        const FString Model = FPlatformMisc::GetEnvironmentVariable(TEXT("NIM_MODEL_NAME")).IsEmpty()
            ? TEXT("meta/llama-3.2-3b-instruct")
            : FPlatformMisc::GetEnvironmentVariable(TEXT("NIM_MODEL_NAME"));

        const FString Mode = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_MODE")).IsEmpty()
            ? TEXT("grammar") : FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_MODE"));

        const FString Script = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_SCRIPT_PATH")).IsEmpty()
            ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("nim_structured.py")))
            : FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_SCRIPT_PATH"));

        const FString SysPath = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_SYSTEM_PROMPT_PATH")).IsEmpty()
            ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("system_prompt.txt")))
            : FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_SYSTEM_PROMPT_PATH"));

        const FString AsstPath = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_ASSISTANT_PROMPT_PATH"));
        const FString GramPath = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_GRAMMAR_PATH")).IsEmpty()
            ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE"), TEXT("command_schema.ebnf")))
            : FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_GRAMMAR_PATH"));

        const FString JsonPath = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_NIM_JSON_PATH"));

        FString PythonExe = FPlatformMisc::GetEnvironmentVariable(TEXT("IGI_PYTHON_EXE"));
        if (PythonExe.IsEmpty()) PythonExe = DefaultPythonExe();
        if (!FPaths::FileExists(PythonExe))
        {
            UE_LOG(LogIGISDK, Warning, TEXT("Python not found at %s; using 'python' from PATH"), *PythonExe);
            PythonExe = TEXT("python");
        }

        TArray<FString> Args;
        Args.Add(Quote(Script));
        Args.Add(TEXT("--base_url ") + Quote(BaseUrl));
        if (!ApiKey.IsEmpty()) Args.Add(TEXT("--api_key ") + Quote(ApiKey));
        Args.Add(TEXT("--model ") + Quote(Model));
        Args.Add(TEXT("--mode ") + Quote(Mode));
        Args.Add(TEXT("--system_prompt_path ") + Quote(SysPath));
        if (!AsstPath.IsEmpty()) Args.Add(TEXT("--assistant_prompt_path ") + Quote(AsstPath));

        if (Mode.Equals(TEXT("grammar"), ESearchCase::IgnoreCase))
            Args.Add(TEXT("--grammar_path ") + Quote(GramPath));
        else
            Args.Add(TEXT("--json_path ") + Quote(JsonPath));

        Args.Add(TEXT("--user ") + Quote(UserPrompt));

        const FString ParamLine = FString::Join(Args, TEXT(" "));
        FString Out, Err;
        int32 Code = -1;

        UE_LOG(LogIGISDK, Log, TEXT("Structured: %s %s"), *PythonExe, *ParamLine);
        const bool bLaunched = FPlatformProcess::ExecProcess(*PythonExe, *ParamLine, &Code, &Out, &Err);
        if (!bLaunched)
        {
            UE_LOG(LogIGISDK, Error, TEXT("Failed to launch Python process"));
            return TEXT("{\"error\":\"python_launch_failed\"}");
        }
        if (Code != 0)
        {
            UE_LOG(LogIGISDK, Warning, TEXT("Structured runner exit %d. stderr: %s"), Code, *Err);
            return Out.IsEmpty()
                ? FString::Printf(TEXT("{\"error\":\"runner_failed\",\"detail\":%s}"), *Quote(Err))
                : Out;
        }
        return Out;
    }

private:
    FCriticalSection CS;

    // Non-owning ptr
    FIGIModule* IGIModulePtr;

    nvigi::IGeneralPurposeTransformer* GPTInterface{ nullptr };
    nvigi::InferenceInstance* GPTInstance{ nullptr };
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

FString FIGIGPT::EvaluateStructured(const FString& UserPrompt)
{
    return Pimpl->EvaluateStructured(UserPrompt);
}