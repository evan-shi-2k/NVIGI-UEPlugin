#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ACEWorldActionRegistry.generated.h"

USTRUCT(BlueprintType)
struct FWorldActionEntry {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Intent;   // e.g., "MoveTo"
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> Aliases;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Doc;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> Tags;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ArgHints; // e.g., "location, anim"
};

USTRUCT(BlueprintType)
struct FWorldActionCandidate {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Intent;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Score = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Doc;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ArgHints;
};

UCLASS()
class ACEDIRECTORRUNTIME_API UACEWorldActionRegistry : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    UFUNCTION(BlueprintCallable, Category = "ACE|World")
    void RetrieveTopK(const FString& Query, int32 K, TArray<FWorldActionCandidate>& Out) const;

private:
    TArray<FWorldActionEntry> Entries;

    static TArray<FString> Tokenize(const FString& S);
    static float CosineLike(const TArray<FString>& Q, const TArray<FString>& D);
    static float LexicalBonus(const TArray<FString>& Q, const FWorldActionEntry& E);
    bool LoadJSON();
    static FString JsonPath();
};
