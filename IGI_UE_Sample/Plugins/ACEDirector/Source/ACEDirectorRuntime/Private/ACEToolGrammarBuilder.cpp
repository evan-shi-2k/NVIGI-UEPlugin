#include "ACEToolGrammarBuilder.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

namespace
{
    static const TCHAR* GenericJsonEbnf = TEXT(R"(
# Generic JSON
value   ::= jstring | jnumber | jobject | jarray | "true" | "false" | "null"
jstring ::= "\"" chars "\""
chars   ::= ( char )*
char    ::= [^"\\\u0000-\u001F] | "\\" ( "\"" | "\\" | "/" | "b" | "f" | "n" | "r" | "t" )
jnumber ::= "-"? int frac? exp?
int     ::= "0" | [1-9][0-9]*
frac    ::= "." [0-9]+
exp     ::= ("e" | "E") ("+" | "-")? [0-9]+
jobject ::= "{" ws ( jmember ( ws "," ws jmember )* )? ws "}"
jmember ::= jstring ws ":" ws value
jarray  ::= "[" ws ( value ( ws "," ws value )* )? ws "]"
ws      ::= (" " | "\t" | "\r" | "\n")*
)");

    static const TCHAR* ActBlockTpl = TEXT(R"(
act_root ::= "{" ws "\"tool\"" ws ":" ws "\"world.act\"" ws "," ws "\"act\"" ws ":" ws act_payload ws "}"
act_payload ::= "{" ws "\"commands\"" ws ":" ws "[" ws cmd ws "]" ws "}"
cmd ::= "{" ws "\"intent\"" ws ":" ws intent ws "," ws "\"args\"" ws ":" ws args_obj (ws "," ws "\"priority\"" ws ":" ws jnumber)? ws "}"
intent ::= {{INTENTS}}
args_obj ::= "{" ws (arg_kv (ws "," ws arg_kv)*)? ws "}"
arg_kv   ::= jstring ws ":" ws jstring
)");

    static const TCHAR* ConsoleBlockTpl = TEXT(R"(
console_root ::= "{" ws "\"tool\"" ws ":" ws "\"console.execute\"" ws "," ws "\"console\"" ws ":" ws console_payload ws "}"
console_payload ::= "{" ws "\"command\"" ws ":" ws command (ws "," ws "\"args\"" ws ":" ws jstring)? ws "}"
command ::= {{COMMANDS}}
)");

    static inline void AppendWithNewline(FString& Out, const FString& Chunk)
    {
        if (!Out.IsEmpty() && !Out.EndsWith(TEXT("\n")))
        {
            Out += TEXT("\n");
        }
        Out += Chunk;
    }
}

FString UACEToolGrammarBuilder::JsonEscape(const FString& In)
{
    FString Out;
    Out.Reserve(In.Len() + 8);

    for (TCHAR c : In)
    {
        switch (c)
        {
        case TEXT('\"'): Out += TEXT("\\\""); break;
        case TEXT('\\'): Out += TEXT("\\\\"); break;
        case TEXT('\b'): Out += TEXT("\\b");  break;
        case TEXT('\f'): Out += TEXT("\\f");  break;
        case TEXT('\n'): Out += TEXT("\\n");  break;
        case TEXT('\r'): Out += TEXT("\\r");  break;
        case TEXT('\t'): Out += TEXT("\\t");  break;
        default:
            Out += (c < 0x20) ? TEXT(' ') : c;
            break;
        }
    }
    return Out;
}

FString UACEToolGrammarBuilder::BuildPerQueryGrammar(
    const TArray<FString>& WorldIntents,
    const TArray<FString>& ConsoleNames)
{
    auto QuoteJoin = [](const TArray<FString>& In, const TCHAR* DefaultChoice) -> FString
        {
            if (In.Num() == 0)
            {
                return FString(DefaultChoice);
            }

            TArray<FString> Q;
            Q.Reserve(In.Num());
            for (const FString& s : In)
            {
                Q.Add(TEXT("\"\\\"") + JsonEscape(s) + TEXT("\\\"\""));
            }
            return FString::Join(Q, TEXT(" | "));
        };

    const bool bHasIntent = WorldIntents.Num() > 0;
    const bool bHasConsole = ConsoleNames.Num() > 0;

    const FString IntentChoices = QuoteJoin(WorldIntents, TEXT("\"\\\"Say\\\"\""));
    const FString CommandChoices = QuoteJoin(ConsoleNames, TEXT("\"\\\"stat fps\\\"\""));

    FString ActBlock(ActBlockTpl);
    ActBlock.ReplaceInline(TEXT("{{INTENTS}}"), *IntentChoices, ESearchCase::CaseSensitive);

    FString ConsoleBlock(ConsoleBlockTpl);
    ConsoleBlock.ReplaceInline(TEXT("{{COMMANDS}}"), *CommandChoices, ESearchCase::CaseSensitive);

    FString Grammar;

    if (bHasConsole && bHasIntent)
    {
        AppendWithNewline(Grammar, TEXT("root ::= console_root | act_root"));
        AppendWithNewline(Grammar, ConsoleBlock);
        AppendWithNewline(Grammar, ActBlock);
        AppendWithNewline(Grammar, FString(GenericJsonEbnf));
        return Grammar;
    }

    if (bHasIntent)
    {
        AppendWithNewline(Grammar, TEXT("root ::= act_root"));
        AppendWithNewline(Grammar, ActBlock);
        AppendWithNewline(Grammar, FString(GenericJsonEbnf));
        return Grammar;
    }

    if (bHasConsole)
    {
        AppendWithNewline(Grammar, TEXT("root ::= console_root"));
        AppendWithNewline(Grammar, ConsoleBlock);
        AppendWithNewline(Grammar, FString(GenericJsonEbnf));
        return Grammar;
    }

    // No candidates: default root to act_root (IntentChoices already defaults to Say)
    AppendWithNewline(Grammar, TEXT("root ::= act_root"));
    AppendWithNewline(Grammar, ActBlock);
    AppendWithNewline(Grammar, FString(GenericJsonEbnf));
    return Grammar;
}

FString UACEToolGrammarBuilder::WriteTempGrammarFile(const FString& Grammar)
{
    const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ACE"));
    IFileManager::Get().MakeDirectory(*Dir, true);

    const FString Path = FPaths::Combine(
        Dir,
        FString::Printf(TEXT("tool_chooser_%llu.ebnf"), FDateTime::UtcNow().GetTicks())
    );

    FFileHelper::SaveStringToFile(Grammar, *Path);
    return Path;
}
