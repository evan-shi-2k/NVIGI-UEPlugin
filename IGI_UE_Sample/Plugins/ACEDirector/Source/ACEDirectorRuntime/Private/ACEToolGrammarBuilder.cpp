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

FString UACEToolGrammarBuilder::BuildPerQueryGrammar(const TArray<FString>& WorldIntents, const TArray<FString>& ConsoleNames)
{
    auto QuoteJoin = [](const TArray<FString>& In) {
        TArray<FString> Q; Q.Reserve(In.Num());
        for (auto& s : In) Q.Add(TEXT("\"") + JsonEscape(s) + TEXT("\""));
        return FString::Join(Q, TEXT(" | "));
        };

    const FString IntentChoices = WorldIntents.Num() ? QuoteJoin(WorldIntents) : TEXT("\"Say\"");
    const FString CommandChoices = ConsoleNames.Num() ? QuoteJoin(ConsoleNames) : TEXT("\"stat fps\"");

    return FString::Printf(
        TEXT(R"(root ::= "{" ws "\"tool\"" ws ":" ws tool (ws "," ws payload)? ws "}" ws

tool ::= "\"console.execute\"" | "\"world.act\""

payload ::= "\"console\"" ws ":" ws console_payload
          | "\"act\""     ws ":" ws act_payload

console_payload ::= "{" ws "\"command\"" ws ":" ws command
                     (ws "," ws "\"args\"" ws ":" ws jstring)? ws "}"

command ::= %s

act_payload ::= "{" ws "\"commands\"" ws ":" ws "[" ws cmd (ws "," ws cmd)* ws "]" ws "}"

cmd ::= "{" ws "\"intent\"" ws ":" ws intent
         (ws "," ws "\"target\"" ws ":" ws jstring)?
         (ws "," ws "\"params\"" ws ":" ws jobject)?
         (ws "," ws "\"priority\"" ws ":" ws jnumber)?
         ws "}"

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
*CommandChoices, *IntentChoices);
}

FString UACEToolGrammarBuilder::WriteTempGrammarFile(const FString& Grammar)
{
    const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ACE"));
    IFileManager::Get().MakeDirectory(*Dir, true);
    const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("tool_chooser_%llu.ebnf"), FDateTime::UtcNow().GetTicks()));
    FFileHelper::SaveStringToFile(Grammar, *Path);
    return Path;
}
