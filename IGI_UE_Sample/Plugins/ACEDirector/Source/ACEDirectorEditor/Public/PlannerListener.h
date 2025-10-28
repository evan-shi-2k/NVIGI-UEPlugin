#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PlannerListener.generated.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FAceTextDelegate, const FString& /*Text*/);

class UCommandRouterComponent;

UCLASS()
class ACEDIRECTOREDITOR_API UPlannerListener : public UObject
{
    GENERATED_BODY()
public:
    void Init(UCommandRouterComponent* InRouter);

    FAceTextDelegate OnPlannerTextNative;

    UFUNCTION()
    void HandlePlannerText(const FString& VisibleText);

private:
    UPROPERTY() TObjectPtr<UCommandRouterComponent> Router;
};
