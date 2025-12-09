// SPDX-FileCopyrightText: Copyright (c) 2025
// SPDX-License-Identifier: MIT
//

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MicCaptureComponent.generated.h"

/**
 * UMicCaptureComponent
 *
 * Lightweight wrapper around UE's voice capture interface (IVoiceCapture).
 * - StartCapture(): begins recording from the default mic
 * - StopCapture(): stops recording
 * - GetCapturedAudio(): returns captured audio as float PCM in [-1, 1]
 */
UCLASS(ClassGroup = (ACE), meta = (BlueprintSpawnableComponent))
class ACEDIRECTORRUNTIME_API UMicCaptureComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UMicCaptureComponent();

    /** Begin capturing from the default microphone. */
    UFUNCTION(BlueprintCallable, Category = "ACE|Mic")
    void StartCapture();

    /** Stop capturing from the microphone. */
    UFUNCTION(BlueprintCallable, Category = "ACE|Mic")
    void StopCapture();

    /** Get all captured audio as normalized float PCM in [-1, 1]. */
    UFUNCTION(BlueprintCallable, Category = "ACE|Mic")
    void GetCapturedAudio(TArray<float>& OutPCMFloat) const;

    /** True while the component is actively capturing from the mic. */
    UPROPERTY(BlueprintReadOnly, Category = "ACE|Mic")
    bool bIsCapturing = false;

    /** Requested sample rate for capture (defaults to 16000 Hz). */
    UPROPERTY(BlueprintReadOnly, Category = "ACE|Mic")
    int32 SampleRateHz = 16000;

    /** Number of input channels (usually 1 for mono). */
    UPROPERTY(BlueprintReadOnly, Category = "ACE|Mic")
    int32 NumChannels = 1;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(
        float DeltaTime,
        enum ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

private:
    /** Create and configure the voice capture interface if needed. */
    void InitializeVoiceCapture();

    /** Read any pending data from the capture interface into CapturedAudio. */
    void PollVoiceData();

private:
    /** Underlying UE voice capture interface. */
    TSharedPtr<class IVoiceCapture> VoiceCapture;

    /** Captured audio as normalized float PCM in [-1, 1]. */
    TArray<float> CapturedAudio;

    /** Raw byte buffer used by GetVoiceData. */
    TArray<uint8> VoiceCaptureBuffer;

    /** Protects CapturedAudio for cross-thread access (if any). */
    mutable FCriticalSection CaptureMutex;
};
