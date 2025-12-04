// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: MIT
//

#include "IGIASR.h"

#include "CoreMinimal.h"
#include "IGIModule.h"
#include "IGILog.h"

#include "nvigi.h"
#include "nvigi_ai.h"
#include "nvigi_asr_whisper.h"
#include "nvigi_stl_helpers.h"
#include "nvigi_struct.h"

#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#pragma warning(push)
#pragma warning(disable : 5257)
#include "nvigi_d3d12.h"
#pragma warning(pop)
#endif

#include <vector>

#include "nvigi_asr_whisper.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

#include <vector>
#include <string>

namespace
{
    constexpr const char* const GGUF_MODEL_WHISPER_GUID = "{5CAD3A03-1272-4D43-9F3D-655417526170}";

    constexpr std::size_t VRAM_BUDGET_MB_RECOMMENDATION = 1024 * 4;
    constexpr std::size_t THREADS_RECOMMENDATION = 1;
    constexpr int32 REQUIRED_SAMPLE_RATE_HZ = 16000;
}

class FIGIASR::Impl
{
public:
    explicit Impl(FIGIModule* InIGIModule)
        : IGIModulePtr(InIGIModule)
    {
        nvigi::Result Result = nvigi::kResultOk;
        
        IGIModulePtr->LoadIGIFeature(nvigi::plugin::asr::ggml::cuda::kId, &AsrInterface, nullptr);
        
        nvigi::ASRWhisperCreationParameters params{};
        
        nvigi::CommonCreationParameters common{};
        auto ConvertedString = StringCast<UTF8CHAR>(*IGIModulePtr->GetModelsPath());
        common.utf8PathToModels = reinterpret_cast<const char*>(ConvertedString.Get());
        common.numThreads = THREADS_RECOMMENDATION;
        common.vramBudgetMB = VRAM_BUDGET_MB_RECOMMENDATION;
        common.modelGUID = GGUF_MODEL_WHISPER_GUID;
        Result = params.chain(common);
        if (Result != nvigi::kResultOk)
        {
            UE_LOG(LogIGISDK, Error, TEXT("[ASR] Unable to chain common parameters; cannot use CiG: %s"), *GetIGIStatusString(Result));
            AsrInterface = nullptr;
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
                        UE_LOG(LogIGISDK, Error, TEXT("[ASR] Unable to chain D3D12 parameters; cannot use CiG: %s"), *GetIGIStatusString(Result));
                        AsrInterface = nullptr;
                    }
                }
                else
                {
                    UE_LOG(LogIGISDK, Error, TEXT("[ASR] Unable to retrieve D3D12 device and command queue from UE; cannot use CiG: %s"), *GetIGIStatusString(Result));
                    AsrInterface = nullptr;
                }
            }
            else
            {
                UE_LOG(LogIGISDK, Error, TEXT("[ASR] Unable to retrieve RHI instance from UE; cannot use CiG: %s"), *GetIGIStatusString(Result));
                AsrInterface = nullptr;
            }
        }
        else
        {
            UE_LOG(LogIGISDK, Log, TEXT("[ASR] UE not using D3D12; cannot use CiG: %s"), *GetIGIStatusString(Result));
            AsrInterface = nullptr;
        }

        Result = AsrInterface->createInstance(params, &AsrInstance);
        if (Result != nvigi::kResultOk)
        {
            UE_LOG(LogIGISDK, Fatal, TEXT("[ASR] Unable to create gpt.ggml.cuda instance: %s"), *GetIGIStatusString(Result));
            AsrInterface = nullptr;
        }

        UE_LOG(LogIGISDK, Log, TEXT("[ASR] FIGIASR initialized"));
    }

    ~Impl()
    {
        if (AsrInstance != nullptr)
        {
            AsrInterface->destroyInstance(AsrInstance);
            AsrInstance = nullptr;
        }

        if (IGIModulePtr)
        {
            IGIModulePtr->UnloadIGIFeature(nvigi::plugin::asr::ggml::cuda::kId, AsrInterface);
            AsrInterface = nullptr;
        }

        IGIModulePtr = nullptr;
    }

    FString TranscribePCM16(
        const TArray<int16>& PCM16,
        int32 SampleRateHz,
        int32 NumChannels,
        bool bIsFinal)
    {
        FScopeLock Lock(&CS);

        if (!AsrInterface || !AsrInstance)
        {
            UE_LOG(LogIGISDK, Warning,
                TEXT("[ASR] TranscribePCM16 called but ASR interface/instance is not initialized."));
            return FString();
        }

        if (PCM16.Num() == 0)
        {
            UE_LOG(LogIGISDK, Warning, TEXT("[ASR] TranscribePCM16 called with empty buffer"));
            return FString();
        }

        if (NumChannels <= 0)
        {
            UE_LOG(LogIGISDK, Warning,
                TEXT("[ASR] TranscribePCM16 called with invalid NumChannels=%d"), NumChannels);
            return FString();
        }

        if (SampleRateHz != REQUIRED_SAMPLE_RATE_HZ)
        {
            UE_LOG(LogIGISDK, Warning,
                TEXT("[ASR] Expected %d Hz mono PCM16, got %d Hz. Please resample before calling TranscribePCM16."),
                REQUIRED_SAMPLE_RATE_HZ, SampleRateHz);
            // We still proceed, but behavior is undefined from NVIGI side.
        }

        TArray<int16> MonoPCM;
        if (NumChannels == 1)
        {
            MonoPCM = PCM16;
        }
        else
        {
            const int32 NumFrames = PCM16.Num() / NumChannels;
            MonoPCM.SetNumUninitialized(NumFrames);

            for (int32 FrameIdx = 0; FrameIdx < NumFrames; ++FrameIdx)
            {
                int64 Sum = 0;
                for (int32 Ch = 0; Ch < NumChannels; ++Ch)
                {
                    const int32 SampleIndex = FrameIdx * NumChannels + Ch;
                    if (PCM16.IsValidIndex(SampleIndex))
                    {
                        Sum += PCM16[SampleIndex];
                    }
                }

                const int32 Avg = (NumChannels > 0)
                    ? static_cast<int32>(Sum / NumChannels)
                    : 0;

                MonoPCM[FrameIdx] = static_cast<int16>(
                    FMath::Clamp(Avg,
                        static_cast<int32>(MIN_int16),
                        static_cast<int32>(MAX_int16)));
            }

            UE_LOG(LogIGISDK, Verbose,
                TEXT("[ASR] Downmixed %d-channel audio to mono (%d frames)."),
                NumChannels, MonoPCM.Num());
        }

        std::vector<int16> AudioSamples;
        AudioSamples.reserve(MonoPCM.Num());
        for (int32 i = 0; i < MonoPCM.Num(); ++i)
        {
            AudioSamples.push_back(MonoPCM[i]);
        }

        nvigi::InferenceDataAudioSTLHelper AudioData(AudioSamples, 1 /* mono */);

        nvigi::InferenceDataSlot AudioSlot{ nvigi::kASRWhisperDataSlotAudio, AudioData };
        nvigi::InferenceDataSlotArray Inputs{ 1, &AudioSlot };

        struct FASRCallbackCtx
        {
            FString TranscribedText;
        } CallbackCtx;

        auto AsrCallback = [](const nvigi::InferenceExecutionContext* ExecCtx,
            nvigi::InferenceExecutionState State,
            void* UserData) -> nvigi::InferenceExecutionState
        {
            if (!UserData || !ExecCtx || !ExecCtx->outputs)
            {
                return nvigi::kInferenceExecutionStateInvalid;
            }

            auto* Ctx = static_cast<FASRCallbackCtx*>(UserData);

            const nvigi::InferenceDataText* TextSlot = nullptr;
            ExecCtx->outputs->findAndValidateSlot(nvigi::kASRWhisperDataSlotTranscribedText, &TextSlot);
            if (TextSlot)
            {
                const std::string Utf8Text = TextSlot->getUTF8Text();
                Ctx->TranscribedText = FString(UTF8_TO_TCHAR(Utf8Text.c_str()));
            }

            // We are not cancelling, just propagate state.
            return State;
        };

        nvigi::ASRWhisperRuntimeParameters RuntimeParams{};

        RuntimeParams.sampling = nvigi::ASRWhisperSamplingStrategy::eBeamSearch;

        nvigi::InferenceExecutionContext Ctx{};
        Ctx.instance = AsrInstance;
        Ctx.callback = AsrCallback;
        Ctx.callbackUserData = &CallbackCtx;
        Ctx.inputs = &Inputs;
        Ctx.runtimeParameters = RuntimeParams;
        Ctx.outputs = nullptr;

        UE_LOG(LogIGISDK, Verbose,
            TEXT("[ASR] TranscribePCM16: Samples=%d, SampleRate=%d, Channels=%d, bIsFinal=%s"),
            PCM16.Num(), SampleRateHz, NumChannels,
            bIsFinal ? TEXT("true") : TEXT("false"));

        const nvigi::Result EvalResult = AsrInstance->evaluate(&Ctx);
        if (EvalResult != nvigi::kResultOk)
        {
            UE_LOG(LogIGISDK, Error,
                TEXT("[ASR] Inference evaluate() failed: %s"),
                *GetIGIStatusString(EvalResult));
            return FString();
        }

        return CallbackCtx.TranscribedText;
    }

    FString TranscribePCMFloat(
        const TArray<float>& PCMFloat,
        int32 SampleRateHz,
        int32 NumChannels,
        bool bIsFinal)
    {
        // Convert float [-1,1] to 16-bit PCM and forward.
        TArray<int16> PCM16;
        PCM16.Reserve(PCMFloat.Num());
        for (float Sample : PCMFloat)
        {
            const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
            const int16 S16 = (int16)FMath::RoundToInt(Clamped * 32767.0f);
            PCM16.Add(S16);
        }

        return TranscribePCM16(PCM16, SampleRateHz, NumChannels, bIsFinal);
    }

private:
    FIGIModule* IGIModulePtr = nullptr;

    nvigi::IAutoSpeechRecognition* AsrInterface{ nullptr };
    nvigi::InferenceInstance* AsrInstance{ nullptr };

    FCriticalSection CS;
};

// ----------------------------------------------------------------------------

FIGIASR::FIGIASR(FIGIModule* InIGIModule)
{
    Pimpl = MakePimpl<FIGIASR::Impl>(InIGIModule);
}

FIGIASR::~FIGIASR() = default;

FString FIGIASR::TranscribePCM16(
    const TArray<int16>& PCM16,
    int32 SampleRateHz,
    int32 NumChannels,
    bool bIsFinal)
{
    return Pimpl->TranscribePCM16(PCM16, SampleRateHz, NumChannels, bIsFinal);
}

FString FIGIASR::TranscribePCMFloat(
    const TArray<float>& PCMFloat,
    int32 SampleRateHz,
    int32 NumChannels,
    bool bIsFinal)
{
    return Pimpl->TranscribePCMFloat(PCMFloat, SampleRateHz, NumChannels, bIsFinal);
}

static void IGI_RunASRSmokeTest()
{
    UE_LOG(LogIGISDK, Log, TEXT("IGI ASR smoke test (Bareback): Starting..."));

    FIGIModule& IGIModule = FModuleManager::LoadModuleChecked<FIGIModule>("IGI");

    // --- VARIABLES TO MANAGE RAW LIFECYCLE ---
    nvigi::IAutoSpeechRecognition* RawInterface = nullptr;
    nvigi::InferenceInstance* RawInstance = nullptr;
    nvigi::Result Result = nvigi::kResultOk;

    // --- 1. LOAD INTERFACE ---
    // NOTE: This will fail/crash if StartupModule has already called GetASR()!
    Result = IGIModule.LoadIGIFeature(nvigi::plugin::asr::ggml::cuda::kId, (nvigi::InferenceInterface**)&RawInterface, nullptr);

    if (Result != nvigi::kResultOk || !RawInterface)
    {
        UE_LOG(LogIGISDK, Error, TEXT("Bareback Test: Failed to load interface. Is ASR already loaded in StartupModule? Result: %s"), *GetIGIStatusString(Result));
        return;
    }

    // --- 2. PREPARE CREATION PARAMETERS ---
    nvigi::ASRWhisperCreationParameters Params{};
    nvigi::CommonCreationParameters CommonParams{};

    // Get Models Path
    auto ModelsUtf8 = StringCast<UTF8CHAR>(*IGIModule.GetModelsPath());
    CommonParams.utf8PathToModels = reinterpret_cast<const char*>(ModelsUtf8.Get());
    CommonParams.numThreads = 1;
    CommonParams.vramBudgetMB = 4096;
    CommonParams.modelGUID = "{5CAD3A03-1272-4D43-9F3D-655417526170}"; // Whisper GUID

    Params.chain(CommonParams);

    // D3D12 Setup (Optional but good for checking GPU integration)
    nvigi::D3D12Parameters D3D12Params{};
    if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::D3D12)
    {
        if (ID3D12DynamicRHI* RHI = static_cast<ID3D12DynamicRHI*>(GDynamicRHI))
        {
            ID3D12Device* D3DDevice = RHI->RHIGetDevice(0);
            ID3D12CommandQueue* Queue = RHI->RHIGetCommandQueue();
            if (D3DDevice && Queue)
            {
                D3D12Params.device = D3DDevice;
                D3D12Params.queue = Queue;
                Params.chain(D3D12Params);
                UE_LOG(LogIGISDK, Log, TEXT("Bareback Test: Chained D3D12 Parameters."));
            }
        }
    }

    // --- 3. CREATE INSTANCE ---
    UE_LOG(LogIGISDK, Log, TEXT("Bareback Test: Creating Instance..."));
    Result = RawInterface->createInstance(Params, &RawInstance);

    if (Result != nvigi::kResultOk || !RawInstance)
    {
        UE_LOG(LogIGISDK, Error, TEXT("Bareback Test: Failed to create instance: %s"), *GetIGIStatusString(Result));
        // Cleanup Interface
        IGIModule.UnloadIGIFeature(nvigi::plugin::asr::ggml::cuda::kId, RawInterface);
        return;
    }

    // --- 4. PREPARE AUDIO (1 Sec Silence) ---
    std::vector<int16> AudioBuffer;
    AudioBuffer.resize(16000, 0); // 16kHz, 1 sec, silence

    nvigi::InferenceDataAudioSTLHelper AudioData(AudioBuffer, 1);
    nvigi::InferenceDataSlot AudioSlot{ nvigi::kASRWhisperDataSlotAudio, AudioData };
    nvigi::InferenceDataSlotArray Inputs{ 1, &AudioSlot };

    // --- 5. DEFINE CALLBACK ---
    struct FTestContext { FString ResultText; };
    FTestContext MyContext;

    auto MyCallback = [](const nvigi::InferenceExecutionContext* Ctx, nvigi::InferenceExecutionState State, void* UserData) -> nvigi::InferenceExecutionState
        {
            if (UserData && Ctx && Ctx->outputs)
            {
                FTestContext* C = (FTestContext*)UserData;
                const nvigi::InferenceDataText* TextSlot = nullptr;
                Ctx->outputs->findAndValidateSlot(nvigi::kASRWhisperDataSlotTranscribedText, &TextSlot);
                if (TextSlot)
                {
                    C->ResultText = FString(UTF8_TO_TCHAR(TextSlot->getUTF8Text()));
                }
            }
            return State;
        };

    nvigi::InferenceExecutionContext ExecCtx{};
    ExecCtx.instance = RawInstance;
    ExecCtx.inputs = &Inputs;
    ExecCtx.callback = MyCallback;
    ExecCtx.callbackUserData = &MyContext;

    // Default runtime params
    nvigi::ASRWhisperRuntimeParameters RuntimeParams{};
    ExecCtx.runtimeParameters = RuntimeParams;

    // --- 6. EVALUATE ---
    UE_LOG(LogIGISDK, Log, TEXT("Bareback Test: Evaluating..."));
    Result = RawInstance->evaluate(&ExecCtx);

    if (Result == nvigi::kResultOk)
    {
        UE_LOG(LogIGISDK, Log, TEXT("Bareback Test SUCCESS. Transcript: '%s'"), *MyContext.ResultText);
    }
    else
    {
        UE_LOG(LogIGISDK, Error, TEXT("Bareback Test FAILED. Result: %s"), *GetIGIStatusString(Result));
    }

    // --- 7. CLEANUP ---
    UE_LOG(LogIGISDK, Log, TEXT("Bareback Test: Cleaning up..."));
    if (RawInstance)
    {
        RawInterface->destroyInstance(RawInstance);
    }
    if (RawInterface)
    {
        IGIModule.UnloadIGIFeature(nvigi::plugin::asr::ggml::cuda::kId, RawInterface);
    }

    UE_LOG(LogIGISDK, Log, TEXT("Bareback Test: Finished."));
}

// Register a console command: "igi.TestASR"
static FAutoConsoleCommand GIGIASRTestCommand(
    TEXT("igi.TestASR"),
    TEXT("Run NVIGI ASR smoke test (1 second of silence at 16kHz)"),
    FConsoleCommandDelegate::CreateStatic(&IGI_RunASRSmokeTest)
);