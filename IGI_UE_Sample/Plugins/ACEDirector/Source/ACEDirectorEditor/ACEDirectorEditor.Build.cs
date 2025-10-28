// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ACEDirectorEditor : ModuleRules
{
	public ACEDirectorEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
                "UMG"
            }
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
                "Core", "CoreUObject", "Engine",
				"Slate", "SlateCore", "InputCore",
				"EditorStyle", "LevelEditor", "UnrealEd",
				"PropertyEditor", "ToolMenus",
                "Json", "JsonUtilities",
                "HTTP",
                "IGI",
                "ACEDirectorRuntime"
            }
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
