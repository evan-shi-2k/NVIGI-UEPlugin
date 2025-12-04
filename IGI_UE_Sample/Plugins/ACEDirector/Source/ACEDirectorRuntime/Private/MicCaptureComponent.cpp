// SPDX-FileCopyrightText: Copyright (c) 2025
// SPDX-License-Identifier: MIT
//

#include "MicCaptureComponent.h"

#include "Modules/ModuleManager.h"
#include "VoiceModule.h"
#include "Interfaces/VoiceCapture.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogACEMicCapture, Log, All);

UMicCaptureComponent::UMicCaptureComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
}

void UMicCaptureComponent::BeginPlay()
{
    Super::BeginPlay();

    InitializeVoiceCapture();
}

void UMicCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopCapture();

    if (VoiceCapture.IsValid())
    {
        VoiceCapture->Shutdown();
        VoiceCapture.Reset();
    }

    Super::EndPlay(EndPlayReason);
}

void UMicCaptureComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (bIsCapturing && VoiceCapture.IsValid())
    {
        PollVoiceData();
    }
}

void UMicCaptureComponent::InitializeVoiceCapture()
{
    if (VoiceCapture.IsValid())
    {
        return;
    }

    FVoiceModule* VoiceModulePtr = FModuleManager::GetModulePtr<FVoiceModule>(TEXT("Voice"));
    if (!VoiceModulePtr)
    {
        UE_LOG(LogACEMicCapture, Error,
            TEXT("InitializeVoiceCapture: Voice module is not loaded. ")
            TEXT("Enable the 'Voice' module/plugin and add it as a dependency."));
        return;
    }

    FVoiceModule& VoiceModule = *VoiceModulePtr;

    const FString DeviceName;
    VoiceCapture = VoiceModule.CreateVoiceCapture(DeviceName, SampleRateHz, NumChannels);
    if (!VoiceCapture.IsValid())
    {
        UE_LOG(LogACEMicCapture, Error,
            TEXT("Failed to create voice capture interface (Device=\"%s\", SR=%d, Ch=%d)."),
            *DeviceName, SampleRateHz, NumChannels);
        return;
    }

    UE_LOG(LogACEMicCapture, Log,
        TEXT("MicCapture: Initialized voice capture (Device=\"%s\", SampleRate=%d, Channels=%d)."),
        *DeviceName, SampleRateHz, NumChannels);
}

void UMicCaptureComponent::StartCapture()
{
    if (!VoiceCapture.IsValid())
    {
        InitializeVoiceCapture();
    }

    if (!VoiceCapture.IsValid())
    {
        UE_LOG(LogACEMicCapture, Error, TEXT("StartCapture: VoiceCapture is invalid."));
        return;
    }

    if (bIsCapturing)
    {
        UE_LOG(LogACEMicCapture, Verbose, TEXT("StartCapture: already capturing."));
        return;
    }

    // Clear any previous recording.
    CapturedAudio.Reset();
    VoiceCaptureBuffer.Reset();

    if (!VoiceCapture->Start())
    {
        UE_LOG(LogACEMicCapture, Error, TEXT("StartCapture: VoiceCapture->Start() failed."));
        return;
    }

    bIsCapturing = true;
    UE_LOG(LogACEMicCapture, Log, TEXT("Microphone capture started."));
}

void UMicCaptureComponent::StopCapture()
{
    if (!VoiceCapture.IsValid())
    {
        bIsCapturing = false;
        return;
    }

    if (!bIsCapturing)
    {
        return;
    }

    VoiceCapture->Stop();
    bIsCapturing = false;

    UE_LOG(LogACEMicCapture, Log, TEXT("Microphone capture stopped."));
}

void UMicCaptureComponent::GetCapturedAudio(TArray<float>& OutPCMFloat) const
{
    OutPCMFloat = CapturedAudio;
}

void UMicCaptureComponent::PollVoiceData()
{
    if (!VoiceCapture.IsValid())
    {
        return;
    }

    uint32 BytesAvailable = 0;
    EVoiceCaptureState::Type CaptureState = VoiceCapture->GetCaptureState(BytesAvailable);

    if (CaptureState == EVoiceCaptureState::Ok && BytesAvailable > 0)
    {
        VoiceCaptureBuffer.SetNumUninitialized(BytesAvailable);

        uint32 BytesWritten = 0;
        const EVoiceCaptureState::Type GetState =
            VoiceCapture->GetVoiceData(VoiceCaptureBuffer.GetData(), BytesAvailable, BytesWritten);

        if (GetState == EVoiceCaptureState::Ok && BytesWritten > 0)
        {
            const int32 NumSamplesTotal = static_cast<int32>(BytesWritten / sizeof(int16));
            const int32 NumFrames = (NumChannels > 0) ? (NumSamplesTotal / NumChannels) : NumSamplesTotal;

            const int16* SampleData = reinterpret_cast<const int16*>(VoiceCaptureBuffer.GetData());

            const int32 OldNum = CapturedAudio.Num();
            CapturedAudio.Reserve(OldNum + NumFrames);

            for (int32 FrameIdx = 0; FrameIdx < NumFrames; ++FrameIdx)
            {
                int64 Sum = 0;
                for (int32 Ch = 0; Ch < NumChannels; ++Ch)
                {
                    const int32 SampleIndex = FrameIdx * NumChannels + Ch;
                    if (SampleIndex < NumSamplesTotal)
                    {
                        Sum += SampleData[SampleIndex];
                    }
                }

                const int32 AvgSample = (NumChannels > 0)
                    ? static_cast<int32>(Sum / NumChannels)
                    : 0;

                const int32 Clamped = FMath::Clamp(
                    AvgSample,
                    static_cast<int32>(MIN_int16),
                    static_cast<int32>(MAX_int16));

                const float Normalized = static_cast<float>(Clamped) / 32768.0f;

                CapturedAudio.Add(Normalized);
            }

            UE_LOG(LogACEMicCapture, Verbose,
                TEXT("PollVoiceData: Bytes=%u, Written=%u, Frames=%d, TotalSamples=%d"),
                BytesAvailable, BytesWritten, NumFrames, CapturedAudio.Num());
        }
        else if (GetState == EVoiceCaptureState::BufferTooSmall)
        {
            UE_LOG(LogACEMicCapture, Warning,
                TEXT("PollVoiceData: Buffer too small. BytesAvailable=%u"),
                BytesAvailable);
        }
    }
    else if (CaptureState == EVoiceCaptureState::NoData)
    {
        // Nothing yet; normal.
    }
    else
    {
        // NotCapturing or other states – no-op.
    }
}
