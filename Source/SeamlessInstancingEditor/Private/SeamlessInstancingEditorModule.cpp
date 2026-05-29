// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingEditorModule.h"
#include "SeamlessInstancingEditorSubsystem.h"
#include "SeamlessInstancingStyle.h"

#include "ToolMenus.h"
#include "Editor/EditorEngine.h"

#define LOCTEXT_NAMESPACE "SeamlessInstancing"

void FSeamlessInstancingEditorModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	FSeamlessInstancingStyle::Initialize();

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection("SeamlessInstancing");

	FUIAction DefaultAction;

	FToolMenuEntry Entry = FToolMenuEntry::InitComboButton(
		"SeamlessInstancing",
		DefaultAction,
		FNewToolMenuDelegate::CreateRaw(this, &FSeamlessInstancingEditorModule::FillDropdownMenu),
		LOCTEXT("ToolbarButton", "Seamless Instancing"),
		LOCTEXT("ToolbarTooltip", "Seamless Instancing tools and commands"),
		FSlateIcon(FSeamlessInstancingStyle::GetStyleSetName(), "SeamlessInstancing.ToolbarIcon")
	);

	Section.AddEntry(Entry);
}

void FSeamlessInstancingEditorModule::ShutdownModule()
{
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->RemoveSection("LevelEditor.LevelEditorToolBar.User", "SeamlessInstancing");
	}

	FSeamlessInstancingStyle::Shutdown();
}

void FSeamlessInstancingEditorModule::FillDropdownMenu(UToolMenu* InMenu)
{
	USeamlessInstancingEditorSubsystem* InstancingSubsystem = GEditor ? GEditor->GetEditorSubsystem<USeamlessInstancingEditorSubsystem>() : nullptr;

	InMenu->AddSection("Options", LOCTEXT("OptionsSection", "Options"));

	FToolMenuEntry ToggleEntry = FToolMenuEntry::InitMenuEntry(
		"ToggleSeamlessInstancing",
		LOCTEXT("ToggleSeamlessInstancing", "Seamless Instancing"),
		LOCTEXT("ToggleSeamlessInstancingTooltip", "Enable seamless instancing"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([InstancingSubsystem]
			{
				if (InstancingSubsystem)
				{
					InstancingSubsystem->SetSeamlessEnabled(!InstancingSubsystem->IsSeamlessEnabled());
				}
			}),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([InstancingSubsystem]
			{
				if (InstancingSubsystem)
				{
					return InstancingSubsystem->IsSeamlessEnabled();
				}
				return false;
			})
		),
		EUserInterfaceActionType::Check
	);
	InMenu->AddMenuEntry("Options", ToggleEntry);

	InMenu->AddSection("Actions", LOCTEXT("ActionsSection", "Actions"));

	FToolMenuEntry ConvertSMToInstanced = FToolMenuEntry::InitMenuEntry(
		"ConvertAllSMToInstanced",
		LOCTEXT("ConvertAllSMToInstanced", "Convert All SM Actors to Instanced"),
		LOCTEXT("ConvertAllSMToInstancedTooltip", "Converts all StaticMesh actors in the world to instanced static mesh components"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([InstancingSubsystem]
		{
			if (InstancingSubsystem)
			{
				InstancingSubsystem->ConvertAllSMToInstanced();
			}
		}))
	);
	InMenu->AddMenuEntry("Actions", ConvertSMToInstanced);

	FToolMenuEntry ConvertInstancedToSM = FToolMenuEntry::InitMenuEntry(
		"ConvertAllInstancedToSM",
		LOCTEXT("ConvertAllInstancedToSM", "Convert All Instanced to SM Actors"),
		LOCTEXT("ConvertAllInstancedToSMTooltip", "Converts all instanced static mesh components back to individual static mesh actors"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([InstancingSubsystem]
		{
			if (InstancingSubsystem)
			{
				InstancingSubsystem->ConvertAllInstancedToSM();
			}
		}))
	);
	InMenu->AddMenuEntry("Actions", ConvertInstancedToSM);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSeamlessInstancingEditorModule, SeamlessInstancingEditor);
