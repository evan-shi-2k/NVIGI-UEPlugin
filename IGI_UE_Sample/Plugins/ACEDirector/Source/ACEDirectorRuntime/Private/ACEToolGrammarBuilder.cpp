#include "ACEToolGrammarBuilder.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

static FString JsonEscape(const FString& In) {
    FString Out; Out.Reserve(In.Len() + 8);
    for (TCHAR c : In) {
        switch (c) {
        case TEXT('\"'): Out += TEXT("\\\""); break;
        case TEXT('\\'): Out += TEXT("\\\\"); break;
        case TEXT('\b'): Out += TEXT("\\b");  break;
        case TEXT('\f'): Out += TEXT("\\f");  break;
        case TEXT('\n'): Out += TEXT("\\n");  break;
        case TEXT('\r'): Out += TEXT("\\r");  break;
        case TEXT('\t'): Out += TEXT("\\t");  break;
        default:
            if (c < 0x20) { Out += TEXT(' '); }
            else { Out += c; }
        }
    }
    return Out;
}

FString UACEToolGrammarBuilder::BuildPerQueryGrammar(
    const TArray<FString>& WorldIntents,
    const TArray<FString>& ConsoleNames)
{
    auto QuoteJoin = [](const TArray<FString>& In)
        {
            TArray<FString> Q;
            Q.Reserve(In.Num());
            for (const FString& s : In)
            {
                // JSON-string literal: "\"value\""
                Q.Add(TEXT("\"\\\"") + JsonEscape(s) + TEXT("\\\"\""));
            }
            return FString::Join(Q, TEXT(" | "));
        };

    const bool bHasConsole = ConsoleNames.Num() > 0;

    const FString IntentChoices =
        WorldIntents.Num() ? QuoteJoin(WorldIntents)
        : TEXT("\"\\\"Say\\\"\"");

    const FString CommandChoices =
        ConsoleNames.Num() ? QuoteJoin(ConsoleNames)
        : TEXT("\"\\\"stat fps\\\"\"");

    if (bHasConsole)
    {
        return FString::Printf(
            TEXT(R"(root ::= console_root | act_root
console_root ::= "{" ws "\"tool\"" ws ":" ws "\"console.execute\"" ws "," ws "\"console\"" ws ":" ws console_payload ws "}" ws
act_root ::= "{" ws "\"tool\"" ws ":" ws "\"world.act\"" ws "," ws "\"act\"" ws ":" ws act_payload ws "}" ws
console_payload ::= "{" ws "\"command\"" ws ":" ws command (ws "," ws "\"args\"" ws ":" ws jstring)? ws "}"
command ::= %s
act_payload ::= "{" ws "\"commands\"" ws ":" ws "[" ws cmd (ws "," ws cmd)* ws "]" ws "}"
cmd ::= "{" ws "\"intent\"" ws ":" ws intent (ws "," ws "\"target\"" ws ":" ws jstring)? (ws "," ws "\"params\"" ws ":" ws jobject)? (ws "," ws "\"priority\"" ws ":" ws jnumber)? ws "}"
intent ::= %s
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
ws      ::= (" " | "\t" | "\r" | "\n")* )"),
            *CommandChoices,
            *IntentChoices
        );
    }
    else
    {
        // No console candidates: only world.act is allowed
        return FString::Printf(
            TEXT(R"(root ::= act_root
act_root ::= "{" ws "\"tool\"" ws ":" ws "\"world.act\"" ws "," ws "\"act\"" ws ":" ws act_payload ws "}" ws
act_payload ::= "{" ws "\"commands\"" ws ":" ws "[" ws cmd (ws "," ws cmd)* ws "]" ws "}"
cmd ::= "{" ws "\"intent\"" ws ":" ws intent (ws "," ws "\"target\"" ws ":" ws jstring)? (ws "," ws "\"params\"" ws ":" ws jobject)? (ws "," ws "\"priority\"" ws ":" ws jnumber)? ws "}"
intent ::= %s
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
ws      ::= (" " | "\t" | "\r" | "\n")* )"),
            *IntentChoices
        );
    }
}

FString UACEToolGrammarBuilder::WriteTempGrammarFile(const FString& Grammar)
{
    const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ACE"));
    IFileManager::Get().MakeDirectory(*Dir, true);
    const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("tool_chooser_%llu.ebnf"), FDateTime::UtcNow().GetTicks()));
    FFileHelper::SaveStringToFile(Grammar, *Path);
    return Path;
}
