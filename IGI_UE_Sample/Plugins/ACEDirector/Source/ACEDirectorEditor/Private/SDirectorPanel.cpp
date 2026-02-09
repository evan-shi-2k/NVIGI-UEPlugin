#include "SDirectorPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "PropertyCustomizationHelpers.h"

#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

#include "Modules/ModuleManager.h"
#include "Async/Async.h"

#include "CommandRouterComponent.h"
#include "PlannerListener.h"
#include "MicCaptureComponent.h"
#include "IGIModule.h"
#include "IGIASR.h"

static FString TimeStamp()
{
    return FDateTime::Now().ToString(TEXT("%H:%M:%S"));
}

struct SDirectorPanel::FGPTLogCaptureDevice : public FOutputDevice
{
    TWeakPtr<SDirectorPanel> Owner;

    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category) override
    {
        if (!V)
        {
            return;
        }

        const TSharedPtr<SDirectorPanel> Pinned = Owner.Pin();
        if (!Pinned.IsValid())
        {
            return;
        }

        const FString Msg(V);

        if (!Pinned->IsLikelyGPTLog(Msg, Category))
        {
            return;
        }

        AsyncTask(ENamedThreads::GameThread, [WeakOwner = Owner, Category, Msg]()
            {
                if (const TSharedPtr<SDirectorPanel> Panel = WeakOwner.Pin())
                {
                    const FString Line = FString::Printf(TEXT("%s: %s"), *Category.ToString(), *Msg);
                    Panel->AppendGPTDebug(Line);
                }
            });
    }
};

SDirectorPanel::~SDirectorPanel()
{
    if (GPTLogCapture.IsValid() && GLog)
    {
        GLog->RemoveOutputDevice(GPTLogCapture.Get());
    }

    if (Listener)
    {
        Listener->RemoveFromRoot();
        Listener = nullptr;
    }
}

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

                // Target Actor Selector
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
                                .Text(FText::FromString(TEXT("Target Actor:")))
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

            // Input Area
            + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(4, 4, 4, 0)
                [
                    SNew(SVerticalBox)

                        + SVerticalBox::Slot()
                        .AutoHeight()
                        [
                            SNew(SBox)
                                //.HeightOverride(100.0f)
                                [
                                    SNew(SScrollBox)
                                        + SScrollBox::Slot()
                                        [
                                            SAssignNew(PromptBox, SMultiLineEditableTextBox)
                                                .HintText(FText::FromString(TEXT("Type or say a directive...")))
                                                .AutoWrapText(true)
                                        ]
                                ]
                        ]

                    + SVerticalBox::Slot()
                        .AutoHeight()
                        .Padding(0, 4)
                        [
                            SNew(SHorizontalBox)
                                + SHorizontalBox::Slot()
                                .FillWidth(0.5f)
                                [
                                    SNew(SButton)
                                        .HAlign(HAlign_Center)
                                        .Text(FText::FromString(TEXT("Send")))
                                        .OnClicked(this, &SDirectorPanel::OnSendClicked)
                                ]

                                + SHorizontalBox::Slot()
                                .FillWidth(0.5f)
                                .Padding(2, 0, 0, 0)
                                [
                                    SAssignNew(PushToTalkButton, SButton)
                                        .HAlign(HAlign_Center)
                                        .Text(this, &SDirectorPanel::GetPushToTalkText)
                                        .OnClicked(this, &SDirectorPanel::OnPushToTalkClicked)
                                ]
                        ]
                ]

            // Main output area
            + SVerticalBox::Slot()
                .FillHeight(1.f)
                .Padding(4)
                [
                    SNew(SScrollBox)
                        + SScrollBox::Slot()
                        [
                            SAssignNew(LogBox, SMultiLineEditableTextBox)
                                .IsReadOnly(true)
                                .AutoWrapText(true)
                        ]
                ]

            // GPT Debug
            + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(4)
                [
                    SNew(SExpandableArea)
                        .InitiallyCollapsed(true)
                        .AreaTitle(FText::FromString(TEXT("GPT Debug")))
                        .BodyContent()
                        [
                            SNew(SBox)
                                .MaxDesiredHeight(250.0f)
                                [
                                    SNew(SScrollBox)
                                        + SScrollBox::Slot()
                                        [
                                            SAssignNew(DebugBox, SMultiLineEditableTextBox)
                                                .IsReadOnly(true)
                                                .AutoWrapText(true)
                                        ]
                                ]
                        ]
                ]
        ];

    if (UCommandRouterComponent* Router = GetRouter())
    {
        Listener = NewObject<UPlannerListener>();
        Listener->AddToRoot();
        Listener->Init(Router);
        Listener->OnPlannerTextNative.AddRaw(this, &SDirectorPanel::AppendLog);
    }

    GPTLogCapture = MakeShared<FGPTLogCaptureDevice>();
    GPTLogCapture->Owner = SharedThis(this);
    if (GLog)
    {
        GLog->AddOutputDevice(GPTLogCapture.Get());
    }
}

bool SDirectorPanel::IsLikelyGPTLog(const FString& Msg, const FName& Category) const
{
    if (Msg.Contains(TEXT("sending to GPT"), ESearchCase::IgnoreCase)) return true;
    if (Msg.Contains(TEXT("response from GPT"), ESearchCase::IgnoreCase)) return true;
    if (Msg.Contains(TEXT("GPTEvaluate"), ESearchCase::IgnoreCase)) return true;
    if (Msg.Contains(TEXT("Failed to parse plan JSON"), ESearchCase::IgnoreCase)) return true;
    if (Msg.Contains(TEXT("[persist]"), ESearchCase::IgnoreCase)) return true;
    if (Msg.Contains(TEXT("[nim_structured]"), ESearchCase::IgnoreCase)) return true;

    const FString Cat = Category.ToString();
    if (Cat.Contains(TEXT("LogIGISDK"))) return true;
    if (Cat.Contains(TEXT("LogACEPlanner"))) return true;
    if (Cat.Contains(TEXT("LogInteractiveProcess"))) return true;

    return false;
}

void SDirectorPanel::AppendLog(const FString& Line)
{
    const FString Msg = FString::Printf(TEXT("[%s] %s\n"), *TimeStamp(), *Line);
    LogBuffer += Msg;

    if (LogBox.IsValid())
    {
        LogBox->SetText(FText::FromString(LogBuffer));
    }
}

void SDirectorPanel::AppendGPTDebug(const FString& Line)
{
    const FString Msg = FString::Printf(TEXT("[%s] %s\n"), *TimeStamp(), *Line);
    DebugBuffer += Msg;

    if (DebugBox.IsValid())
    {
        DebugBox->SetText(FText::FromString(DebugBuffer));
    }
}

AActor* SDirectorPanel::ResolveRuntimeActor() const
{
    AActor* A = TargetActor.Get();
    if (!A) return nullptr;

#if WITH_EDITOR
    if (GEditor && GEditor->PlayWorld)
    {
        if (AActor* PIE = EditorUtilities::GetSimWorldCounterpartActor(A))
        {
            return PIE;
        }
    }
#endif

    return A;
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

#if WITH_EDITOR
    if (!GEditor || !GEditor->PlayWorld)
    {
        AppendLog(TEXT("Start PIE / Simulate first (router needs a GameInstance)."));
        return FReply::Handled();
    }
#endif

    AActor* RuntimeTarget = ResolveRuntimeActor();
    if (!RuntimeTarget)
    {
        AppendLog(TEXT("Pick a target actor first."));
        return FReply::Handled();
    }

    UCommandRouterComponent* Router = RuntimeTarget->FindComponentByClass<UCommandRouterComponent>();
    if (!Router)
    {
        AppendLog(TEXT("Target actor has no CommandRouterComponent (in PIE world)."));
        return FReply::Handled();
    }

    Router->RouteFromText(Prompt, RuntimeTarget);
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
        Mic->StartCapture();
        bIsRecording = true;
        AppendLog(TEXT("[ASR] Recording started..."));
    }
    else
    {
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

        const FString Transcript = ASR->TranscribePCMFloat(Audio, SampleRateHz, NumChannels, bIsFinal);

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

UCommandRouterComponent* SDirectorPanel::GetRouter() const
{
    if (AActor* A = ResolveRuntimeActor())
    {
        return A->FindComponentByClass<UCommandRouterComponent>();
    }
    return nullptr;
}

UMicCaptureComponent* SDirectorPanel::GetMicComponent() const
{
    if (AActor* A = ResolveRuntimeActor())
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