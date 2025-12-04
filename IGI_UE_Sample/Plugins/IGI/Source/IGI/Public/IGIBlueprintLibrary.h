// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include <atomic>
#include "IGIBlueprintLibrary.generated.h"

// ---------------------- GPT async node ----------------------

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FIGIGPTEvaluateAsyncOutputPin, FString, Response);

UCLASS(BlueprintType, meta = (ExposedAsyncProxy = AsyncAction))
class IGI_API UIGIGPTEvaluateAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()
public:

    UFUNCTION(BlueprintCallable, Category = "IGI|GPT", meta = (DisplayName = "Send text to GPT (Async)", BlueprintInternalUseOnly = "true"))
    static UIGIGPTEvaluateAsync* GPTEvaluateAsync(const FString& UserPrompt);

    UFUNCTION(BlueprintCallable, Category = "IGI|GPT", meta = (DisplayName = "Send text to GPT (Structured, Async)", BlueprintInternalUseOnly = "true"))
    static UIGIGPTEvaluateAsync* GPTEvaluateStructuredAsync(const FString& UserPrompt);

    UFUNCTION(BlueprintCallable, Category = "IGI|GPT", meta = (DisplayName = "Send text to GPT (Structured + Grammar)", BlueprintInternalUseOnly = "true"))
    static UIGIGPTEvaluateAsync* GPTEvaluateStructuredWithGrammarAsync(const FString& UserJSON, const FString& GrammarPath);

    void Start() { Activate(); }

    UPROPERTY(BlueprintAssignable)
    FIGIGPTEvaluateAsyncOutputPin OnResponse;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|GPT", meta = (BBlueprintInternalUseOnly = "true"))
    FString SystemPrompt;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|GPT", meta = (BBlueprintInternalUseOnly = "true"))
    FString UserPrompt;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|GPT", meta = (BBlueprintInternalUseOnly = "true"))
    FString AssistantPrompt;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|GPT", meta = (BBlueprintInternalUseOnly = "true"))
    FString SchemaJSON;

    UPROPERTY() bool bUseGrammar = false;
    UPROPERTY() FString UserPayload;
    UPROPERTY() FString GrammarFile;

protected:
    virtual void Activate() override;

    static std::atomic<bool> IsRunning;
};

// ---------------------- ASR async node ----------------------

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FIGIASREvaluateAsyncOutputPin,
    FString, Transcript,
    bool, bIsError);

UCLASS(BlueprintType, meta = (ExposedAsyncProxy = AsyncAction))
class IGI_API UIGIASREvaluateAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category = "IGI|ASR", meta = (DisplayName = "Send voice to ASR (Async)", BlueprintInternalUseOnly = "true"))
    static UIGIASREvaluateAsync* ASRTranscribeFloatAsync(
        const TArray<float>& PCMFloat,
        int32 InSampleRateHz,
        int32 InNumChannels,
        bool  bInIsFinal);

    void Start() { Activate(); }

    UPROPERTY(BlueprintAssignable)
    FIGIASREvaluateAsyncOutputPin OnResponse;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|ASR", meta = (BBlueprintInternalUseOnly = "true"))
    TArray<float> AudioPCM;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|ASR", meta = (BBlueprintInternalUseOnly = "true"))
    int32 SampleRateHz = 16000;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|ASR", meta = (BBlueprintInternalUseOnly = "true"))
    int32 NumChannels = 1;

    UPROPERTY(BlueprintReadOnly, Category = "IGI|ASR", meta = (BBlueprintInternalUseOnly = "true"))
    bool bIsFinal = true;

protected:
    virtual void Activate() override;

    static std::atomic<bool> IsRunning;
};
