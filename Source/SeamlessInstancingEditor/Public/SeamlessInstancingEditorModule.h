// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSeamlessInstancing, Log, All);

class UToolMenu;

/**
 * Editor module for the SeamlessInstancing plugin.
 * Registers the toolbar button with dropdown in the level editor.
 */
class FSeamlessInstancingEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void FillDropdownMenu(UToolMenu* InMenu);
};
