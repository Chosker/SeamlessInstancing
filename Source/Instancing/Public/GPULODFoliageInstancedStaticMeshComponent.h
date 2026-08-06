// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "FoliageInstancedStaticMeshComponent.h"
#include "GPULODFoliageInstancedStaticMeshComponent.generated.h"

namespace Nanite { struct FMaterialAudit; }
struct FInstanceUpdateComponentDesc;
class FPrimitiveSceneProxy;

UCLASS(ClassGroup = Foliage, Blueprintable, meta = (BlueprintSpawnableComponent))
class INSTANCING_API UGPULODFoliageInstancedStaticMeshComponent : public UFoliageInstancedStaticMeshComponent
{
	GENERATED_BODY()

public:
	UGPULODFoliageInstancedStaticMeshComponent();

	virtual FPrimitiveSceneProxy* CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite) override;
	virtual void BuildComponentInstanceData(EShaderPlatform InShaderPlatform, FInstanceUpdateComponentDesc& OutData) override;

	virtual void BuildTree() override;
	virtual void BuildTreeAsync() override;
	virtual int32 GetNumRenderInstances() const override { return PerInstanceSMData.Num(); }
};
