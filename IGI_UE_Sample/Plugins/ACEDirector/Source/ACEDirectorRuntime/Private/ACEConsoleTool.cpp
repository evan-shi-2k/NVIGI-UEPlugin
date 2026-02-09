#include "ACEConsoleTool.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"
#include "Async/Async.h"

static void ExecOnGameThread(UObject* WorldContext, const FString CommandLine)
{
    if (!WorldContext) return;
    UWorld* World = WorldContext->GetWorld();
    if (!World) return;

    // Prefer PlayerController route
    if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
    {
        PC->ConsoleCommand(CommandLine, /*bWriteToLog=*/true);
        return;
    }

    if (GEngine)
    {
        GEngine->Exec(World, *CommandLine);
    }
}

void UACEConsoleTool::Execute(UObject* WorldContext, const FString& CommandLine)
{
    if (IsInGameThread())
    {
        ExecOnGameThread(WorldContext, CommandLine);
    }
    else
    {
        TWeakObjectPtr<UObject> WeakCtx(WorldContext);
        AsyncTask(ENamedThreads::GameThread, [WeakCtx, CommandLine]()
        {
            ExecOnGameThread(WeakCtx.Get(), CommandLine);
        });
    }
}
