// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "SeamlessInstancingEditorSubsystem.generated.h"

class AActor;
class UTypedElementSelectionSet;
class FObjectPreSaveContext;

UENUM()
enum class ESeamlessComponentType : uint8
{
	Auto    UMETA(DisplayName = "Auto"),
	ISM     UMETA(DisplayName = "ISM"),
	HISM    UMETA(DisplayName = "HISM")
};

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

	TArray<AStaticMeshActor*> ConvertAllInstancedToSM();

	TArray<AStaticMeshActor*> ConvertInstancedToSM(const TArray<AActor*>& AggregatesToConvert);

	void SetSeamlessEnabled(bool bEnabled);

	bool IsSeamlessEnabled() const { return bCachedSeamlessEnabled; }

	ESeamlessComponentType GetComponentType() const { return ComponentType; }

	void SetComponentType(ESeamlessComponentType InType);

private:
	UFUNCTION()
	void OnSelectionChanged(const UTypedElementSelectionSet* SelectionSet);

	/** Ticker callback. Retries binding until the selection set becomes available */
	bool TickBindRetry(float DeltaTime);

	/** Ticker callback. Monitors MouseDeltaTracker to capture box-selection rect start */
	bool TickSelectionCheck(float DeltaTime);

	FTSTicker::FDelegateHandle TickerHandle;

	FTSTicker::FDelegateHandle SelectionTickerHandle;

	bool bIsConverting = false;
	bool bSelectionEventsBound = false;

	/** Set of actors that were selected before the most recent selection change */
	TSet<TWeakObjectPtr<AActor>> PreviousSelectedActors;

	/** Cached value of bEnableSeamless from GEditorPerProjectIni */
	bool bCachedSeamlessEnabled = false;

	/** Cached value of ComponentType from GEditorPerProjectIni */
	ESeamlessComponentType ComponentType = ESeamlessComponentType::Auto;

	/** Cached selection-rect start captured in TickSelectionCheck */
	FIntRect CachedSelectionRect = FIntRect();

	/** Handle for the PreSave package delegate subscription */
	FDelegateHandle PreSavePackageHandle;

	/** Handler called before any package is saved. Finds aggregate actors in the package
	 *  and stamps them with a fingerprint tag for change detection. */
	static void OnPreSavePackage(UPackage* Package, FObjectPreSaveContext Context);

};
