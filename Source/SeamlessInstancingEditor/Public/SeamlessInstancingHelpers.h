// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UInstancedStaticMeshComponent;
class ULevel;
class UStaticMesh;
class UStaticMeshComponent;
class UWorld;
class UWorldPartitionEditorSpatialHash;
class UDataLayerAsset;
class FArchive;
class FProperty;
class FViewport;

// ----- Property helpers ---------------------------------------------------

/** Groups instances by mesh and component property values */
struct FInstanceGroupKey
{
	UStaticMesh* Mesh = nullptr;
	uint32 PropertiesHash = 0;

	bool operator==(const FInstanceGroupKey& Other) const
	{
		return Mesh == Other.Mesh && PropertiesHash == Other.PropertiesHash;
	}
};

FORCEINLINE uint32 GetTypeHash(const FInstanceGroupKey& Key)
{
	return HashCombine(GetTypeHash(Key.Mesh), Key.PropertiesHash);
}

/** Returns true if an object property references AActor or UActorComponent subclasses */
bool IsActorOrComponentRef(const FProperty* Prop);

/** Filter for properties to include in hashing and copying */
bool ShouldInclude(const FProperty* Prop);

/** Recursive property serializer for hashing */
void WritePropertyForHash(FArchive& Ar, FProperty* Prop, void* Value);

/** Walks the PropertyLink chain from UStaticMeshComponent up to UObject */
TArray<FProperty*> GatherProperties();

/** Hash all data property values on a component */
uint32 HashComponentProperties(UStaticMeshComponent* Component, const TArray<FProperty*>& Properties);

/** Copy all included properties from Source onto Target using the pre-gathered property list */
void CopyRelevantProperties(UStaticMeshComponent* Source, UStaticMeshComponent* Target, const TArray<FProperty*>& Properties);

/** Find the ISM instance under the cursor via viewport hit-proxy system */
bool FindClickedInstance(AActor* Aggregate, int32& OutInstanceIndex, UInstancedStaticMeshComponent*& OutISMC);

/** Convert a single ISM instance to a StaticMeshActor and select it */
void BreakInstance(UInstancedStaticMeshComponent* ISMC, int32 InstanceIndex, bool bBeginTransaction = true);

/** Find all ISM instances on Aggregate whose hit-proxy screen bounds intersect the rect */
TArray<TPair<UInstancedStaticMeshComponent*, int32>> FindSelectionInstances(FViewport* Viewport, AActor* Aggregate, const FIntRect& SelectionRect);

// ----- World Partition helpers -------------------------------------------

/** Reads the default WP grid cell size from the editor spatial hash. Uses reflection because CellSize is private */
int32 GetWorldPartitionCellSize(const UWorldPartitionEditorSpatialHash* SpatialHash);

/** Finds an existing aggregate actor for the given label or creates one */
AActor* FindOrCreateAggregateActor(UWorld* World, const FString& Label, const TArray<const UDataLayerAsset*>& DataLayers, const TMap<FString, AActor*>& ExistingByLabel, FName RuntimeGrid = NAME_None, ULevel* OverrideLevel = nullptr);
