// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

class FSeamlessInstancingStyle
{
public:
	static void Initialize();
	static void Shutdown();
	static FName GetStyleSetName();
	static const ISlateStyle& Get();

private:
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
