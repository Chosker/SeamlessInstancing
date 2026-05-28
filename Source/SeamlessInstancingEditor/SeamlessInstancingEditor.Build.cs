// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SeamlessInstancingEditor : ModuleRules
{
	public SeamlessInstancingEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"UnrealEd",
			"LevelEditor",
			"Slate",
			"SlateCore",
			"EditorSubsystem",
			"ToolMenus",
			"Projects",
			"TypedElementRuntime",
		});
	}
}
