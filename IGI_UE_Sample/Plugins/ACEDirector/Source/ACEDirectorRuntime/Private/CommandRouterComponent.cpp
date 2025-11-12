#include "CommandRouterComponent.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/DefaultValueHelper.h"

#include "Engine/World.h"
#include "Kismet/KismetStringLibrary.h"

#include "IGIBlueprintLibrary.h"
#include "ACEToolGrammarBuilder.h"
#include "ACEConsoleTool.h"

DEFINE_LOG_CATEGORY_STATIC(LogACEPlanner, Log, All);

static FString JsonValueToCompactString(const TSharedPtr<FJsonValue>& V)
{
    if (!V.IsValid() || V->IsNull()) return TEXT("");
    switch (V->Type)
    {
    case EJson::String: return V->AsString();
    case EJson::Number: return FString::Printf(TEXT("%g"), V->AsNumber());
    case EJson::Boolean: return V->AsBool() ? TEXT("true") : TEXT("false");
    case EJson::Array:
    case EJson::Object:
    default:
    {
        FString Out;
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(V, TEXT(""), Writer);
        Writer->Close();
        return Out;
    }
    }
}

UCommandRouterComponent::UCommandRouterComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UCommandRouterComponent::RouteFromText(const FString& UserDirective, AActor* Instigator)
{
    PendingInstigator = Instigator;

    // 1) Retrieve top-K sets
    TArray<FConsoleCandidate> ConsoleCands;
    if (UACEConsoleCommandRegistry* RC = GetWorld()->GetGameInstance()->GetSubsystem<UACEConsoleCommandRegistry>())
        RC->RetrieveTopK(UserDirective, /*K=*/5, ConsoleCands);

    TArray<FWorldActionCandidate> WorldCands;
    if (UACEWorldActionRegistry* RW = GetWorld()->GetGameInstance()->GetSubsystem<UACEWorldActionRegistry>())
        RW->RetrieveTopK(UserDirective, /*K=*/7, WorldCands);

    //UIGIGPTEvaluateAsync* Node = UIGIGPTEvaluateAsync::GPTEvaluateAsync(UserDirective);
    UIGIGPTEvaluateAsync* Node = UIGIGPTEvaluateAsync::GPTEvaluateStructuredAsync(UserDirective);
    if (!Node)
    {
        UE_LOG(LogACEPlanner, Warning, TEXT("GPTEvaluateAsync returned null"));
        return;
    }

    Node->OnResponse.AddDynamic(this, &UCommandRouterComponent::HandleGPTResponse);

    Node->Start();
}

void UCommandRouterComponent::RegisterAction(const FString& IntentName,
    const FACEActionHandler& Handler)
{
    const FString Key = IntentName.ToLower();

    for (FRegisteredAction& R : Actions)
    {
        if (R.IntentName.Equals(Key, ESearchCase::IgnoreCase))
        {
            R.Handler = Handler;
            return;
        }
    }
    FRegisteredAction NewR;
    NewR.IntentName = Key;
    NewR.Handler = Handler;
    Actions.Add(MoveTemp(NewR));
}

void UCommandRouterComponent::UnregisterAction(const FString& IntentName)
{
    const FString Key = IntentName.ToLower();
    Actions.RemoveAll([&](const FRegisteredAction& R)
        {
            return R.IntentName.Equals(Key, ESearchCase::IgnoreCase);
        });
}

void UCommandRouterComponent::HandleGPTResponse(FString Out)
{
    OnPlannerText.Broadcast(Out);

    FACECommandList Plan;
    if (!TryParsePlan(Out, Plan))
    {
        UE_LOG(LogACEPlanner, Warning, TEXT("Failed to parse plan JSON."));
        return;
    }

    OnPlannerJSON.Broadcast(Plan);
    ExecutePlan(Plan, PendingInstigator.Get());
}

bool UCommandRouterComponent::TryParsePlan(const FString& JSON, FACECommandList& OutPlan) const
{
    if (!FJsonObjectConverter::JsonObjectStringToUStruct<FACECommandList>(JSON, &OutPlan, 0, 0))
    {
        return false;
    }
    
    return OutPlan.commands.Num() > 0;
}

void UCommandRouterComponent::ExecutePlan(const FACECommandList& Plan, AActor* Instigator)
{
    TMap<FString, FACEActionHandler> Table;
    for (const FRegisteredAction& R : Actions)
    {
        Table.Add(R.IntentName.ToLower(), R.Handler);
    }

    for (const FACECommand& Cmd : Plan.commands)
    {
        const FACEActionHandler* H = Table.Find(Cmd.intent.ToLower());
        if (H && H->IsBound())
        {
            H->Execute(Cmd, Instigator);
        }
        else
        {
            UE_LOG(LogACEPlanner, Verbose, TEXT("No handler bound for intent: %s"), *Cmd.intent);
        }
    }
}