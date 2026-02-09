#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class UCommandRouterComponent;
class UPlannerListener;
class UMicCaptureComponent;
class SMultiLineEditableTextBox;

class SDirectorPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDirectorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SDirectorPanel() override;

private:
    AActor* ResolveRuntimeActor() const;

    FReply OnSendClicked();
    FReply OnPushToTalkClicked();
    void OnActorPicked(const FAssetData& AssetData);

    void AppendLog(const FString& Line);
    void AppendGPTDebug(const FString& Line);

    FText GetPushToTalkText() const;

    UCommandRouterComponent* GetRouter() const;
    UMicCaptureComponent* GetMicComponent() const;
    FString GetObjectPath() const;

    bool IsLikelyGPTLog(const FString& Msg, const FName& Category) const;

private:
    TWeakObjectPtr<AActor> TargetActor;

    TSharedPtr<SMultiLineEditableTextBox> PromptBox;
    TSharedPtr<class SButton> PushToTalkButton;
    TSharedPtr<SMultiLineEditableTextBox> LogBox;
    TSharedPtr<SMultiLineEditableTextBox> DebugBox;

    bool bIsRecording = false;

    FString LogBuffer;
    FString DebugBuffer;

    UPlannerListener* Listener = nullptr;

    struct FGPTLogCaptureDevice;
    TSharedPtr<FGPTLogCaptureDevice> GPTLogCapture;
};
