#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "CommandSchema.h"
#include "ACEConsoleCommandRegistry.h"
#include "ACEWorldActionRegistry.h"
#include "CommandRouterComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlannerText, const FString&, VisibleText);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlannerJSON, const FACECommandList&, Plan);

DECLARE_DYNAMIC_DELEGATE_TwoParams(FACEActionHandler, const FACECommand&, Command, AActor*, Instigator);

USTRUCT(BlueprintType)
struct ACEDIRECTORRUNTIME_API FRegisteredAction {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString IntentName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FACEActionHandler Handler;
};

UCLASS(ClassGroup = (ACE), meta = (BlueprintSpawnableComponent))
class ACEDIRECTORRUNTIME_API UCommandRouterComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UCommandRouterComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE") FString SystemPrompt;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE") FString AssistantPreamble;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE") int32 MaxTokens = 200;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE|Schema") FString JSONSchemaOverride;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE|Registry") TArray<FRegisteredAction> Actions;

    UPROPERTY(BlueprintAssignable, Category = "ACE|Events") FOnPlannerText OnPlannerText;
    UPROPERTY(BlueprintAssignable, Category = "ACE|Events") FOnPlannerJSON OnPlannerJSON;

    UFUNCTION(BlueprintCallable, Category = "ACE")
    void RouteFromText(const FString& UserDirective, AActor* Instigator);

    UFUNCTION(BlueprintCallable, Category = "ACE|Router")
    void RegisterAction(const FString& IntentName, const FACEActionHandler& Handler);

    UFUNCTION(BlueprintCallable, Category = "ACE|Router")
    void UnregisterAction(const FString& IntentName);

private:
    UPROPERTY() TWeakObjectPtr<AActor> PendingInstigator;

    FString BuildToolChooserUserJSON(const FString& UserDirective, const TArray<struct FConsoleCandidate>& Cands) const;

    UFUNCTION()
    void HandleGPTResponse(FString Out);

    bool TryParsePlan(const FString& JSON, FACECommandList& OutPlan) const;
    
    void ExecutePlan(const FACECommandList& Plan, AActor* Instigator);
};
