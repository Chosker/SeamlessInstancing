// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UStaticMesh;
class UStaticMeshComponent;
class UWorld;
class UWorldPartitionEditorSpatialHash;
class UDataLayerAsset;
class FArchive;
class FProperty;

// ----- Property helpers ---------------------------------------------------

/** Groups instances by mesh identity and all component property values. */
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

/**
 * Returns true if an object property references AActor or UActorComponent
 * subclasses — these are internal wiring (Owner, AttachParent, etc.) rather
 * than meaningful data.
 */
bool IsActorOrComponentRef(const FProperty* Prop);

/** Filter for properties to include in both hashing and copying. */
bool ShouldInclude(const FProperty* Prop);

/**
 * Recursive property serializer for hashing.  Writes property values into a
 * plain FArchive without going through FProperty::SerializeItem, avoiding
 * the structured-archive API entirely.  Object references are written as
 * path strings for stable cross-component comparison.
 */
void WritePropertyForHash(FArchive& Ar, FProperty* Prop, void* Value);

/** Walks the PropertyLink chain from UStaticMeshComponent up to UObject. */
TArray<FProperty*> GatherProperties();

/** Compute a hash of all "data" property values on a component. */
uint32 HashComponentProperties(UStaticMeshComponent* Component, const TArray<FProperty*>& Properties);

// ----- World Partition helpers -------------------------------------------

/** Cell coordinate (2D, level 0) used as a map key. */
struct FCachedCellCoord
{
	int64 X = 0;
	int64 Y = 0;

	bool operator==(const FCachedCellCoord& Other) const
	{
		return X == Other.X && Y == Other.Y;
	}
};

FORCEINLINE uint32 GetTypeHash(const FCachedCellCoord& Key)
{
	return HashCombine(GetTypeHash(Key.X), GetTypeHash(Key.Y));
}

/**
 * Reads the default WP grid cell size (in cm) from the editor spatial hash.
 * Uses reflection because CellSize is a private UPROPERTY(Config) on the hash.
 */
int32 GetWorldPartitionCellSize(const UWorldPartitionEditorSpatialHash* SpatialHash);

/** Finds an existing aggregate actor for the given label, or creates one.
 *  Always returns an actor with a root component.
 *  @param ExistingByLabel  Pre-built map of existing aggregate actors by label
 *                         (built once per conversion pass; new creations go into
 *                         CellToAggregate on return, not back into this map).
 */
AActor* FindOrCreateAggregateActor(UWorld* World, const FString& Label, const TArray<const UDataLayerAsset*>& DataLayers,
	const TMap<FString, AActor*>& ExistingByLabel);
