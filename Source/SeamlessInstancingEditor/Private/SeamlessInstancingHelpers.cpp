// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingHelpers.h"

#include "Serialization/BufferArchive.h"
#include "Misc/Crc.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorSpatialHash.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"

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
	// Lookup via the pre-built map
	if (const AActor* const* Found = ExistingByLabel.Find(Label))
	{
		return const_cast<AActor*>(*Found);
	}

	// Create a new one
	FActorSpawnParameters SpawnParams;
	SpawnParams.bCreateActorPackage = (World->GetWorldPartition() != nullptr);
	AActor* AggregateActor = World->SpawnActor<AActor>(SpawnParams);
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
		AggregateActor->SetRootComponent(Root);
		Root->RegisterComponent();
	}

	return AggregateActor;
}
