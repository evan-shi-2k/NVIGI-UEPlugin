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
 *
 * Designed to be used with UIGIASREvaluateAsync / FIGIASR (Whisper ASR).
 */
UCLASS(ClassGroup = (ACE), meta = (BlueprintSpawnableComponent))
class ACEDIRECTORRUNTIME_API UMicCaptureComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UMicCaptureComponent();

    UFUNCTION(BlueprintCallable, Category = "ACE|Mic")
    void StartCapture();

    UFUNCTION(BlueprintCallable, Category = "ACE|Mic")
    void StopCapture();

    UFUNCTION(BlueprintCallable, Category = "ACE|Mic")
    void GetCapturedAudio(TArray<float>& OutPCMFloat) const;

    UPROPERTY(BlueprintReadOnly, Category = "ACE|Mic")
    bool bIsCapturing = false;

    UPROPERTY(BlueprintReadOnly, Category = "ACE|Mic")
    int32 SampleRateHz = 16000;

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
    void InitializeVoiceCapture();

    void PollVoiceData();

private:
    TSharedPtr<class IVoiceCapture> VoiceCapture;

    TArray<float> CapturedAudio;

    TArray<uint8> VoiceCaptureBuffer;
};
