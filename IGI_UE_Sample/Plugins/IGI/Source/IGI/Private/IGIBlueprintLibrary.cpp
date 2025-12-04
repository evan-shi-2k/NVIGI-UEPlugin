// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "IGIBlueprintLibrary.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Modules/ModuleManager.h"

#include "IGIGPT.h"
#include "IGILog.h"

#include "IGIModule.h"
#include "IGIASR.h"

std::atomic<bool> UIGIGPTEvaluateAsync::IsRunning = false;
std::atomic<bool> UIGIASREvaluateAsync::IsRunning = false;

UIGIGPTEvaluateAsync* UIGIGPTEvaluateAsync::GPTEvaluateAsync(const FString& UserPrompt)
{
    if (IsRunning)
    {
        UE_LOG(LogIGISDK, Log, TEXT("%s: GPT is already running! Request was ignored."), ANSI_TO_TCHAR(__FUNCTION__));
        return nullptr;
    }

    UIGIGPTEvaluateAsync* BlueprintNode = NewObject<UIGIGPTEvaluateAsync>();
    BlueprintNode->UserPrompt = UserPrompt;
    BlueprintNode->AddToRoot();

    return BlueprintNode;
}

UIGIGPTEvaluateAsync* UIGIGPTEvaluateAsync::GPTEvaluateStructuredAsync(const FString& UserPrompt)
{
    if (IsRunning)
    {
        UE_LOG(LogIGISDK, Log, TEXT("%s: GPT is already running! Request was ignored."), ANSI_TO_TCHAR(__FUNCTION__));
        return nullptr;
    }

    UIGIGPTEvaluateAsync* Node = NewObject<UIGIGPTEvaluateAsync>();
    Node->UserPrompt = UserPrompt;
    Node->AddToRoot();
    return Node;
}

UIGIGPTEvaluateAsync* UIGIGPTEvaluateAsync::GPTEvaluateStructuredWithGrammarAsync(const FString& UserJSON, const FString& GrammarPath)
{
    if (IsRunning)
    {
        UE_LOG(LogIGISDK, Log, TEXT("%s: GPT is already running! Request was ignored."), ANSI_TO_TCHAR(__FUNCTION__));
        return nullptr;
    }

    UIGIGPTEvaluateAsync* Node = NewObject<UIGIGPTEvaluateAsync>();
    Node->bUseGrammar = true;
    Node->UserPayload = UserJSON;
    Node->GrammarFile = GrammarPath;
    Node->AddToRoot();
    return Node;
}

void UIGIGPTEvaluateAsync::Activate()
{
    const FString TrimmedSystemPrompt = SystemPrompt.TrimStartAndEnd();
    const FString TrimmedUserPrompt = UserPrompt.TrimStartAndEnd();
    const FString TrimmedAssistantPrompt = AssistantPrompt.TrimStartAndEnd();

    if (TrimmedUserPrompt.IsEmpty() && UserPayload.IsEmpty())
    {
        UE_LOG(LogIGISDK, Log, TEXT("%s: GPT called with empty user prompt!"), ANSI_TO_TCHAR(__FUNCTION__));
    }
    else
    {
        IsRunning = true;

        UE_LOG(LogIGISDK, Log, TEXT("%s: sending to GPT: %s"), ANSI_TO_TCHAR(__FUNCTION__), *TrimmedUserPrompt);

        AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, TrimmedSystemPrompt, TrimmedUserPrompt, TrimmedAssistantPrompt]()
        {
            FString result;
            FIGIGPT* GPT{ FModuleManager::GetModuleChecked<FIGIModule>(FName("IGI")).GetGPT()};
            if (GPT != nullptr)
            {
                //result = GPT->Evaluate(TrimmedUserPrompt);
                if (bUseGrammar && !GrammarFile.IsEmpty())
                    result = GPT->EvaluateStructuredWithGrammar(UserPayload, GrammarFile);
                else
                    result = GPT->EvaluateStructured(TrimmedUserPrompt);

                AsyncTask(ENamedThreads::GameThread, [this, result]()
                    {
                        OnResponse.Broadcast(result);
                    });

                UE_LOG(LogIGISDK, Log, TEXT("%s: response from GPT: %s"), ANSI_TO_TCHAR(__FUNCTION__), *result);
            }

            IsRunning = false;
            this->RemoveFromRoot();
        });
    }
}

UIGIASREvaluateAsync* UIGIASREvaluateAsync::ASRTranscribeFloatAsync(
    const TArray<float>& PCMFloat,
    int32 InSampleRateHz,
    int32 InNumChannels,
    bool  bInIsFinal)
{
    if (IsRunning)
    {
        UE_LOG(LogIGISDK, Log,
            TEXT("%s: ASR is already running! Request was ignored."),
            ANSI_TO_TCHAR(__FUNCTION__));
        return nullptr;
    }

    if (PCMFloat.Num() == 0)
    {
        UE_LOG(LogIGISDK, Warning,
            TEXT("%s: ASR called with empty audio buffer!"),
            ANSI_TO_TCHAR(__FUNCTION__));
    }

    UIGIASREvaluateAsync* Node = NewObject<UIGIASREvaluateAsync>();

    Node->AudioPCM = PCMFloat;
    Node->SampleRateHz = InSampleRateHz;
    Node->NumChannels = InNumChannels;
    Node->bIsFinal = bInIsFinal;

    Node->AddToRoot();
    return Node;
}

void UIGIASREvaluateAsync::Activate()
{
    if (AudioPCM.Num() == 0)
    {
        UE_LOG(LogIGISDK, Log,
            TEXT("%s: ASR called with empty audio buffer!"),
            ANSI_TO_TCHAR(__FUNCTION__));

        AsyncTask(ENamedThreads::GameThread, [this]()
            {
                OnResponse.Broadcast(TEXT(""), /*bIsError=*/true);
                this->RemoveFromRoot();
            });

        return;
    }

    IsRunning = true;

    const TArray<float> AudioCopy = AudioPCM;
    const int32         LocalSR = SampleRateHz;
    const int32         LocalCh = NumChannels;
    const bool          bLocalFinal = bIsFinal;

    UE_LOG(LogIGISDK, Log,
        TEXT("%s: sending audio to ASR: Samples=%d, SampleRate=%d, Channels=%d, bIsFinal=%s"),
        ANSI_TO_TCHAR(__FUNCTION__),
        AudioCopy.Num(), LocalSR, LocalCh,
        bLocalFinal ? TEXT("true") : TEXT("false"));

    AsyncTask(ENamedThreads::AnyBackgroundHiPriTask,
        [this, AudioCopy, LocalSR, LocalCh, bLocalFinal]()
        {
            FString Result;
            bool bError = false;

            FIGIASR* ASR = nullptr;
            {
                FIGIModule& IGIModule =
                    FModuleManager::GetModuleChecked<FIGIModule>(FName("IGI"));
                ASR = IGIModule.GetASR();
            }

            if (!ASR)
            {
                bError = true;
                Result = TEXT("[ASR] ASR interface not available (FIGIASR is null)");
            }
            else
            {
                Result = ASR->TranscribePCMFloat(
                    AudioCopy,
                    LocalSR,
                    LocalCh,
                    bLocalFinal);

                if (Result.IsEmpty())
                {
                    bError = true;
                }
            }

            UE_LOG(LogIGISDK, Log,
                TEXT("%s: response from ASR: Error=%s, Text=\"%s\""),
                ANSI_TO_TCHAR(__FUNCTION__),
                bError ? TEXT("true") : TEXT("false"),
                *Result);

            AsyncTask(ENamedThreads::GameThread, [this, Result, bError]()
                {
                    OnResponse.Broadcast(Result, bError);
                });

            IsRunning = false;
            this->RemoveFromRoot();
        });
}