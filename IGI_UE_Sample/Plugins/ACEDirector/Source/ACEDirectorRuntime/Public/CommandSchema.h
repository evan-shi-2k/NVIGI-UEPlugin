#pragma once
#include "CoreMinimal.h"
#include "CommandSchema.generated.h"

USTRUCT(BlueprintType)
struct ACEDIRECTORRUNTIME_API FACECommand {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString intent;   // "MoveTo", "Interact", "Say", "Patrol"
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString target;   // "NPC_Scout", "Door_A", "Switch_03"
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TMap<FString, FString> params; // {"location":"X Y Z","anim":"Wave"}
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float priority = 0.5f;
};

USTRUCT(BlueprintType)
struct ACEDIRECTORRUNTIME_API FACECommandList {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FACECommand> commands;
};
