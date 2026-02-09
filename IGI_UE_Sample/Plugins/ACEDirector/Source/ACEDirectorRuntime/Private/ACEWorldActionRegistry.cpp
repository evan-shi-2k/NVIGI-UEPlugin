// SPDX-License-Identifier: MIT
#include "ACEWorldActionRegistry.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

static FString JsonStringify(const TSharedPtr<FJsonValue>& V)
{
    if (!V.IsValid()) return TEXT("");
    FString Out;
    auto Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(V, TEXT(""), Writer);
    return Out;
}

static FString JsonStringifyObject(const TSharedPtr<FJsonObject>& O)
{
    if (!O.IsValid()) return TEXT("");
    FString Out;
    auto Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(O.ToSharedRef(), Writer);
    return Out;
}

static FString FlattenExamplesSummary(const TArray<TSharedPtr<FJsonValue>>& Examples)
{
    FString S;
    for (const auto& ExV : Examples)
    {
        if (!ExV.IsValid() || ExV->Type != EJson::Object) continue;
        const auto ExO = ExV->AsObject();

        FString Intent;
        ExO->TryGetStringField(TEXT("intent"), Intent);

        FString ArgsBrief;
        const TSharedPtr<FJsonObject>* ArgsObjPtr = nullptr;
        if (ExO->TryGetObjectField(TEXT("args"), ArgsObjPtr) && ArgsObjPtr && ArgsObjPtr->IsValid())
        {
            const TSharedPtr<FJsonObject>& ArgsObj = *ArgsObjPtr;

            for (const auto& KVP : ArgsObj->Values)
            {
                if (!KVP.Value.IsValid()) continue;
                FString ValStr;
                if (KVP.Value->Type == EJson::String) ValStr = KVP.Value->AsString();
                else ValStr = JsonStringify(KVP.Value);

                if (!ArgsBrief.IsEmpty()) ArgsBrief += TEXT(", ");
                ArgsBrief += KVP.Key + TEXT("=") + ValStr;
            }
        }

        if (!Intent.IsEmpty())
        {
            if (!S.IsEmpty()) S += TEXT("; ");
            S += Intent;
            if (!ArgsBrief.IsEmpty())
            {
                S += TEXT(" ");
                S += ArgsBrief;
            }
        }
    }
    return S;
}

static FString FlattenArgsSummary(const TSharedPtr<FJsonObject>& ArgsObj)
{
    if (!ArgsObj.IsValid()) return TEXT("");

    FString S;
    for (const auto& KVP : ArgsObj->Values)
    {
        const FString ArgName = KVP.Key;
        if (!KVP.Value.IsValid() || KVP.Value->Type != EJson::Object) continue;

        const auto ArgSpec = KVP.Value->AsObject();
        FString Type, Doc;
        ArgSpec->TryGetStringField(TEXT("type"), Type);
        ArgSpec->TryGetStringField(TEXT("doc"), Doc);

        FString One;
        One += ArgName;
        if (!Type.IsEmpty()) One += TEXT(" ") + Type;
        if (!Doc.IsEmpty())  One += TEXT(" ") + Doc;

        // values (enum)
        const TArray<TSharedPtr<FJsonValue>>* ValuesArr = nullptr;
        if (ArgSpec->TryGetArrayField(TEXT("values"), ValuesArr))
        {
            TArray<FString> Vals;
            for (auto& v : *ValuesArr) if (v.IsValid()) Vals.Add(v->AsString());
            if (Vals.Num() > 0) One += TEXT(" values ") + FString::Join(Vals, TEXT(","));
        }

        // examples
        const TArray<TSharedPtr<FJsonValue>>* ExArr = nullptr;
        if (ArgSpec->TryGetArrayField(TEXT("examples"), ExArr))
        {
            TArray<FString> Ex;
            for (auto& e : *ExArr)
            {
                if (!e.IsValid()) continue;
                if (e->Type == EJson::String) Ex.Add(e->AsString());
                else Ex.Add(JsonStringify(e));
            }
            if (Ex.Num() > 0) One += TEXT(" examples ") + FString::Join(Ex, TEXT(","));
        }

        if (!S.IsEmpty()) S += TEXT(" ");
        S += One;
    }
    return S;
}

static FString FlattenConstraintsSummary(const TArray<TSharedPtr<FJsonValue>>& Constraints)
{
    TArray<FString> Lines;
    for (const auto& C : Constraints)
    {
        if (!C.IsValid()) continue;
        if (C->Type == EJson::String) Lines.Add(C->AsString());
        else Lines.Add(JsonStringify(C));
    }
    return FString::Join(Lines, TEXT("; "));
}

FString UACEWorldActionRegistry::JsonPath()
{
    return FPaths::Combine(FPaths::ProjectDir(), TEXT("ACE/data/world_actions.json"));
}

void UACEWorldActionRegistry::Initialize(FSubsystemCollectionBase& Collection)
{
    LoadJSON();
}

bool UACEWorldActionRegistry::LoadJSON()
{
    const FString Path = JsonPath();
    FString Raw;
    if (!FPaths::FileExists(Path) || !FFileHelper::LoadFileToString(Raw, *Path))
        return false;

    TSharedPtr<FJsonValue> RootVal;
    auto Reader = TJsonReaderFactory<>::Create(Raw);
    if (!FJsonSerializer::Deserialize(Reader, RootVal) || !RootVal.IsValid() || RootVal->Type != EJson::Array)
        return false;

    const auto& Arr = RootVal->AsArray();
    Entries.Reset();

    for (const auto& V : Arr)
    {
        if (!V.IsValid() || V->Type != EJson::Object) continue;
        const auto O = V->AsObject();

        FWorldActionEntry E;
        O->TryGetStringField(TEXT("intent"), E.Intent);
        O->TryGetStringField(TEXT("doc"), E.Doc);

        // aliases/tags
        const TArray<TSharedPtr<FJsonValue>>* Ali = nullptr;
        if (O->TryGetArrayField(TEXT("aliases"), Ali))
            for (auto& x : *Ali) if (x.IsValid()) E.Aliases.Add(x->AsString());

        const TArray<TSharedPtr<FJsonValue>>* Tg = nullptr;
        if (O->TryGetArrayField(TEXT("tags"), Tg))
            for (auto& x : *Tg) if (x.IsValid()) E.Tags.Add(x->AsString());

        {
            const TSharedPtr<FJsonObject>* ArgsObjPtr = nullptr;
            if (O->TryGetObjectField(TEXT("args"), ArgsObjPtr) && ArgsObjPtr && ArgsObjPtr->IsValid())
            {
                const TSharedPtr<FJsonObject>& ArgsObj = *ArgsObjPtr;

                E.ArgsSchemaJson = JsonStringifyObject(ArgsObj);
                E.ArgsSummary = FlattenArgsSummary(ArgsObj);
            }
        }

        {
            const TArray<TSharedPtr<FJsonValue>>* Constraints = nullptr;
            if (O->TryGetArrayField(TEXT("constraints"), Constraints) && Constraints)
            {
                E.ConstraintsSummary = FlattenConstraintsSummary(*Constraints);
            }
        }

        {
            const TArray<TSharedPtr<FJsonValue>>* Examples = nullptr;
            if (O->TryGetArrayField(TEXT("examples"), Examples) && Examples)
            {
                TSharedPtr<FJsonValue> ExamplesVal = MakeShared<FJsonValueArray>(*Examples);
                E.ExamplesJson = JsonStringify(ExamplesVal);
                E.ExamplesSummary = FlattenExamplesSummary(*Examples);
            }
        }

        if (!E.Intent.IsEmpty())
            Entries.Add(MoveTemp(E));
    }

    return Entries.Num() > 0;
}

TArray<FString> UACEWorldActionRegistry::Tokenize(const FString& S)
{
    FString L = S.ToLower();
    for (TCHAR& c : L) if (!FChar::IsAlnum(c)) c = ' ';
    TArray<FString> Out; L.ParseIntoArrayWS(Out);
    return Out;
}

float UACEWorldActionRegistry::CosineLike(const TArray<FString>& Q, const TArray<FString>& D)
{
    if (Q.Num() == 0 || D.Num() == 0) return 0.f;
    TMap<FString, int32> fq, fd;
    for (auto& t : Q) fq.FindOrAdd(t)++;
    for (auto& t : D) fd.FindOrAdd(t)++;
    float dot = 0, n1 = 0, n2 = 0;
    for (auto& kv : fq) { n1 += kv.Value * kv.Value; dot += kv.Value * (float)fd.FindRef(kv.Key); }
    for (auto& kv : fd) { n2 += kv.Value * kv.Value; }
    if (n1 == 0 || n2 == 0) return 0.f;
    return dot / (FMath::Sqrt(n1) * FMath::Sqrt(n2));
}

float UACEWorldActionRegistry::LexicalBonus(const TArray<FString>& Q, const FWorldActionEntry& E)
{
    float bonus = 0.f;
    const FString In = E.Intent.ToLower();
    for (const FString& q : Q) if (q == In) bonus += 0.6f;

    for (const FString& a : E.Aliases)
    {
        const FString al = a.ToLower();
        for (const FString& q : Q) if (al.Contains(q)) bonus += 0.15f;
    }
    for (const FString& t : E.Tags)
    {
        const FString tl = t.ToLower();
        for (const FString& q : Q) if (tl == q) bonus += 0.1f;
    }
    return FMath::Clamp(bonus, 0.f, 1.5f);
}

void UACEWorldActionRegistry::RetrieveTopK(const FString& Query, int32 K, TArray<FWorldActionCandidate>& Out) const
{
    TArray<FString> QTok = Tokenize(Query);

    struct S { int32 Idx; float Score; };
    TArray<S> Scored;
    Scored.Reserve(Entries.Num());

    for (int32 i = 0; i < Entries.Num(); ++i)
    {
        const auto& E = Entries[i];

        FString Docline =
            E.Intent + TEXT(" ")
            + FString::Join(E.Aliases, TEXT(" ")) + TEXT(" ")
            + E.Doc + TEXT(" ")
            + FString::Join(E.Tags, TEXT(" ")) + TEXT(" ")
            + E.ArgsSummary + TEXT(" ")
            + E.ConstraintsSummary + TEXT(" ")
            + E.ExamplesSummary;

        const float cos = CosineLike(QTok, Tokenize(Docline));
        const float lex = LexicalBonus(QTok, E);
        const float s = 0.8f * cos + 0.2f * lex;
        if (s > 0.f) Scored.Add({ i, s });
    }

    Scored.Sort([](const S& A, const S& B) { return A.Score > B.Score; });

    Out.Reset();
    for (int32 i = 0; i < Scored.Num() && i < K; ++i)
    {
        const auto& E = Entries[Scored[i].Idx];

        FWorldActionCandidate C;
        C.Intent = E.Intent;
        C.Doc = E.Doc;
        C.Score = Scored[i].Score;

        C.ArgsSchemaJson = E.ArgsSchemaJson;
        C.ExamplesJson = E.ExamplesJson;

        Out.Add(MoveTemp(C));
    }
}
