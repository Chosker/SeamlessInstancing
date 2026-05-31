// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "SeamlessInstancingEditorSubsystem.generated.h"

class AActor;
class UTypedElementSelectionSet;

UCLASS()
class USeamlessInstancingEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Attempts to bind selection-change events. Called repeatedly until succeeds */
	void TryBindSelectionEvents();

	void ConvertSMToInstanced(const TArray<class AStaticMeshActor*>& ActorsToConvert);

	void ConvertAllSMToInstanced();

	void ConvertAllInstancedToSM();

	void ConvertInstancedToSM(const TArray<AActor*>& AggregatesToConvert);

	void SetSeamlessEnabled(bool bEnabled);

	bool IsSeamlessEnabled() const { return bCachedSeamlessEnabled; }

private:
	UFUNCTION()
	void OnSelectionChanged(const UTypedElementSelectionSet* SelectionSet);

	/** Ticker callback. Retries binding until the selection set becomes available */
	bool TickBindRetry(float DeltaTime);

	FTSTicker::FDelegateHandle TickerHandle;

	bool bIsConverting = false;
	bool bSelectionEventsBound = false;

	/** Set of actors that were selected before the most recent selection change */
	TSet<TWeakObjectPtr<AActor>> PreviousSelectedActors;

	/** Cached value of bEnableSeamless from GEditorPerProjectIni */
	bool bCachedSeamlessEnabled = false;

	bool FindClickedInstance(AActor* Aggregate,	int32& OutInstanceIndex, UInstancedStaticMeshComponent*& OutISMC) const;

	void BreakInstance(UInstancedStaticMeshComponent* ISMC, int32 InstanceIndex);
};
