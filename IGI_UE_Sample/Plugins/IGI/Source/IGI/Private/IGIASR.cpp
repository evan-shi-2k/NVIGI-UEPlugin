// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: MIT
//

#include "IGIASR.h"

#include "CoreMinimal.h"
#include "IGIModule.h"
#include "IGILog.h"

#include "nvigi.h"
#include "nvigi_ai.h"
// TODO: include the proper ASR header when you enable the plugin, e.g.:
// #include "nvigi_asr.h"

#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#pragma warning(push)
#pragma warning(disable : 5257)
#include "nvigi_d3d12.h"
#pragma warning(pop)
#endif

namespace
{
    // You can tune these once you know the ASR model you want.
    constexpr std::size_t VRAM_BUDGET_MB_RECOMMENDATION = 1024 * 4;  // smaller than GPT
    constexpr std::size_t THREADS_RECOMMENDATION = 1;

    // TODO: replace with actual GUID for your Whisper ASR GGUF
    constexpr const char* const GGUF_MODEL_WHISPER_GUID = "{00000000-0000-0000-0000-000000000000}";
}

class FIGIASR::Impl
{
public:
    explicit Impl(FIGIModule* InIGIModule)
        : IGIModulePtr(InIGIModule)
    {
        using namespace nvigi;

        if (!IGIModulePtr)
        {
            UE_LOG(LogIGISDK, Error, TEXT("[ASR] FIGIASR constructed with null IGIModulePtr"));
            return;
        }

        Result NvResult = kResultOk;

        // TODO: when you enable ASR plugin, replace with the real PluginID:
        // e.g. nvigi::plugin::asr::ggml::cuda::kId
        PluginID AsrPluginId{};
        FMemory::Memset(&AsrPluginId, 0, sizeof(AsrPluginId));

        NvResult = IGIModulePtr->LoadIGIFeature(AsrPluginId, &AsrInterface, nullptr);
        if (NvResult != kResultOk)
        {
            UE_LOG(LogIGISDK, Error, TEXT("[ASR] Failed to load ASR feature: %s"), *GetIGIStatusString(NvResult));
            AsrInterface = nullptr;
            return;
        }

        // --- Creation parameters (skeleton; you will replace with ASR-specific types) ---

        // Many NVIGI interfaces share a "CommonCreationParameters" pattern.
        // Pseudo-code, replace with the real ASRCreationParameters when available.
        //
        // nvigi::ASRCreationParameters params{};
        // nvigi::CommonCreationParameters common{};
        //
        // auto ConvertedString = StringCast<UTF8CHAR>(*IGIModulePtr->GetModelsPath());
        // common.utf8PathToModels = reinterpret_cast<const char*>(ConvertedString.Get());
        // common.numThreads       = THREADS_RECOMMENDATION;
        // common.vramBudgetMB     = VRAM_BUDGET_MB_RECOMMENDATION;
        // common.modelGUID        = GGUF_MODEL_WHISPER_GUID;
        // NvResult = params.chain(common);

#if PLATFORM_WINDOWS
    // Optional D3D12 binding (similar to FIGIGPT). If the ASR plugin supports
    // GPU offload via nvigi::D3D12Parameters, you can chain it here.
        if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::D3D12)
        {
            ID3D12DynamicRHI* RHI = static_cast<ID3D12DynamicRHI*>(GDynamicRHI);
            if (RHI)
            {
                ID3D12CommandQueue* CmdQ = RHI->RHIGetCommandQueue();
                constexpr uint32 RHI_DEVICE_INDEX = 0u;
                ID3D12Device* D3D12Device = RHI->RHIGetDevice(RHI_DEVICE_INDEX);

                if (CmdQ && D3D12Device)
                {
                    // nvigi::D3D12Parameters d3d12Params{};
                    // d3d12Params.device = D3D12Device;
                    // d3d12Params.queue  = CmdQ;
                    // NvResult = params.chain(d3d12Params);
                }
                else
                {
                    UE_LOG(LogIGISDK, Warning, TEXT("[ASR] Could not get D3D12 device/queue; ASR will run on CPU only."));
                }
            }
        }
#endif

        // TODO: actually create the ASR instance once you have the ASR interface type.
        //
        // NvResult = AsrInterface->createInstance(params, &AsrInstance);
        // if (NvResult != kResultOk)
        // {
        //     UE_LOG(LogIGISDK, Error, TEXT("[ASR] Unable to create ASR instance: %s"), *GetIGIStatusString(NvResult));
        //     AsrInstance = nullptr;
        // }

        UE_LOG(LogIGISDK, Log, TEXT("[ASR] FIGIASR initialized (stub, no NVIGI instance yet)."));
    }

    ~Impl()
    {
        using namespace nvigi;

        if (AsrInstance && AsrInterface)
        {
            // TODO: uncomment when you wire real ASR:
            // AsrInterface->destroyInstance(AsrInstance);
            AsrInstance = nullptr;
        }

        if (IGIModulePtr && AsrInterface)
        {
            // TODO: use the same PluginID you used in ctor:
            PluginID AsrPluginId{};
            FMemory::Memset(&AsrPluginId, 0, sizeof(AsrPluginId));

            IGIModulePtr->UnloadIGIFeature(AsrPluginId, AsrInterface);
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

        if (PCM16.Num() == 0)
        {
            UE_LOG(LogIGISDK, Warning, TEXT("[ASR] TranscribePCM16 called with empty buffer"));
            return FString();
        }

        // Stub implementation for now; just log stats and return placeholder.
        // You’ll drop in NVIGI inference calls here.
        UE_LOG(LogIGISDK, Verbose,
            TEXT("[ASR] TranscribePCM16: Samples=%d, SampleRate=%d, Channels=%d, bIsFinal=%s"),
            PCM16.Num(), SampleRateHz, NumChannels, bIsFinal ? TEXT("true") : TEXT("false"));

        // TODO: Build nvigi::InferenceDataSlotArray with audio buffer and call evaluateAsync / evaluateSync,
        // similar to FIGIGPT::Evaluate, then extract the output text.

        return TEXT("[ASR stub] Transcription not yet implemented");
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

    // NVIGI state – currently unused until you wire the real ASR API.
    nvigi::InferenceInterface* AsrInterface = nullptr;
    nvigi::InferenceInstance* AsrInstance = nullptr;

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
