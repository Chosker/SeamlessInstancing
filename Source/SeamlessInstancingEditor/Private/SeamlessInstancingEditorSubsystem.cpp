// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingEditorSubsystem.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Selection.h"
#include "LevelEditorSubsystem.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorSpatialHash.h"
#include "Engine/StaticMeshActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Misc/Crc.h"
#include "Serialization/BufferArchive.h"

#define LOCTEXT_NAMESPACE "SeamlessInstancing"

// ----- Property helpers ---------------------------------------------------

/**
 * Returns true if an object property references AActor or UActorComponent
 * subclasses — these are internal wiring (Owner, AttachParent, etc.) rather
 * than meaningful data.
 */
static bool IsActorOrComponentRef(const FProperty* Prop)
{
	const FObjectPropertyBase* ObjProp = CastField<const FObjectPropertyBase>(Prop);
	return ObjProp && ObjProp->PropertyClass
		&& (ObjProp->PropertyClass->IsChildOf<AActor>()
		 || ObjProp->PropertyClass->IsChildOf<UActorComponent>());
}

/** Filter for properties to include in both hashing and copying. */
static bool ShouldInclude(const FProperty* Prop)
{
	if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_EditorOnly))
		return false;
	if (Prop->IsA<FDelegateProperty>() || Prop->IsA<FMulticastDelegateProperty>()
		|| Prop->IsA<FInterfaceProperty>())
		return false;
	if (Prop->IsA<FSetProperty>() || Prop->IsA<FMapProperty>())
		return false;
	if (IsActorOrComponentRef(Prop))
		return false;
	if (const FArrayProperty* ArrayProp = CastField<const FArrayProperty>(Prop))
	{
		if (IsActorOrComponentRef(ArrayProp->Inner))
			return false;
	}

	// Skip administrative/internal properties that shouldn't be copied
	// onto the destination ISMC.
	static const TSet<FName> SkipNames = {
		TEXT("CreationMethod"),              // would overwrite AddInstanceComponent's setting
		TEXT("ComponentInstanceDataCache"),  // internal cache
		TEXT("RelativeLocation"),            // transform — each instance has its own via AddInstance
		TEXT("RelativeRotation"),
		TEXT("RelativeScale3D"),
		TEXT("AttachSocketName"),            // attachment wiring
	};
	if (SkipNames.Contains(Prop->GetFName()))
	{
		return false;
	}

	return true;
}

/**
 * Recursive property serializer for hashing.  Writes property values into a
 * plain FArchive without going through FProperty::SerializeItem, avoiding
 * the structured-archive API entirely.  Object references are written as
 * path strings for stable cross-component comparison.
 */
static void WritePropertyForHash(FArchive& Ar, FProperty* Prop, void* Value)
{
	// Struct: recurse into members
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			if (!ShouldInclude(*It))
				continue;
			void* MemberValue = It->ContainerPtrToValuePtr<uint8>(Value);
			WritePropertyForHash(Ar, *It, MemberValue);
		}
		return;
	}

	// Array: count + elements
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper Helper(ArrayProp, Value);
		int32 Num = Helper.Num();
		Ar << Num;
		for (int32 i = 0; i < Num; ++i)
		{
			void* ElemPtr = Helper.GetRawPtr(i);
			if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(ArrayProp->Inner))
			{
				UObject* Obj = ObjProp->GetObjectPropertyValue(ElemPtr);
				FString Path = Obj ? Obj->GetPathName() : FString();
				Ar << Path;
			}
			else if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
			{
				WritePropertyForHash(Ar, InnerStruct, ElemPtr);
			}
			else
			{
				Ar.Serialize(ElemPtr, ArrayProp->Inner->GetElementSize());
			}
		}
		return;
	}

	// Object reference: write path
	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* Obj = ObjProp->GetObjectPropertyValue(Value);
		FString Path = Obj ? Obj->GetPathName() : FString();
		Ar << Path;
		return;
	}

	// Everything else: raw bytes
	Ar.Serialize(Value, Prop->GetElementSize() * Prop->ArrayDim);
}

/** Walks the PropertyLink chain from UStaticMeshComponent up to UObject. */
static TArray<FProperty*> GatherProperties()
{
	TArray<FProperty*> Props;
	for (UClass* Class = UStaticMeshComponent::StaticClass();
		 Class && Class != UObject::StaticClass();
		 Class = Class->GetSuperClass())
	{
		for (FProperty* Prop = Class->PropertyLink; Prop; Prop = Prop->PropertyLinkNext)
		{
			Props.Add(Prop);
		}
	}
	return Props;
}

/** Compute a hash of all "data" property values on a component. */
static uint32 HashComponentProperties(UStaticMeshComponent* Component, const TArray<FProperty*>& Properties)
{
	FBufferArchive Ar;

	for (FProperty* Prop : Properties)
	{
		if (!ShouldInclude(Prop))
			continue;
		void* Value = Prop->ContainerPtrToValuePtr<uint8>(Component);
		WritePropertyForHash(Ar, Prop, Value);
	}

	return FCrc::MemCrc32(Ar.GetData(), Ar.Num());
}

/** Groups instances by mesh identity and all component property values. */
struct FInstanceGroupKey
{
	UStaticMesh* Mesh;
	uint32 PropertiesHash;

	bool operator==(const FInstanceGroupKey& Other) const
	{
		return Mesh == Other.Mesh && PropertiesHash == Other.PropertiesHash;
	}
};

FORCEINLINE uint32 GetTypeHash(const FInstanceGroupKey& Key)
{
	return HashCombine(GetTypeHash(Key.Mesh), Key.PropertiesHash);
}

// ----- World Partition helpers -------------------------------------------

/** Cell coordinate (2D, level 0) used as a map key. */
struct FCachedCellCoord
{
	int64 X;
	int64 Y;

	bool operator==(const FCachedCellCoord& Other) const
	{
		return X == Other.X && Y == Other.Y;
	}
};

FORCEINLINE uint32 GetTypeHash(const FCachedCellCoord& Key)
{
	return HashCombine(GetTypeHash(Key.X), GetTypeHash(Key.Y));
}

/** Reads the default WP grid cell size (in cm). */
static int32 GetWorldPartitionCellSize(const UWorldPartition* WorldPartition)
{
	if (!WorldPartition)
		return 25600;

	return WorldPartition->DefaultGridCellSize;
}


/** Finds an existing aggregate actor for the given label, or creates one.
 *  Always returns an actor with a root component. */
static AActor* FindOrCreateAggregateActor(UWorld* World, const FString& Label, const TArray<const UDataLayerAsset*>& DataLayers)
{
	// Look for an existing one by label (which encodes the cell coord)
	AActor* AggregateActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() == Label)
		{
			AggregateActor = *It;
			break;
		}
	}

	if (!AggregateActor)
	{
		// Create a new one
		FActorSpawnParameters SpawnParams;
		SpawnParams.bCreateActorPackage = (World->GetWorldPartition() != nullptr);
		AggregateActor = World->SpawnActor<AActor>(SpawnParams);
		AggregateActor->SetActorLabel(Label);
		AggregateActor->Tags.AddUnique(TEXT("SeamlessInstanceActor"));

		// Assign data layers from the partition key
		for (const UDataLayerAsset* DL : DataLayers)
		{
			if (UDataLayerManager* DLMgr = World->GetDataLayerManager())
			{
				if (const UDataLayerInstance* DLInstance = DLMgr->GetDataLayerInstanceFromAsset(DL))
				{
					DLInstance->AddActor(AggregateActor);
				}
			}
		}
	}

	// Ensure every aggregate actor has a root component so we can attach ISMCs.
	if (!AggregateActor->GetRootComponent())
	{
		USceneComponent* Root = NewObject<USceneComponent>(AggregateActor);
		Root->SetFlags(RF_Transactional);
		AggregateActor->SetRootComponent(Root);
		Root->RegisterComponent();
	}

	return AggregateActor;
}

// ----- Subsystem implementation -------------------------------------------

void USeamlessInstancingEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (GEditor)
	{
		Collection.InitializeDependency<ULevelEditorSubsystem>();

		// Cache the seamless-instancing toggle from config so we don't need
		// to read GEditorPerProjectIni on every selection-change event.
		GConfig->GetBool(TEXT("SeamlessInstancing"), TEXT("bEnableSeamless"), bCachedSeamlessEnabled, GEditorPerProjectIni);

		// Prime the previous-selection set from the current state.
		if (USelection* ActorSelection = GEditor->GetSelectedActors())
		{
			for (int32 i = 0; i < ActorSelection->Num(); ++i)
			{
				if (AActor* Actor = Cast<AActor>(ActorSelection->GetSelectedObject(i)))
				{
					PreviousSelectedActors.Add(Actor);
				}
			}
		}

		// Attempt binding now — the ticker will retry if the selection set
		// isn't ready yet (e.g. on initial editor startup).
		TryBindSelectionEvents();
	}

	UE_LOG(LogTemp, Log, TEXT("SeamlessInstancingEditorSubsystem initialized."));
}

void USeamlessInstancingEditorSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	TickerHandle.Reset();

	if (GEditor)
	{
		if (bSelectionEventsBound)
		{
			if (ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
			{
				if (UTypedElementSelectionSet* SelectionSet = LevelEditorSubsystem->GetSelectionSet())
				{
					SelectionSet->OnSelectionChange.RemoveDynamic(this, &USeamlessInstancingEditorSubsystem::OnSelectionChanged);
				}
			}
		}
	}

	PreviousSelectedActors.Empty();
	bIsConverting = false;
	bSelectionEventsBound = false;

	UE_LOG(LogTemp, Log, TEXT("SeamlessInstancingEditorSubsystem deinitialized."));

	Super::Deinitialize();
}

void USeamlessInstancingEditorSubsystem::ConvertAllSMToInstanced()
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	TArray<AStaticMeshActor*> ActorsToConvert;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* SMActor = *It;
		UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
		if (!SMC || !SMC->GetStaticMesh()) continue;
		if (!SMActor->GetIsSpatiallyLoaded()) continue;

		ActorsToConvert.Add(SMActor);
	}

	if (ActorsToConvert.IsEmpty())
	{
		return;
	}

	ConvertSMToInstanced(ActorsToConvert);
}

void USeamlessInstancingEditorSubsystem::ConvertSMToInstanced(const TArray<AStaticMeshActor*>& ActorsToConvert)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	const TArray<FProperty*> RelevantProperties = GatherProperties();

	// Determine whether to split into partition cells and what cell size to use.
	// The editor spatial hash's GetCellCoords uses the configured WP grid cell size.
	bool bUseCellGrouping = false;
	UWorldPartitionEditorSpatialHash* EditorSpatialHash = nullptr;
	TMap<FCachedCellCoord, AActor*> CellToAggregate;
	TMap<AStaticMeshActor*, FCachedCellCoord> ActorToCell;

	if (UWorldPartition* WorldPartition = World->GetWorldPartition())
	{
		if (WorldPartition->bEnableStreaming)
		{
			EditorSpatialHash = Cast<UWorldPartitionEditorSpatialHash>(WorldPartition->EditorHash);
			if (EditorSpatialHash)
			{
				bUseCellGrouping = true;

				const int32 CellSize = GetWorldPartitionCellSize(WorldPartition);

				for (AStaticMeshActor* SMActor : ActorsToConvert)
				{
					UWorldPartitionEditorSpatialHash::FCellCoord Cell = EditorSpatialHash->GetCellCoords(SMActor->GetActorLocation(), 0);
					FCachedCellCoord CacheKey{Cell.X, Cell.Y};

					ActorToCell.Add(SMActor, CacheKey);

					AActor*& Found = CellToAggregate.FindOrAdd(CacheKey);
					if (!Found)
					{
						FString Label = FString::Printf(TEXT("SeamlessInstanceActor_%lld_%lld"), Cell.X, Cell.Y);
						Found = FindOrCreateAggregateActor(World, Label, SMActor->GetDataLayerAssets());

						// Center the aggregate on its WP tile so the actor origin isn't arbitrary.
						Found->SetActorLocation(FVector(
							double(Cell.X) * CellSize + CellSize * 0.5,
							double(Cell.Y) * CellSize + CellSize * 0.5,
							0.0
						));
					}
				}
			}
		}
	}

	if (!bUseCellGrouping)
	{
		// Single aggregate for non-WP or non-streaming worlds
		AActor* SingleActor = FindOrCreateAggregateActor(World, TEXT("SeamlessInstanceActor_0_0"), {});
		CellToAggregate.Add(FCachedCellCoord{0, 0}, SingleActor);
	}

	TMap<AActor*, TMap<FInstanceGroupKey, UInstancedStaticMeshComponent*>> AggregateToMeshMap;
	TArray<AActor*> ActorsToDestroy;

	GEditor->BeginTransaction(LOCTEXT("ConvertSMToInstanced", "Convert SM Actors to Instanced"));

	for (AStaticMeshActor* SMActor : ActorsToConvert)
	{
		AActor* AggregateActor = nullptr;

		if (bUseCellGrouping)
		{
			AggregateActor = CellToAggregate[ActorToCell[SMActor]];
		}
		else
		{
			AggregateActor = CellToAggregate[FCachedCellCoord{0, 0}];
		}

		if (SMActor == AggregateActor) continue;

		UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
		SMActor->Modify();

		FInstanceGroupKey InstanceKey;
		InstanceKey.Mesh = SMC->GetStaticMesh();
		InstanceKey.PropertiesHash = HashComponentProperties(SMC, RelevantProperties);

		TMap<FInstanceGroupKey, UInstancedStaticMeshComponent*>& MeshToComponent = AggregateToMeshMap.FindOrAdd(AggregateActor);
		UInstancedStaticMeshComponent* ISMC = MeshToComponent.FindRef(InstanceKey);
		if (!ISMC)
		{
			ISMC = NewObject<UInstancedStaticMeshComponent>(AggregateActor);
			ISMC->SetFlags(RF_Transactional);
			ISMC->SetStaticMesh(InstanceKey.Mesh);
			ISMC->SetupAttachment(AggregateActor->GetRootComponent());
			AggregateActor->AddInstanceComponent(ISMC);
			ISMC->RegisterComponent();

			// Copy all included properties from source SMC onto the new ISMC.
			for (FProperty* Prop : RelevantProperties)
			{
				if (!ShouldInclude(Prop))
					continue;
				Prop->CopyCompleteValue_InContainer(ISMC, SMC);
			}

			MeshToComponent.Add(InstanceKey, ISMC);
		}

		ISMC->AddInstance(SMActor->GetTransform(), /*bWorldSpace=*/true);
		ActorsToDestroy.Add(SMActor);
	}

	for (AActor* Actor : ActorsToDestroy)
	{
		World->DestroyActor(Actor);
	}

	GEditor->EndTransaction();
}

void USeamlessInstancingEditorSubsystem::ConvertAllInstancedToSM()
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return;

	const TArray<FProperty*> RelevantProperties = GatherProperties();

	GEditor->BeginTransaction(LOCTEXT("ConvertInstancedToSM", "Convert Instanced to SM Actors"));

	// Collect all aggregate actors to convert (there may be one per WP cell).
	TArray<AActor*> AggregatesToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(TEXT("SeamlessInstanceActor")))
			continue;

		AActor* Aggregate = *It;
		TArray<UInstancedStaticMeshComponent*> ISMCs;
		Aggregate->GetComponents(ISMCs);

		if (ISMCs.IsEmpty())
			continue;

		Aggregate->Modify();

		for (UInstancedStaticMeshComponent* ISMC : ISMCs)
		{
			UStaticMesh* Mesh = ISMC->GetStaticMesh();
			if (!Mesh) continue;

			ISMC->Modify();
			const int32 NumInstances = ISMC->GetInstanceCount();

			for (int32 i = 0; i < NumInstances; ++i)
			{
				FTransform InstanceTransform;
				if (!ISMC->GetInstanceTransform(i, InstanceTransform, /*bWorldSpace=*/true))
					continue;

				AStaticMeshActor* SMActor = World->SpawnActor<AStaticMeshActor>();
				SMActor->SetActorTransform(InstanceTransform);
				UStaticMeshComponent* NewSMC = SMActor->GetStaticMeshComponent();
				NewSMC->SetStaticMesh(Mesh);
				SMActor->Modify();

				// Copy included properties from the ISMC onto the new SMC.
				for (FProperty* Prop : RelevantProperties)
				{
					if (!ShouldInclude(Prop))
						continue;
					Prop->CopyCompleteValue_InContainer(NewSMC, ISMC);
				}

				NewSMC->MarkRenderStateDirty();
			}

			ISMC->ClearInstances();
			ISMC->DestroyComponent(/*bPromoteChildren=*/false);
		}

		AggregatesToDestroy.Add(Aggregate);
	}

	for (AActor* Aggregate : AggregatesToDestroy)
	{
		World->DestroyActor(Aggregate);
	}

	GEditor->EndTransaction();
}

void USeamlessInstancingEditorSubsystem::SetSeamlessEnabled(bool bEnabled)
{
	bCachedSeamlessEnabled = bEnabled;
	GConfig->SetBool(TEXT("SeamlessInstancing"), TEXT("bEnableSeamless"), bEnabled, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

// ----- Lazy binding ---------------------------------------------------------

void USeamlessInstancingEditorSubsystem::TryBindSelectionEvents()
{
	if (bSelectionEventsBound)
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}

	if (ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
	{
		if (UTypedElementSelectionSet* SelectionSet = LevelEditorSubsystem->GetSelectionSet())
		{
			SelectionSet->OnSelectionChange.AddDynamic(this, &USeamlessInstancingEditorSubsystem::OnSelectionChanged);
			bSelectionEventsBound = true;
			TickerHandle.Reset();

			UE_LOG(LogTemp, Log, TEXT("SeamlessInstancingEditorSubsystem: selection events bound."));
			return;
		}
	}

	// Selection set not ready yet — retry via ticker.
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &USeamlessInstancingEditorSubsystem::TickBindRetry),
			0.5f);
	}
}

bool USeamlessInstancingEditorSubsystem::TickBindRetry(float DeltaTime)
{
	TryBindSelectionEvents();
	return !bSelectionEventsBound;
}

// ----- Selection-change handler --------------------------------------------

void USeamlessInstancingEditorSubsystem::OnSelectionChanged(const UTypedElementSelectionSet* SelectionSet)
{
	if (bIsConverting)
	{
		return;
	}

	if (!IsSeamlessEnabled())
	{
		return;
	}

	// Snapshot the current selection set.
	TSet<TWeakObjectPtr<AActor>> CurrentSelection;
	if (SelectionSet)
	{
		SelectionSet->ForEachSelectedObject([&CurrentSelection](UObject* Object)
		{
			if (AActor* Actor = Cast<AActor>(Object))
			{
				CurrentSelection.Add(Actor);
			}
			return true;
		});
	}

	// Find actors that were selected before but aren't anymore.
	TArray<AStaticMeshActor*> ActorsToConvert;
	for (const TWeakObjectPtr<AActor>& PrevActor : PreviousSelectedActors)
	{
		AActor* Actor = PrevActor.Get();
		if (!Actor)
		{
			continue;
		}
		if (CurrentSelection.Contains(Actor))
		{
			continue;
		}
		if (AStaticMeshActor* SMActor = Cast<AStaticMeshActor>(Actor))
		{
			UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
			if (SMC && SMC->GetStaticMesh() && SMActor->GetIsSpatiallyLoaded())
			{
				ActorsToConvert.Add(SMActor);
			}
		}
	}

	// Update tracking before converting so re-entrant events from DestroyActor
	// see a consistent state.
	PreviousSelectedActors = CurrentSelection;

	if (!ActorsToConvert.IsEmpty())
	{
		TGuardValue<bool> Guard(bIsConverting, true);
		ConvertSMToInstanced(ActorsToConvert);
	}
}

#undef LOCTEXT_NAMESPACE
