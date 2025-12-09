#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SObjectPropertyEntryBox;
class UCommandRouterComponent;
class UPlannerListener;
class UMicCaptureComponent;

class SDirectorPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDirectorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply OnSendClicked();
    FReply OnPushToTalkClicked();

    void OnActorPicked(const FAssetData& AssetData);
    void AppendLog(const FString& Line);

    FText GetPushToTalkText() const;

    UWorld* GetCurrentWorld() const;
    UCommandRouterComponent* GetRouter() const;
    UMicCaptureComponent* GetMicComponent() const;
    FString GetObjectPath() const;

private:
    TWeakObjectPtr<AActor> TargetActor;

    TSharedPtr<class SEditableTextBox>      PromptBox;
    TSharedPtr<class SMultiLineEditableText> LogBox;
    TSharedPtr<class SButton>               PushToTalkButton;

    bool bIsRecording = false;

    UPlannerListener* Listener = nullptr;
};
