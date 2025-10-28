// Copyright Epic Games, Inc. All Rights Reserved.

#include "ACEDirectorEditor.h"

#include "ToolMenus.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "SDirectorPanel.h"

#define LOCTEXT_NAMESPACE "FACEDirectorEditorModule"

static const FName ACEDirectorTabName("ACEDirector");

void FACEDirectorEditorModule::StartupModule()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        ACEDirectorTabName,
        FOnSpawnTab::CreateRaw(this, &FACEDirectorEditorModule::OnSpawnTab))
        .SetDisplayName(FText::FromString(TEXT("ACE Director")))
        .SetTooltipText(FText::FromString(TEXT("Open ACE Director tool")))
        .SetMenuType(ETabSpawnerMenuType::Hidden);

    RegisterMenus();
}

void FACEDirectorEditorModule::ShutdownModule()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ACEDirectorTabName);
}

void FACEDirectorEditorModule::RegisterMenus()
{
    UToolMenus* Menus = UToolMenus::Get();
    if (!Menus) return;

    UToolMenu* Menu = Menus->ExtendMenu("LevelEditor.MainMenu.Window");
    FToolMenuSection& Section = Menu->AddSection("ACEDirector", FText::FromString("ACE"));
    Section.AddMenuEntry(
        "OpenACEDirector",
        FText::FromString("ACE Director"),
        FText::FromString("Open the ACE Director panel"),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]
            {
                FGlobalTabmanager::Get()->TryInvokeTab(ACEDirectorTabName);
            }))
    );
}

TSharedRef<SDockTab> FACEDirectorEditorModule::OnSpawnTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SDirectorPanel)
        ];
}
#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FACEDirectorEditorModule, ACEDirector)