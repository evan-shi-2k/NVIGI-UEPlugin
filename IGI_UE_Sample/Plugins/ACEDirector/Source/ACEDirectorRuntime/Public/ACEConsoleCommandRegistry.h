#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ACEConsoleCommandRegistry.generated.h"

USTRUCT(BlueprintType)
struct FConsoleCommandEntry {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Name;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> Aliases;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Doc;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> Tags;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ArgNames;
};

USTRUCT(BlueprintType)
struct FConsoleCandidate {
    GENERATED_USTRUCT_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Name;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ArgNames;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Doc;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> Aliases;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> Tags;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Score = 0.f;
};

UCLASS()
class ACEDIRECTORRUNTIME_API UACEConsoleCommandRegistry : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    UFUNCTION(BlueprintCallable, Category = "ACE|Console")
    void RetrieveTopK(const FString& Query, int32 K, TArray<FConsoleCandidate>& Out) const;

    UFUNCTION(BlueprintCallable, Category = "ACE|Console")
    const TArray<FConsoleCommandEntry>& GetAll() const { return Entries; }

private:
    TArray<FConsoleCommandEntry> Entries;

    static TArray<FString> Tokenize(const FString& S);
    static float LexicalBonus(const TArray<FString>& Q, const FConsoleCommandEntry& E);
    static float CosineLike(const TArray<FString>& Q, const TArray<FString>& D);
    bool LoadJSON();
};
