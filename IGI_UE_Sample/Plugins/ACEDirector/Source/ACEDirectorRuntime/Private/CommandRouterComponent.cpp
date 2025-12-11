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

FString UCommandRouterComponent::BuildToolChooserUserJSON(
    const FString& UserText,
    const TArray<FConsoleCandidate>& Cands) const
{
    FString Out(TEXT("{\"user\":\""));
    Out += UACEToolGrammarBuilder::JsonEscape(UserText);
    Out += TEXT("\",\"console_candidates\":[");
    for (int32 i = 0; i < Cands.Num(); ++i) {
        const auto& C = Cands[i];
        Out += TEXT("{\"name\":\"") + UACEToolGrammarBuilder::JsonEscape(C.Name) 
             + TEXT("\",\"argNames\":\"") + UACEToolGrammarBuilder::JsonEscape(C.ArgNames) 
             + TEXT("\",\"doc\":\"") + UACEToolGrammarBuilder::JsonEscape(C.Doc) 
             + TEXT("\",\"aliases\":[");
        for (int32 j = 0; j < C.Aliases.Num(); ++j) {
            Out += TEXT("\"") + UACEToolGrammarBuilder::JsonEscape(C.Aliases[j]) + TEXT("\"");
            if (j + 1 < C.Aliases.Num()) Out += TEXT(",");
        }
        Out += TEXT("],\"tags\":[");
        for (int32 j = 0; j < C.Tags.Num(); ++j) {
            Out += TEXT("\"") + UACEToolGrammarBuilder::JsonEscape(C.Tags[j]) + TEXT("\"");
            if (j + 1 < C.Tags.Num()) Out += TEXT(",");
        }
        Out += FString::Printf(TEXT("],\"score\":%.3f,\"idx\":%d}"), C.Score, i);
        if (i + 1 < Cands.Num()) Out += TEXT(",");
    }
    Out += TEXT("]}");
    return Out;
}

UCommandRouterComponent::UCommandRouterComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UCommandRouterComponent::RouteFromText(const FString& UserDirective, AActor* Instigator)
{
    PendingInstigator = Instigator;

    // Retrieve top-K sets
    TArray<FConsoleCandidate> ConsoleCands;
    if (UACEConsoleCommandRegistry* RC = GetWorld()->GetGameInstance()->GetSubsystem<UACEConsoleCommandRegistry>())
        RC->RetrieveTopK(UserDirective, /*K=*/5, ConsoleCands);

    TArray<FWorldActionCandidate> WorldCands;
    if (UACEWorldActionRegistry* RW = GetWorld()->GetGameInstance()->GetSubsystem<UACEWorldActionRegistry>())
        RW->RetrieveTopK(UserDirective, /*K=*/5, WorldCands);

    TArray<FString> IntentNames;  for (auto& c : WorldCands)   IntentNames.Add(c.Intent);
    TArray<FString> ConsoleNames; for (auto& c : ConsoleCands) ConsoleNames.Add(c.Name);

    const FString Grammar = UACEToolGrammarBuilder::BuildPerQueryGrammar(IntentNames, ConsoleNames);
    const FString GrammarPath = UACEToolGrammarBuilder::WriteTempGrammarFile(Grammar);

    const FString Packed = BuildToolChooserUserJSON(UserDirective, ConsoleCands);

    UIGIGPTEvaluateAsync* Node = UIGIGPTEvaluateAsync::GPTEvaluateStructuredWithGrammarAsync(Packed, GrammarPath);
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

// TODO: Refactor
void UCommandRouterComponent::HandleGPTResponse(FString Out)
{
    OnPlannerText.Broadcast(Out);

    TSharedPtr<FJsonObject> Root;
    auto Reader = TJsonReaderFactory<>::Create(Out);
    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
    {
        FString Tool;
        if (Root->TryGetStringField(TEXT("tool"), Tool))
        {
            if (Tool.Equals(TEXT("console.execute"), ESearchCase::IgnoreCase))
            {
                const TSharedPtr<FJsonObject>* Console = nullptr;
                if (Root->TryGetObjectField(TEXT("console"), Console) && Console && Console->IsValid())
                {
                    FString Cmd, Args; (*Console)->TryGetStringField(TEXT("command"), Cmd);
                    (*Console)->TryGetStringField(TEXT("args"), Args);
                    FString Line = Cmd; if (!Args.IsEmpty()) { Line += TEXT(" "); Line += Args; }
                    UACEConsoleTool::Execute(this, Line);
                    return;
                }
            }
        }
    }

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