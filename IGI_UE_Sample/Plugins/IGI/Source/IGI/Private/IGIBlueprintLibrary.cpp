// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "IGIBlueprintLibrary.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Modules/ModuleManager.h"

#include "IGIGPT.h"
#include "IGILog.h"

//static std::atomic<bool> GDidPythonWarmup{ false };
std::atomic<bool> UIGIGPTEvaluateAsync::IsRunning = false;

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
                //if (!GDidPythonWarmup.exchange(true))
                //{
                //    UE_LOG(LogIGISDK, Log, TEXT("[IGI] Performing one-time Python warmup before first request"));
                //    GPT->WarmUpPython(/*TimeoutSec=*/20.0);
                //}

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
