#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SObjectPropertyEntryBox;
class UCommandRouterComponent;
class UPlannerListener;

class SDirectorPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDirectorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    // UI callbacks
    FReply OnSendClicked();
    void OnActorPicked(const FAssetData& AssetData);
    void AppendLog(const FString& Line);

    // Helpers
    UWorld* GetCurrentWorld() const;
    UCommandRouterComponent* GetRouter() const;
    FString GetObjectPath() const;

private:
    TWeakObjectPtr<AActor> TargetActor;
    TSharedPtr<class SEditableTextBox> PromptBox;
    TSharedPtr<class SMultiLineEditableText> LogBox;

    UPlannerListener* Listener = nullptr;
};
