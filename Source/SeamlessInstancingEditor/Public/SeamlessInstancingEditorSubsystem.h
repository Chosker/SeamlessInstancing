// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "SeamlessInstancingEditorSubsystem.generated.h"

class AActor;
class UTypedElementSelectionSet;

/**
 * Editor subsystem for SeamlessInstancing.
 * Registers on editor startup and serves as the central hub for plugin functionality.
 */
UCLASS()
class USeamlessInstancingEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Attempts to bind selection-change events. Safe to call repeatedly — no-op once bound. */
	void TryBindSelectionEvents();

	/** Converts all StaticMesh actors in the world to instanced static mesh components. */
	void ConvertSMToInstanced(const TArray<class AStaticMeshActor*>& ActorsToConvert);

	void ConvertAllSMToInstanced();

	/** Converts instanced static mesh components back to individual static mesh actors. */
	void ConvertAllInstancedToSM();

	/** Converts a given set of aggregate actors back to individual static mesh actors. */
	void ConvertInstancedToSM(const TArray<AActor*>& AggregatesToConvert);

	/** Toggle the seamless instancing mode on/off. Flushes to config immediately. */
	void SetSeamlessEnabled(bool bEnabled);

	/** Returns true if seamless instancing mode is currently enabled (cached). */
	bool IsSeamlessEnabled() const { return bCachedSeamlessEnabled; }

private:
	/** Called when level editor selection changes. Converts deselected StaticMeshActors to instances if seamless mode is active. */
	UFUNCTION()
	void OnSelectionChanged(const UTypedElementSelectionSet* SelectionSet);

	/** Ticker callback — retries binding until the selection set becomes available. */
	bool TickBindRetry(float DeltaTime);

	/** Handle for the deferred-binding ticker. */
	FTSTicker::FDelegateHandle TickerHandle;

	bool bIsConverting = false;
	bool bSelectionEventsBound = false;

	/** Set of actors that were selected before the most recent selection change. */
	TSet<TWeakObjectPtr<AActor>> PreviousSelectedActors;

	/** Cached value of bEnableSeamless from GEditorPerProjectIni to avoid config reads on every selection event. */
	bool bCachedSeamlessEnabled = false;
};
