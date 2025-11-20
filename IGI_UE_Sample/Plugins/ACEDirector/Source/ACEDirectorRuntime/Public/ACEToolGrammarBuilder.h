#pragma once
#include "CoreMinimal.h"
#include "ACEToolGrammarBuilder.generated.h"

UCLASS()
class ACEDIRECTORRUNTIME_API UACEToolGrammarBuilder : public UObject {
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category = "ACE|Grammar")
    static FString JsonEscape(const FString& In);

    UFUNCTION(BlueprintCallable, Category = "ACE|Grammar")
    static FString BuildPerQueryGrammar(const TArray<FString>& WorldIntents, const TArray<FString>& ConsoleNames);

    UFUNCTION(BlueprintCallable, Category = "ACE|Grammar")
    static FString WriteTempGrammarFile(const FString& Grammar);
};
