#pragma once
#include "CoreMinimal.h"
#include "CommandSchema.generated.h"

USTRUCT(BlueprintType)
struct ACEDIRECTORRUNTIME_API FACECommand {
    GENERATED_USTRUCT_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString intent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TMap<FString, FString> args;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float priority = 0.5f;
};

USTRUCT(BlueprintType)
struct ACEDIRECTORRUNTIME_API FACECommandList {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FACECommand> commands;
};
