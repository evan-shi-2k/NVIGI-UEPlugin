#include "CommandRouterComponent.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/DefaultValueHelper.h"
#include "HAL/IConsoleManager.h"

#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Kismet/KismetStringLibrary.h"

#include "IGIBlueprintLibrary.h"
#include "ACEToolGrammarBuilder.h"
#include "ACEConsoleTool.h"

DEFINE_LOG_CATEGORY_STATIC(LogACEPlanner, Log, All);

static TAutoConsoleVariable<float> CVarACE_MinConsoleCandidateScore(
    TEXT("ace.MinConsoleCandidateScore"),
    0.10f,
    TEXT("Console candidate must have Score >= this to be included in the per-query grammar."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarACE_MinWorldCandidateScore(
    TEXT("ace.MinWorldCandidateScore"),
    0.10f,
    TEXT("World action candidate must have Score >= this to be included in the per-query grammar."),
    ECVF_Default);

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
    const TArray<FConsoleCandidate>& ConsoleCands,
    const TArray<FWorldActionCandidate>& WorldCands) const
{
    FString Out(TEXT("{\"user\":\""));
    Out += UACEToolGrammarBuilder::JsonEscape(UserText);

    Out += TEXT("\",\"console_candidates\":[");
    for (int32 i = 0; i < ConsoleCands.Num(); ++i) {
        const auto& C = ConsoleCands[i];
        Out += TEXT("{\"name\":\"") + UACEToolGrammarBuilder::JsonEscape(C.Name) 
             + TEXT("\",\"argNames\":\"") + UACEToolGrammarBuilder::JsonEscape(C.ArgNames) 
             + TEXT("\",\"doc\":\"") + UACEToolGrammarBuilder::JsonEscape(C.Doc);
        Out += FString::Printf(TEXT("\",\"score\":%.3f}"), C.Score);
        if (i + 1 < ConsoleCands.Num()) Out += TEXT(",");
    }
    Out += TEXT("]");

    Out += TEXT(",\"world_candidates\":[");
    for (int32 i = 0; i < WorldCands.Num(); ++i)
    {
        const auto& C = WorldCands[i];
        Out += TEXT("{\"intent\":\"") + UACEToolGrammarBuilder::JsonEscape(C.Intent)
            + TEXT("\",\"doc\":\"") + UACEToolGrammarBuilder::JsonEscape(C.Doc)
            + TEXT("\",\"schema\":") + (C.ArgsSchemaJson.IsEmpty() ? TEXT("null") : C.ArgsSchemaJson)
            + TEXT(",\"examples\":") + (C.ExamplesJson.IsEmpty() ? TEXT("null") : C.ExamplesJson)
            + FString::Printf(TEXT(",\"score\":%.3f}"), C.Score);
        if (i + 1 < WorldCands.Num()) Out += TEXT(",");
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

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogACEPlanner, Warning, TEXT("RouteFromText: GetWorld() is null"));
        return;
    }

    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        UE_LOG(LogACEPlanner, Warning,
            TEXT("RouteFromText: GameInstance is null (WorldType=%d). This usually means you're calling from the Editor world; run PIE and target the PIE actor."),
            (int32)World->WorldType);
        return;
    }

    // Retrieve top-K sets
    TArray<FConsoleCandidate> ConsoleCands;
    if (UACEConsoleCommandRegistry* RC = GetWorld()->GetGameInstance()->GetSubsystem<UACEConsoleCommandRegistry>())
        RC->RetrieveTopK(UserDirective, /*K=*/3, ConsoleCands);

    TArray<FWorldActionCandidate> WorldCands;
    if (UACEWorldActionRegistry* RW = GetWorld()->GetGameInstance()->GetSubsystem<UACEWorldActionRegistry>())
        RW->RetrieveTopK(UserDirective, /*K=*/3, WorldCands);

    const float MinConsole = CVarACE_MinConsoleCandidateScore.GetValueOnGameThread();
    const float MinWorld = CVarACE_MinWorldCandidateScore.GetValueOnGameThread();

    ConsoleCands.RemoveAll([&](const FConsoleCandidate& C) { return C.Score < MinConsole; });
    WorldCands.RemoveAll([&](const FWorldActionCandidate& C) { return C.Score < MinWorld; });

    TArray<FString> IntentNames;  for (auto& c : WorldCands)   IntentNames.Add(c.Intent);
    TArray<FString> ConsoleNames; for (auto& c : ConsoleCands) ConsoleNames.Add(c.Name);

    const FString Grammar = UACEToolGrammarBuilder::BuildPerQueryGrammar(IntentNames, ConsoleNames);
    const FString GrammarPath = UACEToolGrammarBuilder::WriteTempGrammarFile(Grammar);
    const FString Packed = BuildToolChooserUserJSON(UserDirective, ConsoleCands, WorldCands);
    UE_LOG(LogACEPlanner, Warning, TEXT("%s"), *Packed);

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

            if (Tool.Equals(TEXT("world.act"), ESearchCase::IgnoreCase))
            {
                const TSharedPtr<FJsonObject>* ActObj = nullptr;
                if (Root->TryGetObjectField(TEXT("act"), ActObj) && ActObj && ActObj->IsValid())
                {
                    FString Unwrapped;
                    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Unwrapped);
                    FJsonSerializer::Serialize((*ActObj).ToSharedRef(), Writer);
                    Writer->Close();
                    Out = Unwrapped;
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