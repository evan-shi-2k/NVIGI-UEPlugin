// SPDX-FileCopyrightText: Copyright (c) 2025
// SPDX-License-Identifier: MIT
//

#include "MicCaptureComponent.h"

#include "Modules/ModuleManager.h"
#include "VoiceModule.h"
#include "Interfaces/VoiceCapture.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogACEMicCapture, Log, All);

static constexpr int32 MIN_AUDIO_SAMPLES = 8000; // minimum samples to send to ASR

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

    FVoiceModule* VoiceModulePtr =
        FModuleManager::GetModulePtr<FVoiceModule>(TEXT("Voice"));
    if (!VoiceModulePtr)
    {
        UE_LOG(LogACEMicCapture, Error,
            TEXT("InitializeVoiceCapture: Voice module is not loaded. ")
            TEXT("Enable the 'Voice' module/plugin and add it as a dependency."));
        return;
    }

    FVoiceModule& VoiceModule = *VoiceModulePtr;

    // Empty device name = use default device
    const FString DeviceName;

    // Use whatever you configured (defaults: 16000 Hz, 1 channel).
    const int32 DesiredSampleRate = (SampleRateHz > 0) ? SampleRateHz : 16000;
    const int32 DesiredChannels = (NumChannels > 0) ? NumChannels : 1;

    VoiceCapture = VoiceModule.CreateVoiceCapture(
        DeviceName,
        DesiredSampleRate,
        DesiredChannels);

    if (!VoiceCapture.IsValid())
    {
        UE_LOG(LogACEMicCapture, Error,
            TEXT("Failed to create voice capture interface (Device=\"%s\", SR=%d, Ch=%d)."),
            *DeviceName, DesiredSampleRate, DesiredChannels);
        return;
    }

    SampleRateHz = DesiredSampleRate;
    NumChannels = DesiredChannels;

    UE_LOG(LogACEMicCapture, Log,
        TEXT("MicCapture: Created voice capture (Device=\"%s\", SR=%d, Ch=%d)"),
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

    {
        FScopeLock Lock(&CaptureMutex);
        CapturedAudio.Reset();
    }
    VoiceCaptureBuffer.Reset();

    if (!VoiceCapture->Start())
    {
        UE_LOG(LogACEMicCapture, Error, TEXT("StartCapture: VoiceCapture->Start() failed."));
        return;
    }

    bIsCapturing = true;

    UE_LOG(LogACEMicCapture, Log,
        TEXT("Microphone capture started (SR=%d, Ch=%d)."),
        SampleRateHz, NumChannels);
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

    // Pull any remaining buffered samples
    PollVoiceData();

    VoiceCapture->Stop();
    bIsCapturing = false;

    UE_LOG(LogACEMicCapture, Log,
        TEXT("Microphone capture stopped. Total Samples: %d"),
        CapturedAudio.Num());
}

void UMicCaptureComponent::GetCapturedAudio(TArray<float>& OutPCMFloat) const
{
    FScopeLock Lock(&CaptureMutex);

    OutPCMFloat = CapturedAudio;

    // Ensure a minimum length by padding with silence (0.0f) if needed.
    if (OutPCMFloat.Num() < MIN_AUDIO_SAMPLES)
    {
        const int32 SamplesNeeded = MIN_AUDIO_SAMPLES - OutPCMFloat.Num();
        UE_LOG(LogACEMicCapture, Warning,
            TEXT("GetCapturedAudio: Audio too short (%d samples). Padding with %d samples of silence."),
            OutPCMFloat.Num(), SamplesNeeded);

        OutPCMFloat.AddZeroed(SamplesNeeded);
    }
}

void UMicCaptureComponent::PollVoiceData()
{
    if (!VoiceCapture.IsValid())
    {
        return;
    }

    uint32 BytesAvailable = 0;
    const EVoiceCaptureState::Type CaptureState =
        VoiceCapture->GetCaptureState(BytesAvailable);

    if (CaptureState != EVoiceCaptureState::Ok || BytesAvailable == 0)
    {
        return;
    }

    VoiceCaptureBuffer.SetNumUninitialized(BytesAvailable);

    uint32 BytesWritten = 0;
    const EVoiceCaptureState::Type GetState =
        VoiceCapture->GetVoiceData(
            VoiceCaptureBuffer.GetData(),
            BytesAvailable,
            BytesWritten);

    if (GetState != EVoiceCaptureState::Ok || BytesWritten == 0)
    {
        return;
    }

    // Interpret as 16-bit signed PCM (interleaved if multi-channel).
    const int32 NumSamples = static_cast<int32>(BytesWritten / sizeof(int16));
    const int16* SampleData =
        reinterpret_cast<const int16*>(VoiceCaptureBuffer.GetData());

    if (NumSamples <= 0 || SampleData == nullptr)
    {
        return;
    }

    FScopeLock Lock(&CaptureMutex);

    const int32 OldNum = CapturedAudio.Num();
    CapturedAudio.Reserve(OldNum + NumSamples);

    for (int32 SampleIdx = 0; SampleIdx < NumSamples; ++SampleIdx)
    {
        // Normalize from int16 [-32768,32767] to float [-1,1]
        const float Normalized =
            static_cast<float>(SampleData[SampleIdx]) / 32768.0f;
        CapturedAudio.Add(Normalized);
    }

    // Optional debug: uncomment to see amplitude statistics per chunk
    
    int16 MinSample = INT16_MAX;
    int16 MaxSample = INT16_MIN;
    int64 SumAbs = 0;
    for (int32 i = 0; i < NumSamples; ++i)
    {
        int16 S = SampleData[i];
        MinSample = FMath::Min(MinSample, S);
        MaxSample = FMath::Max(MaxSample, S);
        SumAbs += FMath::Abs((int32)S);
    }
    const float MeanAbs = float(SumAbs) / FMath::Max(NumSamples, 1);
    UE_LOG(LogACEMicCapture, Verbose,
        TEXT("PollVoiceData: +%d samples, Min=%d, Max=%d, MeanAbs=%.1f"),
        NumSamples, (int32)MinSample, (int32)MaxSample, MeanAbs);
    
}
