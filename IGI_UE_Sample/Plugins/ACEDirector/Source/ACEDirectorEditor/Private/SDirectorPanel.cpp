#include "SDirectorPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "PropertyCustomizationHelpers.h"

#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

#include "Modules/ModuleManager.h"

#include "CommandRouterComponent.h"
#include "PlannerListener.h"
#include "MicCaptureComponent.h"
#include "IGIModule.h"
#include "IGIASR.h"

void SDirectorPanel::Construct(const FArguments& InArgs)
{
    if (GEditor)
    {
        if (USelection* Sel = GEditor->GetSelectedActors())
        {
            if (Sel->Num() > 0)
            {
                if (AActor* First = Cast<AActor>(Sel->GetSelectedObject(0)))
                {
                    TargetActor = First;
                }
            }
        }
    }

    ChildSlot
        [
            SNew(SVerticalBox)

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(4)
                [
                    SNew(SHorizontalBox)
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(0, 0, 8, 0)
                        [
                            SNew(STextBlock)
                                .Text(FText::FromString(TEXT("Target Actor (has CommandRouter + MicCapture):")))
                        ]
                        + SHorizontalBox::Slot()
                        .FillWidth(1.f)
                        [
                            SNew(SObjectPropertyEntryBox)
                                .AllowedClass(AActor::StaticClass())
                                .AllowClear(true)
                                .ObjectPath(this, &SDirectorPanel::GetObjectPath)
                                .OnObjectChanged_Lambda([this](const FAssetData& InAsset)
                                    {
                                        OnActorPicked(InAsset);
                                    })
                        ]
                ]

            + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(4)
                [
                    SNew(SHorizontalBox)

                        + SHorizontalBox::Slot()
                        .FillWidth(1.f)
                        [
                            SAssignNew(PromptBox, SEditableTextBox)
                                .HintText(FText::FromString(TEXT("Type or say a directive, e.g. 'MoveTo origin'")))
                                .MinDesiredWidth(400.f)
                        ]

                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .Padding(8, 0, 0, 0)
                        [
                            SNew(SButton)
                                .Text(FText::FromString(TEXT("Send")))
                                .OnClicked(this, &SDirectorPanel::OnSendClicked)
                        ]

                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .Padding(8, 0, 0, 0)
                        [
                            SAssignNew(PushToTalkButton, SButton)
                                .Text(this, &SDirectorPanel::GetPushToTalkText)
                                .OnClicked(this, &SDirectorPanel::OnPushToTalkClicked)
                        ]
                ]

            + SVerticalBox::Slot()
                .FillHeight(1.f)
                .Padding(4)
                [
                    SAssignNew(LogBox, SMultiLineEditableText)
                        .AutoWrapText(true)
                        .IsReadOnly(true)
                ]
        ];

    if (UCommandRouterComponent* Router = GetRouter())
    {
        Listener = NewObject<UPlannerListener>();
        Listener->AddToRoot();
        Listener->Init(Router);
        Listener->OnPlannerTextNative.AddRaw(this, &SDirectorPanel::AppendLog);
    }
}

static FString TimeStamp()
{
    return FDateTime::Now().ToString(TEXT("HH:mm:ss"));
}

void SDirectorPanel::AppendLog(const FString& Line)
{
    if (LogBox.IsValid())
    {
        const FString Msg = FString::Printf(TEXT("[%s] %s\n"), *TimeStamp(), *Line);
        LogBox->InsertTextAtCursor(FText::FromString(Msg));
    }
}

FReply SDirectorPanel::OnSendClicked()
{
    if (!PromptBox.IsValid())
    {
        return FReply::Handled();
    }

    const FString Prompt = PromptBox->GetText().ToString().TrimStartAndEnd();
    if (Prompt.IsEmpty())
    {
        AppendLog(TEXT("Prompt is empty."));
        return FReply::Handled();
    }

    AActor* Target = TargetActor.Get();
    if (!Target)
    {
        AppendLog(TEXT("Pick a target actor first."));
        return FReply::Handled();
    }

    UCommandRouterComponent* Router = GetRouter();
    if (!Router)
    {
        AppendLog(TEXT("Target actor has no CommandRouterComponent."));
        return FReply::Handled();
    }

    Router->RouteFromText(Prompt, Target);
    AppendLog(FString::Printf(TEXT(">> %s"), *Prompt));
    return FReply::Handled();
}

FReply SDirectorPanel::OnPushToTalkClicked()
{
    AActor* Target = TargetActor.Get();
    if (!Target)
    {
        AppendLog(TEXT("[ASR] Pick a target actor first."));
        return FReply::Handled();
    }

    UMicCaptureComponent* Mic = GetMicComponent();
    if (!Mic)
    {
        AppendLog(TEXT("[ASR] Target actor is missing MicCaptureComponent."));
        return FReply::Handled();
    }

    if (!bIsRecording)
    {
        // Start recording
        Mic->StartCapture();
        bIsRecording = true;
        AppendLog(TEXT("[ASR] Recording started..."));
    }
    else
    {
        // Stop recording and run ASR
        Mic->StopCapture();
        bIsRecording = false;
        AppendLog(TEXT("[ASR] Recording stopped. Running transcription..."));

        TArray<float> Audio;
        Mic->GetCapturedAudio(Audio);

        if (Audio.Num() == 0)
        {
            AppendLog(TEXT("[ASR] No audio captured."));
            return FReply::Handled();
        }

        // Call IGI ASR synchronously
        FIGIASR* ASR = nullptr;
        {
            if (FModuleManager::Get().IsModuleLoaded(TEXT("IGI")))
            {
                FIGIModule& IGIModule = FModuleManager::GetModuleChecked<FIGIModule>(TEXT("IGI"));
                ASR = IGIModule.GetASR();
            }
            else
            {
                FIGIModule& IGIModule = FModuleManager::LoadModuleChecked<FIGIModule>(TEXT("IGI"));
                ASR = IGIModule.GetASR();
            }
        }

        if (!ASR)
        {
            AppendLog(TEXT("[ASR] ASR interface not available (FIGIASR is null)."));
            return FReply::Handled();
        }

        const int32 SampleRateHz = 16000;
        const int32 NumChannels = 1;
        const bool bIsFinal = true;

        const FString Transcript =
            ASR->TranscribePCMFloat(Audio, SampleRateHz, NumChannels, bIsFinal);

        if (Transcript.IsEmpty())
        {
            AppendLog(TEXT("[ASR] Empty transcript or ASR error."));
        }
        else
        {
            if (PromptBox.IsValid())
            {
                PromptBox->SetText(FText::FromString(Transcript));
            }

            AppendLog(FString::Printf(TEXT("[ASR] \"%s\""), *Transcript));
        }
    }

    return FReply::Handled();
}

FText SDirectorPanel::GetPushToTalkText() const
{
    return bIsRecording
        ? FText::FromString(TEXT("Stop & Transcribe"))
        : FText::FromString(TEXT("Push to Talk"));
}

UWorld* SDirectorPanel::GetCurrentWorld() const
{
    if (GEditor)
    {
        const FWorldContext* Ctx = &GEditor->GetEditorWorldContext();
        return Ctx ? Ctx->World() : nullptr;
    }
    return nullptr;
}

UCommandRouterComponent* SDirectorPanel::GetRouter() const
{
    if (AActor* A = TargetActor.Get())
    {
        return A->FindComponentByClass<UCommandRouterComponent>();
    }
    return nullptr;
}

UMicCaptureComponent* SDirectorPanel::GetMicComponent() const
{
    if (AActor* A = TargetActor.Get())
    {
        return A->FindComponentByClass<UMicCaptureComponent>();
    }
    return nullptr;
}

void SDirectorPanel::OnActorPicked(const FAssetData& AssetData)
{
    TargetActor = Cast<AActor>(AssetData.GetAsset());
    AppendLog(TargetActor.IsValid()
        ? FString::Printf(TEXT("Target set: %s"), *TargetActor->GetName())
        : TEXT("Target cleared."));

    if (Listener)
    {
        Listener->RemoveFromRoot();
        Listener = nullptr;
    }
    if (UCommandRouterComponent* Router = GetRouter())
    {
        Listener = NewObject<UPlannerListener>();
        Listener->AddToRoot();
        Listener->Init(Router);
        Listener->OnPlannerTextNative.AddRaw(this, &SDirectorPanel::AppendLog);
    }
}

FString SDirectorPanel::GetObjectPath() const
{
    const UObject* Obj = TargetActor.Get();
    return Obj ? Obj->GetPathName() : FString();
}
