#include "ACEConsoleTool.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"

void UACEConsoleTool::Execute(UObject* WorldContext, const FString& CommandLine) {
    if (!WorldContext) return;
    UWorld* World = WorldContext->GetWorld();
    if (!World) return;

    // Prefer engine-level Exec so it also works on dedicated server with authority.
    if (GEngine) {
        GEngine->Exec(World, *CommandLine);
        return;
    }

    // Fallback: first local player controller.
    if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0)) {
        PC->ConsoleCommand(CommandLine, /*bWriteToLog=*/true);
    }
}
