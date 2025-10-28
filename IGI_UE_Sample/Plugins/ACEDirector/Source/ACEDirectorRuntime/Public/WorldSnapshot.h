#pragma once
#include "CoreMinimal.h"
#include "WorldCognition.h"
#include "WorldSnapshot.generated.h"

UCLASS()
class ACEDIRECTORRUNTIME_API UWorldSnapshot : public UGameInstanceSubsystem {
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable)
	FWorldCognition BuildSnapshot(AActor* Instigator, float Radius = 3000.f) const;
};
