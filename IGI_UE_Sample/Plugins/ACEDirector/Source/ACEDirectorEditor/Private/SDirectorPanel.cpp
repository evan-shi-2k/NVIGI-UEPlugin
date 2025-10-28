#include "SDirectorPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "PropertyCustomizationHelpers.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

#include "CommandRouterComponent.h"
#include "PlannerListener.h"

void SDirectorPanel::Construct(const FArguments& InArgs)
{
    // Default target = current selection (if any)
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

                // Target actor picker
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                        [
                            SNew(STextBlock).Text(FText::FromString(TEXT("Target Actor (has CommandRouter):")))
                        ]
                        + SHorizontalBox::Slot().FillWidth(1.f)
                        [
                            SNew(SObjectPropertyEntryBox)
                                .AllowedClass(AActor::StaticClass())
                                .AllowClear(true)
                                .ObjectPath(this, &SDirectorPanel::GetObjectPath)
                                .OnObjectChanged_Lambda([this](const FAssetData& InAsset) { OnActorPicked(InAsset); })
                        ]
                ]

            // Prompt
            + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.f)
                        [
                            SAssignNew(PromptBox, SEditableTextBox)
                                .HintText(FText::FromString(TEXT("Type a directive, e.g. 'MoveTo origin'")))
                                .MinDesiredWidth(400.f)
                        ]
                        + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
                        [
                            SNew(SButton)
                                .Text(FText::FromString(TEXT("Send")))
                                .OnClicked(this, &SDirectorPanel::OnSendClicked)
                        ]
                ]

            // Log
            + SVerticalBox::Slot().FillHeight(1.f).Padding(4)
                [
                    SAssignNew(LogBox, SMultiLineEditableText)
                        .AutoWrapText(true)
                        .IsReadOnly(true)
                ]
        ];

    // If a target already exists and has router, hook the listener
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
    if (!PromptBox.IsValid()) return FReply::Handled();

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

void SDirectorPanel::OnActorPicked(const FAssetData& AssetData)
{
    TargetActor = Cast<AActor>(AssetData.GetAsset());
    AppendLog(TargetActor.IsValid()
        ? FString::Printf(TEXT("Target set: %s"), *TargetActor->GetName())
        : TEXT("Target cleared."));

    // Rehook listener to new router
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
