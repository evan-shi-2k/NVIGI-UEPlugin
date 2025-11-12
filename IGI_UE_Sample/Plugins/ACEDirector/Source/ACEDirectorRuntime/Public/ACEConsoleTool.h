#pragma once
#include "CoreMinimal.h"
#include "ACEConsoleTool.generated.h"

UCLASS()
class ACEDIRECTORRUNTIME_API UACEConsoleTool : public UObject {
    GENERATED_BODY()
public:
    // Execute the final commandline via UE console.
    UFUNCTION(BlueprintCallable, Category = "ACE|Console")
    static void Execute(UObject* WorldContext, const FString& CommandLine);
};
