// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include <atomic>
#include "IGIBlueprintLibrary.generated.h"

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

    UFUNCTION(BlueprintCallable, meta = (WorldContext = "IGI|GPT", DisplayName = "Send text to GPT (Structured + Grammar)"), Category = "IGI|GPT")
    static class UIGIGPTEvaluateAsync* GPTEvaluateStructuredWithGrammarAsync(const FString& UserJSON, const FString& GrammarPath);

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
