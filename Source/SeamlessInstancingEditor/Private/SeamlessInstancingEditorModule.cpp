// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingEditorModule.h"
#include "SeamlessInstancingEditorSubsystem.h"
#include "SeamlessInstancingStyle.h"

#include "ToolMenus.h"
#include "Editor/EditorEngine.h"

#define LOCTEXT_NAMESPACE "SeamlessInstancing"

DEFINE_LOG_CATEGORY(LogSeamlessInstancing);

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
		LOCTEXT("ToolbarTooltip", "Seamless Instancing"),
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

	InMenu->AddSection("Seamless Instancing", LOCTEXT("SeamlessInstancingSection", "Seamless Instancing"));

	FToolMenuEntry ToggleEntry = FToolMenuEntry::InitMenuEntry(
		"ToggleSeamlessInstancing",
		LOCTEXT("ToggleSeamlessInstancing", "Enable Seamless Instancing"),
		LOCTEXT("ToggleSeamlessInstancingTooltip", "Enables the seamless instancing system"),
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
	InMenu->AddMenuEntry("Seamless Instancing", ToggleEntry);

	InMenu->AddSection("Options", LOCTEXT("OptionsSection", "Options"));

	FToolMenuEntry ComponentTypeEntry = FToolMenuEntry::InitSubMenu(
		"ComponentType",
		LOCTEXT("ComponentType", "Component Type"),
		LOCTEXT("ComponentTypeTooltip", "Choose which instanced component type to use when converting"),
		FNewToolMenuDelegate::CreateLambda([InstancingSubsystem](UToolMenu* SubMenu)
		{
			FToolMenuSection& SubSection = SubMenu->AddSection("ComponentTypeSection", LOCTEXT("ComponentTypeSection", "Component Type"));

			// Auto
			SubSection.AddMenuEntry(
				"ComponentTypeAuto",
				LOCTEXT("ComponentTypeAuto", "Auto"),
				LOCTEXT("ComponentTypeAutoTooltip", "Automatically choose ISM or HISM based on Nanite use"),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([InstancingSubsystem]
					{
						if (InstancingSubsystem)
						{
							InstancingSubsystem->SetComponentType(ESeamlessComponentType::Auto);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([InstancingSubsystem]
					{
						return InstancingSubsystem && InstancingSubsystem->GetComponentType() == ESeamlessComponentType::Auto;
					})
				),
				EUserInterfaceActionType::RadioButton
			);

			// ISM
			SubSection.AddMenuEntry(
				"ComponentTypeISM",
				LOCTEXT("ComponentTypeISM", "ISM"),
				LOCTEXT("ComponentTypeISMTooltip", "Always use InstancedStaticMeshComponent"),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([InstancingSubsystem]
					{
						if (InstancingSubsystem)
						{
							InstancingSubsystem->SetComponentType(ESeamlessComponentType::ISM);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([InstancingSubsystem]
					{
						return InstancingSubsystem && InstancingSubsystem->GetComponentType() == ESeamlessComponentType::ISM;
					})
				),
				EUserInterfaceActionType::RadioButton
			);

			// HISM
			SubSection.AddMenuEntry(
				"ComponentTypeHISM",
				LOCTEXT("ComponentTypeHISM", "HISM"),
				LOCTEXT("ComponentTypeHISMTooltip", "Always use HierarchicalInstancedStaticMeshComponent"),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([InstancingSubsystem]
					{
						if (InstancingSubsystem)
						{
							InstancingSubsystem->SetComponentType(ESeamlessComponentType::HISM);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([InstancingSubsystem]
					{
						return InstancingSubsystem && InstancingSubsystem->GetComponentType() == ESeamlessComponentType::HISM;
					})
				),
				EUserInterfaceActionType::RadioButton
			);
		}),
		false
	);
	InMenu->AddMenuEntry("Options", ComponentTypeEntry);

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

	FToolMenuEntry RecreateInstances = FToolMenuEntry::InitMenuEntry(
		"RecreateInstances",
		LOCTEXT("RecreateInstances", "Recreate All Instances"),
		LOCTEXT("RecreateInstancesTooltip", "Breaks all instances and then re-instances them, when a cleanup is needed"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([InstancingSubsystem]
		{
			if (InstancingSubsystem)
			{
				TArray<AStaticMeshActor*> CreatedActors = InstancingSubsystem->ConvertAllInstancedToSM();
				if (!CreatedActors.IsEmpty())
				{
					InstancingSubsystem->ConvertSMToInstanced(CreatedActors);
				}
			}
		}))
	);
	InMenu->AddMenuEntry("Actions", RecreateInstances);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSeamlessInstancingEditorModule, SeamlessInstancingEditor);
