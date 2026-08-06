// Copyright Epic Games, Inc. All Rights Reserved.

#include "GPULODFoliageInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "InstancedStaticMesh/ISMInstanceDataManager.h"

UGPULODFoliageInstancedStaticMeshComponent::UGPULODFoliageInstancedStaticMeshComponent()
{
	// Set to Default akin to InstancedStaticMeshComponent
	PrimitiveInstanceDataManager.SetMode(FPrimitiveInstanceDataManager::EMode::Default);
	bUseGpuLodSelection = 1;
}

FPrimitiveSceneProxy* UGPULODFoliageInstancedStaticMeshComponent::CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite)
{
	// InstancedStaticMeshComponent behavior
	return UInstancedStaticMeshComponent::CreateStaticMeshSceneProxy(NaniteMaterials, bCreateNanite);
}

void UGPULODFoliageInstancedStaticMeshComponent::BuildComponentInstanceData(EShaderPlatform InShaderPlatform, FInstanceUpdateComponentDesc& OutData)
{
	// InstancedStaticMeshComponent behavior
	UInstancedStaticMeshComponent::BuildComponentInstanceData(InShaderPlatform, OutData);
}

void UGPULODFoliageInstancedStaticMeshComponent::BuildTree()
{
	// InstancedStaticMeshComponent behavior, no cluster tree
}

void UGPULODFoliageInstancedStaticMeshComponent::BuildTreeAsync()
{
	// InstancedStaticMeshComponent behavior, No cluster tree
}
