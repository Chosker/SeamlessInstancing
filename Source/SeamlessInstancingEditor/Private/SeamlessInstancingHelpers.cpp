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
		UE_LOG(LogSeamlessInstancing, Log, TEXT("FindClickedInstance: unexpected HitProxy type \"%s\" at (%d,%d)"), HitProxy->GetType()->GetName(), MouseX, MouseY);
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

	// Deselect the aggregate before breaking the instance to avoid rendering its outline
	if (GEditor)
	{
		GEditor->SelectActor(Aggregate, /*bSelected=*/false, /*bNotify=*/true);
	}

	if (bBeginTransaction)
	{
		GEditor->BeginTransaction(LOCTEXT("BreakInstance", "Break Instance"));
	}

	AStaticMeshActor* NewSMActor = World->SpawnActor<AStaticMeshActor>();
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

	NewSMC->MarkRenderStateDirty();

	// Remove the instance from the ISMC
	ISMC->RemoveInstance(InstanceIndex);

	// Clean up empty ISMCs and if needed, the aggregate itself
	if (ISMC->GetInstanceCount() == 0)
	{
		ISMC->DestroyComponent();

		TArray<UInstancedStaticMeshComponent*> RemainingISMCs;
		Aggregate->GetComponents(RemainingISMCs);
		if (RemainingISMCs.IsEmpty())
		{
			World->DestroyActor(Aggregate);
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

AActor* FindOrCreateAggregateActor(UWorld* World, const FString& Label, const TArray<const UDataLayerAsset*>& DataLayers, const TMap<FString, AActor*>& ExistingByLabel)
{
	AActor* AggregateActor = nullptr;

	// Lookup via the pre-built map
	if (const AActor* const* Found = ExistingByLabel.Find(Label))
	{
		AggregateActor = const_cast<AActor*>(*Found);
		// Safeguard to ensure correct Static Mobility
		if (AggregateActor->GetRootComponent()->Mobility != EComponentMobility::Static)
		{
			AggregateActor->GetRootComponent()->SetMobility(EComponentMobility::Static);
		}

		// Verify that data layers match, otherwise create a new Aggregate
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
		if (bDataLayersMatch)
		{
			return AggregateActor;
		}

		AggregateActor = nullptr;
	}

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

	// Ensure every aggregate actor has a root component so we can attach ISMCs
	if (!AggregateActor->GetRootComponent())
	{
		USceneComponent* Root = NewObject<USceneComponent>(AggregateActor);
		Root->SetFlags(RF_Transactional);
		Root->CreationMethod = EComponentCreationMethod::Instance;
		Root->Mobility = EComponentMobility::Static;
		AggregateActor->SetRootComponent(Root);
		Root->RegisterComponent();
	}

	return AggregateActor;
}

#undef LOCTEXT_NAMESPACE
