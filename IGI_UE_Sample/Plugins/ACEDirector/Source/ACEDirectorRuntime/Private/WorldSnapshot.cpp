#include "WorldSnapshot.h"

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "DrawDebugHelpers.h"

FWorldCognition UWorldSnapshot::BuildSnapshot(AActor* Instigator, float Radius) const {
    FWorldCognition Out;
    if (!Instigator) return Out;
    UWorld* World = Instigator->GetWorld();
    Out.InstigatorLocation = Instigator->GetActorLocation();

    const float Hours = World->GetTimeSeconds() / 3600.f;
    Out.TimeOfDay = (FMath::Fmod(Hours, 24.f) < 12.f) ? TEXT("Morning") : TEXT("Evening");

    TArray<AActor*> Candidates;
    for (TActorIterator<AActor> It(World); It; ++It) {
        AActor* A = *It;
        if (!IsValid(A) || A == Instigator) continue;
        if (FVector::DistSquared(A->GetActorLocation(), Out.InstigatorLocation) > Radius * Radius) continue;

        FWorldEntity E;
        E.Id = A->GetFName().ToString();
        E.Class = A->GetClass()->GetName();

        for (const FName& T : A->Tags) { E.Tags.Add(T.ToString()); }

        E.Location = A->GetActorLocation();

        FHitResult Hit;
        const bool bLoS = !World->LineTraceSingleByChannel(Hit, Out.InstigatorLocation, E.Location, ECC_Visibility);
        E.bReachable = bLoS; 

        if (A->ActorHasTag(TEXT("Switch"))) { E.Affordances.Add(TEXT("Interact"), TEXT("Toggle")); }
        if (A->ActorHasTag(TEXT("NPC"))) { E.Affordances.Add(TEXT("Talk"), TEXT("Yes")); }

        Out.Nearby.Add(MoveTemp(E));
        if (Out.Nearby.Num() >= 20) break;
    }
    return Out;
}
