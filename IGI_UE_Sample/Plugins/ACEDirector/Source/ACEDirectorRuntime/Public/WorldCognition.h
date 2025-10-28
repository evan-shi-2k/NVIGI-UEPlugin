#pragma once
#include "CoreMinimal.h"
#include "WorldCognition.generated.h"

USTRUCT(BlueprintType)
struct ACEDIRECTORRUNTIME_API FWorldEntity {
	GENERATED_USTRUCT_BODY()
	UPROPERTY() FString Id;
	UPROPERTY() FString Class;
	UPROPERTY() TArray<FString> Tags;
	UPROPERTY() FVector Location;
	UPROPERTY() bool bReachable = true;
	UPROPERTY() TMap<FString, FString> Affordances;
};

USTRUCT(BlueprintType)
struct ACEDIRECTORRUNTIME_API FWorldCognition {
	GENERATED_USTRUCT_BODY()
	UPROPERTY() FVector InstigatorLocation;
	UPROPERTY() FString TimeOfDay;
	UPROPERTY() TArray<FWorldEntity> Nearby;
};