#include "ACEConsoleCommandRegistry.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

void UACEConsoleCommandRegistry::Initialize(FSubsystemCollectionBase& Collection) {
    LoadJSON();
}

static FString RegPathJSON() { return FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE/data/console_registry.json")); }

bool UACEConsoleCommandRegistry::LoadJSON() {
    FString Raw;
    if (!FPaths::FileExists(RegPathJSON()) || !FFileHelper::LoadFileToString(Raw, *RegPathJSON())) return false;

    TSharedPtr<FJsonValue> RootVal;
    auto Reader = TJsonReaderFactory<>::Create(Raw);
    if (!FJsonSerializer::Deserialize(Reader, RootVal) || !RootVal.IsValid() || RootVal->Type != EJson::Array) return false;

    const auto& Arr = RootVal->AsArray();
    Entries.Reset();
    for (const auto& V : Arr) {
        if (!V.IsValid() || V->Type != EJson::Object) continue;
        const auto& O = V->AsObject();
        FConsoleCommandEntry E;
        O->TryGetStringField(TEXT("name"), E.Name);
        O->TryGetStringField(TEXT("doc"), E.Doc);
        O->TryGetStringField(TEXT("argNames"), E.ArgNames);

        const TArray<TSharedPtr<FJsonValue>>* Ali = nullptr; if (O->TryGetArrayField(TEXT("aliases"), Ali)) for (auto& x : *Ali) if (x.IsValid()) E.Aliases.Add(x->AsString());
        const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr; if (O->TryGetArrayField(TEXT("tags"), Tags)) for (auto& x : *Tags) if (x.IsValid()) E.Tags.Add(x->AsString());

        if (!E.Name.IsEmpty()) Entries.Add(MoveTemp(E));
    }
    return Entries.Num() > 0;
}

// Simple retrieval
TArray<FString> UACEConsoleCommandRegistry::Tokenize(const FString& S) {
    FString Lower = S.ToLower();
    for (TCHAR& c : Lower) if (!FChar::IsAlnum(c)) c = ' ';
    TArray<FString> Out; Lower.ParseIntoArrayWS(Out);
    return Out;
}

float UACEConsoleCommandRegistry::CosineLike(const TArray<FString>& Q, const TArray<FString>& D) {
    if (Q.Num() == 0 || D.Num() == 0) return 0.f;
    TMap<FString, int32> fq, fd;
    for (auto& t : Q) fq.FindOrAdd(t)++; for (auto& t : D) fd.FindOrAdd(t)++;
    float dot = 0, n1 = 0, n2 = 0;
    for (auto& kv : fq) { n1 += kv.Value * kv.Value; dot += kv.Value * (float)fd.FindRef(kv.Key); }
    for (auto& kv : fd) { n2 += kv.Value * kv.Value; }
    if (n1 == 0 || n2 == 0) return 0.f;
    return dot / (FMath::Sqrt(n1) * FMath::Sqrt(n2));
}

float UACEConsoleCommandRegistry::LexicalBonus(const TArray<FString>& Q, const FConsoleCommandEntry& E) {
    float bonus = 0.f;
    // exact command token match gives a big bump
    for (auto& q : Q) {
        if (q == E.Name.ToLower()) { bonus += 1.0f; }
    }
    // alias contains / fuzzy-ish (substring) bumps
    for (auto& a : E.Aliases) {
        const FString al = a.ToLower();
        for (auto& q : Q) if (al.Contains(q)) bonus += 0.15f;
    }
    // tags overlap
    for (auto& t : E.Tags) {
        const FString tl = t.ToLower();
        for (auto& q : Q) if (tl == q) bonus += 0.1f;
    }
    return FMath::Clamp(bonus, 0.f, 2.0f);
}

void UACEConsoleCommandRegistry::RetrieveTopK(const FString& Query, int32 K, TArray<FConsoleCandidate>& Out) const {
    TArray<FString> QTok = Tokenize(Query);
    struct Scored { int32 Idx; float Score; };
    TArray<Scored> scored; scored.Reserve(Entries.Num());
    for (int32 i = 0; i < Entries.Num(); ++i) {
        const auto& E = Entries[i];
        FString Docline = E.Name + TEXT(" ") + FString::Join(E.Aliases, TEXT(" ")) + TEXT(" ") + E.Doc + TEXT(" ") + FString::Join(E.Tags, TEXT(" ")) + TEXT(" ") + E.ArgNames;
        float cos = CosineLike(QTok, Tokenize(Docline));
        float lex = LexicalBonus(QTok, E);
        float s = 0.8f * cos + 0.2f * lex;
        if (s > 0.f) scored.Add({ i, s });
    }
    scored.Sort([](const Scored& A, const Scored& B) { return A.Score > B.Score; });
    Out.Reset();
    for (int32 i = 0; i < scored.Num() && i < K; ++i) {
        const auto& E = Entries[scored[i].Idx];
        FConsoleCandidate C;
        C.Name = E.Name; C.Aliases = E.Aliases; C.Doc = E.Doc; C.Tags = E.Tags; C.ArgNames = E.ArgNames; C.Score = scored[i].Score;
        Out.Add(MoveTemp(C));
    }
}
