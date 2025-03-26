// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class IGI_UE_Sample : ModuleRules
{
	public IGI_UE_Sample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "IGI" });

		PrivateDependencyModuleNames.AddRange(new string[] {});
	}
}
