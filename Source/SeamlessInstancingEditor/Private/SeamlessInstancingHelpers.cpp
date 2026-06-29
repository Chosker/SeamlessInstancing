// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingHelpers.h"
#include "SeamlessInstancingEditorModule.h"

#include "Serialization/BufferArchive.h"
#include "Misc/Crc.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorSpatialHash.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "SeamlessInstancing"

// ============================================================================
// Property helpers
// ============================================================================

bool IsActorOrComponentRef(const FProperty* Prop)
{
	const FObjectPropertyBase* ObjProp = CastField<const FObjectPropertyBase>(Prop);
	return ObjProp && ObjProp->PropertyClass
		&& (ObjProp->PropertyClass->IsChildOf<AActor>()
		 || ObjProp->PropertyClass->IsChildOf<UActorComponent>());
}

bool ShouldInclude(const FProperty* Prop)
{
	if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_EditorOnly))
	{
		return false;
	}
	if (Prop->IsA<FDelegateProperty>() || Prop->IsA<FMulticastDelegateProperty>()
		|| Prop->IsA<FInterfaceProperty>())
	{
		return false;
	}
	if (Prop->IsA<FSetProperty>() || Prop->IsA<FMapProperty>())
	{
		return false;
	}
	if (IsActorOrComponentRef(Prop))
	{
		return false;
	}
	if (const FArrayProperty* ArrayProp = CastField<const FArrayProperty>(Prop))
	{
		if (IsActorOrComponentRef(ArrayProp->Inner))
		{
			return false;
		}
	}

	// Skip properties that shouldn't be copied onto the destination ISMC.
	static const TSet<FName> SkipNames = {
		TEXT("CreationMethod"),              // would overwrite AddInstanceComponent's setting
		TEXT("ComponentInstanceDataCache"),  // internal cache
		TEXT("RelativeLocation"),            // transform
		TEXT("RelativeRotation"),
		TEXT("RelativeScale3D"),
		TEXT("AttachSocketName"),            // attachment wiring
		TEXT("ComponentTags"),               // labels only; the SrcHash_* tag we stamp on the ISMC
		                                     // lives here and would cause hash mismatches if included,
		                                     // and two identical components with different tags should
		                                     // still merge into the same ISM group
		TEXT("CustomPrimitiveData"),         // transferred explicitly as per-instance custom data
	};
	if (SkipNames.Contains(Prop->GetFName()))
	{
		return false;
	}

	return true;
}

void WritePropertyForHash(FArchive& Ar, FProperty* Prop, void* Value)
{
	// Struct: recurse into members
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			if (!ShouldInclude(*It))
			{
				continue;
			}
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

TArray<FProperty*> GatherProperties()
{
	TArray<FProperty*> Props;
	for (UClass* Class = UStaticMeshComponent::StaticClass();
		 Class && Class != UObject::StaticClass();
		 Class = Class->GetSuperClass())
	{
		for (TFieldIterator<FProperty> It(Class, EFieldIterationFlags::None); It; ++It)
		{
			Props.Add(*It);
		}
	}
	return Props;
}

uint32 HashComponentProperties(UStaticMeshComponent* Component, const TArray<FProperty*>& Properties)
{
	FBufferArchive Ar;

	for (FProperty* Prop : Properties)
	{
		if (!ShouldInclude(Prop))
		{
			continue;
		}
		void* Value = Prop->ContainerPtrToValuePtr<uint8>(Component);
		WritePropertyForHash(Ar, Prop, Value);
	}

	return FCrc::MemCrc32(Ar.GetData(), Ar.Num());
}

void CopyRelevantProperties(UStaticMeshComponent* Source, UStaticMeshComponent* Target, const TArray<FProperty*>& Properties)
{
	for (FProperty* Prop : Properties)
	{
		if (!ShouldInclude(Prop))
		{
			continue;
		}
		Prop->CopyCompleteValue_InContainer(Target, Source);
	}
}

// ============================================================================
// Selection helpers
// ============================================================================

bool FindClickedInstance(AActor* Aggregate, int32& OutInstanceIndex, UInstancedStaticMeshComponent*& OutISMC)
{
	if (!GEditor || !Aggregate)
	{
		return false;
	}

	FViewport* ActiveViewport = GEditor->GetActiveViewport();
	if (!ActiveViewport)
	{
		return false;
	}

	const int32 MouseX = ActiveViewport->GetMouseX();
	const int32 MouseY = ActiveViewport->GetMouseY();
	if (MouseX < 0 || MouseY < 0)
	{
		return false;
	}

	HHitProxy* HitProxy = ActiveViewport->GetHitProxy(MouseX, MouseY);
	if (!HitProxy)
	{
		UE_LOG(LogSeamlessInstancing, Log, TEXT("FindClickedInstance: HitProxy is null at (%d,%d)"), MouseX, MouseY);
		return false;
	}

	if (!HitProxy->IsA(HInstancedStaticMeshInstance::StaticGetType()))
	{
		//UE_LOG(LogSeamlessInstancing, Log, TEXT("FindClickedInstance: unexpected HitProxy type \"%s\" at (%d,%d)"), HitProxy->GetType()->GetName(), MouseX, MouseY);
		return false;
	}

	const HInstancedStaticMeshInstance* ISMHit = static_cast<const HInstancedStaticMeshInstance*>(HitProxy);
	if (!ISMHit->Component || ISMHit->Component->GetOwner() != Aggregate)
	{
		return false;
	}

	OutISMC = ISMHit->Component;
	OutInstanceIndex = ISMHit->InstanceIndex;
	return true;
}

void BreakInstance(UInstancedStaticMeshComponent* ISMC, int32 InstanceIndex, bool bBeginTransaction)
{
	if (!ISMC || InstanceIndex < 0 || InstanceIndex >= ISMC->GetInstanceCount())
	{
		return;
	}

	AActor* Aggregate = ISMC->GetOwner();
	UWorld* World = Aggregate ? Aggregate->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	FTransform InstanceTransform;
	if (!ISMC->GetInstanceTransform(InstanceIndex, InstanceTransform, /*bWorldSpace=*/true))
	{
		return;
	}

	UStaticMesh* Mesh = ISMC->GetStaticMesh();
	if (!Mesh)
	{
		return;
	}

	const TArray<FProperty*> RelevantProperties = GatherProperties();

	if (bBeginTransaction)
	{
		GEditor->BeginTransaction(LOCTEXT("BreakInstance", "Break Instance"));
	}

	// Save the state of all affected objects so the transaction can properly undo
	Aggregate->Modify();
	ISMC->Modify();
	if (USceneComponent* Root = Aggregate->GetRootComponent())
	{
		Root->Modify();
	}

	// Deselect the aggregate before breaking the instance to avoid rendering its outline
	if (GEditor)
	{
		GEditor->SelectActor(Aggregate, /*bSelected=*/false, /*bNotify=*/true);
	}

	// Spawn the new SM actor in the same sublevel as the aggregate
	FActorSpawnParameters BreakSpawnParams;
	BreakSpawnParams.OverrideLevel = Aggregate->GetLevel();
	AStaticMeshActor* NewSMActor = World->SpawnActor<AStaticMeshActor>(BreakSpawnParams);
	NewSMActor->SetActorTransform(InstanceTransform);
	NewSMActor->Modify();

	UStaticMeshComponent* NewSMC = NewSMActor->GetStaticMeshComponent();
	NewSMC->SetStaticMesh(Mesh);

	// Set the actor label to the mesh name (made unique across all actors in the world)
	TSet<FString> ExistingLabels;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It != NewSMActor)
		{
			ExistingLabels.Add(It->GetActorLabel());
		}
	}

	FString BaseLabel = Mesh->GetName();
	FString FinalLabel = BaseLabel;
	int32 Suffix = 1;
	while (ExistingLabels.Contains(FinalLabel))
	{
		FinalLabel = FString::Printf(TEXT("%s_%d"), *BaseLabel, Suffix++);
	}
	NewSMActor->SetActorLabel(FinalLabel);

	CopyRelevantProperties(ISMC, NewSMC, RelevantProperties);

	// Copy PerInstanceCustomData from the ISMC onto the new SMC's CustomPrimitiveData
	if (ISMC->NumCustomDataFloats > 0 && InstanceIndex * ISMC->NumCustomDataFloats < ISMC->PerInstanceSMCustomData.Num())
	{
		const float* InstanceDataStart = &ISMC->PerInstanceSMCustomData[InstanceIndex * ISMC->NumCustomDataFloats];
		NewSMC->SetDefaultCustomPrimitiveDataFloatArray(0, MakeConstArrayView(InstanceDataStart, ISMC->NumCustomDataFloats));
	}

	// Safeguard to ensure correct Static Mobility
	if (NewSMC->Mobility != EComponentMobility::Static)
	{
		NewSMC->SetMobility(EComponentMobility::Static);
	}

	// Assign the same data layers as the aggregate
	for (const UDataLayerAsset* DL : Aggregate->GetDataLayerAssets())
	{
		if (UDataLayerManager* DLMgr = World->GetDataLayerManager())
		{
			if (const UDataLayerInstance* DLInstance = DLMgr->GetDataLayerInstanceFromAsset(DL))
			{
				DLInstance->AddActor(NewSMActor);
			}
		}
	}

	// Copy the Runtime Grid from the aggregate
	{
		const FName AggregateRuntimeGrid = Aggregate->GetRuntimeGrid();
		if (AggregateRuntimeGrid != NAME_None)
		{
			NewSMActor->SetRuntimeGrid(AggregateRuntimeGrid);
		}
	}
	// Copy the classic Actor Layers from the aggregate
	NewSMActor->Layers = Aggregate->Layers;

	NewSMC->MarkRenderStateDirty();

	// Detach the instance from the ISMC
	ISMC->RemoveInstance(InstanceIndex);

	// Clean up empty ISMCs and if needed, the aggregate itself
	if (ISMC->GetInstanceCount() == 0)
	{
		ISMC->DestroyComponent();

		TArray<UInstancedStaticMeshComponent*> RemainingISMCs;
		Aggregate->GetComponents(RemainingISMCs);
		if (RemainingISMCs.IsEmpty())
		{
			Aggregate->SetIsTemporarilyHiddenInEditor(true);
		}
	}

	if (bBeginTransaction)
	{
		GEditor->EndTransaction();
	}
	// Refresh the World Outliner
	GEditor->BroadcastLevelActorListChanged();

	// Defer selection of the newly broken-out actor to the next tick
	TWeakObjectPtr<AStaticMeshActor> WeakNewActor = NewSMActor;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakNewActor](float) -> bool
		{
			if (GEditor)
			{
				if (AStaticMeshActor* NewActor = WeakNewActor.Get())
				{
					GEditor->SelectActor(NewActor, /*bSelected=*/true, /*bNotify=*/true);
				}
			}
			return false;
		}
	));
}

TArray<TPair<UInstancedStaticMeshComponent*, int32>> FindSelectionInstances(FViewport* Viewport, AActor* Aggregate, const FIntRect& SelectionRect)
{
	TArray<TPair<UInstancedStaticMeshComponent*, int32>> Out;
	if (!Viewport || !Aggregate)
	{
		return Out;
	}
	if (SelectionRect.Width() <= 0 || SelectionRect.Height() <= 0)
	{
		return Out;
	}

	TSet<TPair<UInstancedStaticMeshComponent*, int32>> Seen;

	Viewport->EnumerateHitProxiesInRect(SelectionRect,
		[&Out, Aggregate, &Seen](HHitProxy* HitProxy) -> bool
	{
		if (HitProxy && HitProxy->IsA(HInstancedStaticMeshInstance::StaticGetType()))
		{
			const HInstancedStaticMeshInstance* ISMH = static_cast<const HInstancedStaticMeshInstance*>(HitProxy);
			UInstancedStaticMeshComponent* ISMC = ISMH ? ISMH->Component : nullptr;
			if (ISMC && ISMC->GetOwner() == Aggregate)
			{
				const TPair<UInstancedStaticMeshComponent*, int32> Key(ISMC, ISMH->InstanceIndex);
				bool bAlreadyInSet = false;
				Seen.Add(Key, &bAlreadyInSet);
				if (!bAlreadyInSet)
				{
					Out.Emplace(ISMC, ISMH->InstanceIndex);
				}
			}
		}
		return true; // keep iterating
	});

	return Out;
}

void AddInstanceDeterministic(UInstancedStaticMeshComponent* ISMC, const FTransform& NewWorldTransform, const TArray<float>& NewCustomData)
{
	if (!ISMC)
	{
		return;
	}

	struct FDeterministicInstanceData
	{
		FTransform Transform;
		TArray<float> CustomData;
	};

	TArray<FDeterministicInstanceData> InstanceData;
	const int32 ExistingCount = ISMC->GetInstanceCount();
	const int32 OldStride = ISMC->NumCustomDataFloats;
	const int32 NumSrcFloats = NewCustomData.Num();
	const int32 NewStride = FMath::Max(OldStride, NumSrcFloats);

	InstanceData.Reserve(ExistingCount + 1);

	// Snapshot existing instances (Transform + PerInstanceCustomData)
	for (int32 i = 0; i < ExistingCount; ++i)
	{
		FDeterministicInstanceData& Item = InstanceData.AddDefaulted_GetRef();
		ISMC->GetInstanceTransform(i, Item.Transform, true);

		if (OldStride > 0)
		{
			Item.CustomData.SetNum(OldStride);
			FMemory::Memcpy(Item.CustomData.GetData(), &ISMC->PerInstanceSMCustomData[i * OldStride], OldStride * sizeof(float));
		}
	}

	// Prepare new instance's data
	FDeterministicInstanceData NewItem;
	NewItem.Transform = NewWorldTransform;

	if (NewStride > 0)
	{
		NewItem.CustomData.SetNumZeroed(NewStride);
		if (NumSrcFloats > 0)
		{
			FMemory::Memcpy(NewItem.CustomData.GetData(), NewCustomData.GetData(), NumSrcFloats * sizeof(float));
		}
	}

	InstanceData.Add(MoveTemp(NewItem));

	// Sort all instances by X, then Y, then Z for deterministic ordering
	InstanceData.Sort([](const FDeterministicInstanceData& A, const FDeterministicInstanceData& B)
	{
		const FVector LocA = A.Transform.GetLocation();
		const FVector LocB = B.Transform.GetLocation();
		if (LocA.X < LocB.X) return true;
		if (LocB.X < LocA.X) return false;
		if (LocA.Y < LocB.Y) return true;
		if (LocB.Y < LocA.Y) return false;
		return LocA.Z < LocB.Z;
	});

	// Clear the ISMC and re-add all instances in sorted order
	ISMC->ClearInstances();

	TArray<FTransform> AllTransforms;
	AllTransforms.Reserve(InstanceData.Num());
	for (const FDeterministicInstanceData& Item : InstanceData)
	{
		AllTransforms.Add(Item.Transform);
	}
	ISMC->AddInstances(AllTransforms, true, true);

	// Update custom data stride and populate PerInstanceSMCustomData
	if (NewStride > 0)
	{
		ISMC->NumCustomDataFloats = NewStride;
		ISMC->PerInstanceSMCustomData.SetNum(NewStride * InstanceData.Num());

		for (int32 i = 0; i < InstanceData.Num(); ++i)
		{
			const int32 SrcCount = InstanceData[i].CustomData.Num();
			float* Dst = &ISMC->PerInstanceSMCustomData[i * NewStride];
			if (SrcCount > 0)
			{
				FMemory::Memcpy(Dst, InstanceData[i].CustomData.GetData(), FMath::Min(NewStride, SrcCount) * sizeof(float));
			}
			else
			{
				FMemory::Memset(Dst, 0, NewStride * sizeof(float));
			}
		}
	}
}

// ============================================================================
// World Partition helpers
// ============================================================================

int32 GetWorldPartitionCellSize(const UWorldPartitionEditorSpatialHash* SpatialHash)
{
	int32 DefaultWPCellSize = 25600;
	if (!SpatialHash)
	{
		return DefaultWPCellSize;
	}

	FIntProperty* CellSizeProp = CastField<FIntProperty>(UWorldPartitionEditorSpatialHash::StaticClass()->FindPropertyByName(TEXT("CellSize")));
	if (!CellSizeProp)
	{
		return DefaultWPCellSize;
	}

	return CellSizeProp->GetPropertyValue(CellSizeProp->ContainerPtrToValuePtr<int32>(SpatialHash));
}

AActor* FindOrCreateAggregateActor(UWorld* World, const FString& Label, const TArray<const UDataLayerAsset*>& DataLayers, const TMultiMap<FString, AActor*>& ExistingByLabel, FName RuntimeGrid, ULevel* OverrideLevel, const FVector& SpawnLocation)
{
	AActor* AggregateActor = nullptr;

	// Lookup via the pre-built multi-map (handles multiple aggregates with the same label but different data layers)
	TArray<AActor*> Candidates;
	ExistingByLabel.MultiFind(Label, Candidates);
	for (AActor* Candidate : Candidates)
	{
		AggregateActor = Candidate;
		// Safeguard to ensure correct Static Mobility
		if (AggregateActor->GetRootComponent()->Mobility != EComponentMobility::Static)
		{
			AggregateActor->GetRootComponent()->SetMobility(EComponentMobility::Static);
		}

		// Verify that data layers match, otherwise try the next candidate
		const TArray<const UDataLayerAsset*> CurrentLayers = AggregateActor->GetDataLayerAssets();
		bool bDataLayersMatch = (CurrentLayers.Num() == DataLayers.Num());
		if (bDataLayersMatch)
		{
			for (const UDataLayerAsset* DL : DataLayers)
			{
				if (!CurrentLayers.Contains(DL))
				{
					bDataLayersMatch = false;
					break;
				}
			}
		}

		// Verify that RuntimeGrid matches, otherwise try the next candidate
		bool bRuntimeGridMatch = (AggregateActor->GetRuntimeGrid() == RuntimeGrid);

		if (bDataLayersMatch && bRuntimeGridMatch)
		{
			return AggregateActor;
		}

		AggregateActor = nullptr;
	}

	// Create a new one in the same sublevel as the source actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.bCreateActorPackage = (World->GetWorldPartition() != nullptr);
	SpawnParams.OverrideLevel = OverrideLevel;
	AggregateActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnParams);
	AggregateActor->SetActorLabel(Label);
	AggregateActor->Tags.AddUnique(TEXT("SeamlessInstanceActor"));

	// Assign the Runtime Grid
	if (RuntimeGrid != NAME_None)
	{
		AggregateActor->SetRuntimeGrid(RuntimeGrid);
	}

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

	// Ensure every aggregate actor has a root component so we can attach ISMCs
	if (!AggregateActor->GetRootComponent())
	{
		USceneComponent* Root = NewObject<USceneComponent>(AggregateActor);
		Root->SetFlags(RF_Transactional);
		Root->CreationMethod = EComponentCreationMethod::Instance;
		Root->Mobility = EComponentMobility::Static;
		AggregateActor->SetRootComponent(Root);
		Root->SetWorldLocation(SpawnLocation);
		Root->RegisterComponent();
	}

	return AggregateActor;
}

#undef LOCTEXT_NAMESPACE

// ============================================================================
// Instance fingerprint (used to detect unedited round-trips)
// ============================================================================

uint32 ComputeActorFingerprint(AActor* Actor)
{
	if (!Actor)
	{
		return 0;
	}

	FBufferArchive Ar;

	// Hash all ISM components and their instances
	{
		TArray<UInstancedStaticMeshComponent*> ISMCs;
		Actor->GetComponents(ISMCs);

		int32 NumISMCs = ISMCs.Num();
		Ar << NumISMCs;

		for (const UInstancedStaticMeshComponent* ISMC : ISMCs)
		{
			if (!ISMC)
			{
				continue;
			}

			// Mesh path
			UStaticMesh* Mesh = ISMC->GetStaticMesh();
			FString MeshPath = Mesh ? Mesh->GetPathName() : FString();
			Ar << MeshPath;

			// Number of instances on this component
			int32 NumInstances = ISMC->GetInstanceCount();
			Ar << NumInstances;

			// Each instance's world transform
			for (int32 i = 0; i < NumInstances; ++i)
			{
				FTransform InstanceTransform;
				if (ISMC->GetInstanceTransform(i, InstanceTransform, /*bWorldSpace=*/true))
				{
					Ar << InstanceTransform;
				}
			}
		}
	}

	// Actor-level world transform
	{
		FTransform WorldTransform = Actor->ActorToWorld();
		Ar << WorldTransform;
	}

	// Runtime grid
	{
		FName RuntimeGrid = Actor->GetRuntimeGrid();
		Ar << RuntimeGrid;
	}

	// Actor layers (classic layer system)
	{
		const TArray<FName>& ActorLayers = Actor->Layers;
		int32 NumLayers = ActorLayers.Num();
		Ar << NumLayers;
		for (const FName& Layer : ActorLayers)
		{
			FString LayerStr = Layer.ToString();
			Ar << LayerStr;
		}
	}

	// Data layers
	{
		const TArray<const UDataLayerAsset*> DataLayers = Actor->GetDataLayerAssets();
		int32 NumDLs = DataLayers.Num();
		Ar << NumDLs;
		for (const UDataLayerAsset* DL : DataLayers)
		{
			FString DLPath = DL ? DL->GetPathName() : FString();
			Ar << DLPath;
		}
	}

	return FCrc::MemCrc32(Ar.GetData(), Ar.Num());
}
