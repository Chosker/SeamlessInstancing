// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingStyle.h"

#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

TSharedPtr<FSlateStyleSet> FSeamlessInstancingStyle::StyleInstance = nullptr;

void FSeamlessInstancingStyle::Initialize()
{
	if (StyleInstance.IsValid())
	{
		return;
	}

	StyleInstance = MakeShareable(new FSlateStyleSet("SeamlessInstancingStyle"));

	const FString ResourceDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("SeamlessInstancing"))->GetBaseDir(),
		TEXT("Resources")
	);
	StyleInstance->SetContentRoot(ResourceDir);

	StyleInstance->Set("SeamlessInstancing.ToolbarIcon", new FSlateImageBrush(
		ResourceDir / TEXT("Icon128.png"),
		FVector2D(128.0f, 128.0f)
	));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
}

void FSeamlessInstancingStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		StyleInstance.Reset();
	}
}

FName FSeamlessInstancingStyle::GetStyleSetName()
{
	static FName StyleName("SeamlessInstancingStyle");
	return StyleName;
}

const ISlateStyle& FSeamlessInstancingStyle::Get()
{
	return *StyleInstance;
}
