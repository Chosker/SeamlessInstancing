// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingEditorSubsystem.h"
#include "SeamlessInstancingEditorModule.h"
#include "SeamlessInstancingHelpers.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Selection.h"
#include "MouseDeltaTracker.h"
#include "LevelEditorSubsystem.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorSpatialHash.h"
#include "Engine/StaticMeshActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "EditorViewportClient.h"
#include "SceneView.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "SeamlessInstancing"

// ============================================================================
// Subsystem implementation
// ============================================================================

void USeamlessInstancingEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (GEditor)
	{
		Collection.InitializeDependency<ULevelEditorSubsystem>();

		// Cache the toggle from config
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

		TryBindSelectionEvents();
	}

	// Start continuous ticker for detecting box-selection drags
	SelectionTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USeamlessInstancingEditorSubsystem::TickSelectionCheck));

	UE_LOG(LogSeamlessInstancing, Log, TEXT("SeamlessInstancingEditorSubsystem initialized."));
}

void USeamlessInstancingEditorSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	TickerHandle.Reset();

	FTSTicker::GetCoreTicker().RemoveTicker(SelectionTickerHandle);
	SelectionTickerHandle.Reset();

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

	UE_LOG(LogSeamlessInstancing, Log, TEXT("SeamlessInstancingEditorSubsystem deinitialized."));
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
		if (!SMC || !SMC->GetStaticMesh())
		{
			continue;
		}
		if (SMC->Mobility != EComponentMobility::Static)
		{
			continue;
		}
		if (!SMActor->GetIsSpatiallyLoaded())
		{
			continue;
		}
		if (SMActor->IsHiddenEd())
		{
			continue;
		}
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

	TMap<FString, AActor*> ExistingAggregateActors;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(TEXT("SeamlessInstanceActor")))
		{
			ExistingAggregateActors.Add(It->GetActorLabel(), *It);
		}
	}

	// Check whether to split by WP cell and get cell size
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
				const int32 CellSize = GetWorldPartitionCellSize(EditorSpatialHash);

				for (AStaticMeshActor* SMActor : ActorsToConvert)
				{
					UWorldPartitionEditorSpatialHash::FCellCoord Cell = EditorSpatialHash->GetCellCoords(SMActor->GetActorLocation(), 0);
					FCachedCellCoord CacheKey{Cell.X, Cell.Y};
					ActorToCell.Add(SMActor, CacheKey);

					AActor*& Found = CellToAggregate.FindOrAdd(CacheKey);
					if (!Found)
					{
						FString Label = FString::Printf(TEXT("SeamlessInstanceActor_%lld_%lld"), Cell.X, Cell.Y);
						Found = FindOrCreateAggregateActor(World, Label, SMActor->GetDataLayerAssets(), ExistingAggregateActors);

						// Center aggregate on its WP tile
						Found->SetActorLocation(FVector(double(Cell.X) * CellSize + CellSize * 0.5,
														double(Cell.Y) * CellSize + CellSize * 0.5,
														0.0));
					}
				}
			}
		}
	}

	if (!bUseCellGrouping)
	{
		// Single aggregate for non-WP or non-streaming worlds
		AActor* SingleActor = FindOrCreateAggregateActor(World, TEXT("SeamlessInstanceActor"), {}, ExistingAggregateActors);
		CellToAggregate.Add(FCachedCellCoord{0, 0}, SingleActor);
	}

	TArray<AActor*> ActorsToDestroy;
	GEditor->BeginTransaction(LOCTEXT("ConvertSMToInstanced", "Convert SM Actors to Instanced"));

	for (AStaticMeshActor* SMActor : ActorsToConvert)
	{
		const FCachedCellCoord CellKey = bUseCellGrouping ? ActorToCell[SMActor] : FCachedCellCoord{0, 0};
		AActor* AggregateActor = CellToAggregate[CellKey];

		if (SMActor == AggregateActor)
		{
			continue;
		}

		UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
		SMActor->Modify();

		FInstanceGroupKey InstanceKey;
		InstanceKey.Mesh = SMC->GetStaticMesh();
		InstanceKey.PropertiesHash = HashComponentProperties(SMC, RelevantProperties);

		// Scan the aggregate for an existing ISMC with matching mesh and properties
		UInstancedStaticMeshComponent* ISMC = nullptr;
		{
			TArray<UInstancedStaticMeshComponent*> ExistingISMCs;
			AggregateActor->GetComponents(ExistingISMCs);

			const FName IncomingHashTag = FName(*FString::Printf(TEXT("SrcHash_%u"), InstanceKey.PropertiesHash));
			for (UInstancedStaticMeshComponent* Existing : ExistingISMCs)
			{
				if (Existing->GetStaticMesh() == InstanceKey.Mesh && Existing->ComponentTags.Contains(IncomingHashTag))
				{
					ISMC = Existing;
					break;
				}
			}
		}

		if (!ISMC)
		{
			const FName ISMCName = *FString::Printf(TEXT("ISMC_%s_%u"), *InstanceKey.Mesh->GetName(), InstanceKey.PropertiesHash);
			ISMC = NewObject<UInstancedStaticMeshComponent>(AggregateActor, ISMCName, RF_Transactional);
			ISMC->SetStaticMesh(InstanceKey.Mesh);
			ISMC->SetupAttachment(AggregateActor->GetRootComponent());
			ISMC->bHasPerInstanceHitProxies = true;
			AggregateActor->AddInstanceComponent(ISMC);
			ISMC->RegisterComponent();

			// Copy all included properties from source SMC onto the new ISMC
			for (FProperty* Prop : RelevantProperties)
			{
				if (!ShouldInclude(Prop))
				{
					continue;
				}
				Prop->CopyCompleteValue_InContainer(ISMC, SMC);
			}

			// Restore bHasPerInstanceHitProxies as it has been overwritten
			ISMC->bHasPerInstanceHitProxies = true;

			// Stamp the source properties hash so future calls can find this ISMC
			ISMC->ComponentTags.Add(FName(*FString::Printf(TEXT("SrcHash_%u"), InstanceKey.PropertiesHash)));
		}

		const int32 NewInstanceIndex = ISMC->AddInstance(SMActor->GetTransform(), /*bWorldSpace=*/true);

		// Transfer the source SMC's CustomPrimitiveData to PerInstanceCustomData on the ISM
		const TArray<float>& SrcCustomData = SMC->GetCustomPrimitiveData().Data;
		const int32 NumSrcFloats = SrcCustomData.Num();
		if (NumSrcFloats > 0)
		{
			if (NumSrcFloats > ISMC->NumCustomDataFloats)
			{
				// If the new instance has more CustomData floats than the current stride expand NumCustomDataFloats and re-layout existing PerInstanceData
				const int32 OldNumFloats = ISMC->NumCustomDataFloats;
				const int32 NewNumFloats = NumSrcFloats;
				const int32 NumExistingInstances = ISMC->GetInstanceCount() - 1;

				TArray<float> ExpandedData;
				ExpandedData.SetNum(NewNumFloats * ISMC->GetInstanceCount());
				for (int32 InstIdx = 0; InstIdx < NumExistingInstances; ++InstIdx)
				{
					FMemory::Memcpy(
						&ExpandedData[InstIdx * NewNumFloats],
						&ISMC->PerInstanceSMCustomData[InstIdx * OldNumFloats],
						OldNumFloats * sizeof(float)
					);
				}
				ISMC->PerInstanceSMCustomData = MoveTemp(ExpandedData);
				ISMC->NumCustomDataFloats = NewNumFloats;
			}
			else
			{
				// Same or fewer floats: ensure the array has room for the new instance
				if (ISMC->NumCustomDataFloats == 0)
				{
					ISMC->NumCustomDataFloats = NumSrcFloats;
				}
				const int32 RequiredSize = ISMC->NumCustomDataFloats * ISMC->GetInstanceCount();
				if (ISMC->PerInstanceSMCustomData.Num() < RequiredSize)
				{
					ISMC->PerInstanceSMCustomData.AddZeroed(RequiredSize - ISMC->PerInstanceSMCustomData.Num());
				}
			}

			const int32 DataCount = FMath::Min(NumSrcFloats, ISMC->NumCustomDataFloats);
			ISMC->SetCustomData(NewInstanceIndex, TArrayView<const float>(SrcCustomData.GetData(), DataCount), false);
		}
		ActorsToDestroy.Add(SMActor);
	}

	for (AActor* Actor : ActorsToDestroy)
	{
		World->DestroyActor(Actor);
	}

	GEditor->EndTransaction();

	// Refresh the World Outliner
	GEditor->BroadcastLevelActorListChanged();
}

void USeamlessInstancingEditorSubsystem::ConvertAllInstancedToSM()
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	TArray<AActor*> AggregatesToConvert;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(TEXT("SeamlessInstanceActor")))
		{
			continue;
		}
		AActor* Aggregate = *It;
		TArray<UInstancedStaticMeshComponent*> ISMCs;
		Aggregate->GetComponents(ISMCs);
		if (ISMCs.IsEmpty())
		{
			continue;
		}
		AggregatesToConvert.Add(Aggregate);
	}

	if (AggregatesToConvert.IsEmpty())
	{
		return;
	}

	ConvertInstancedToSM(AggregatesToConvert);
}

void USeamlessInstancingEditorSubsystem::ConvertInstancedToSM(const TArray<AActor*>& AggregatesToConvert)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	const TArray<FProperty*> RelevantProperties = GatherProperties();

	// Collect existing actor labels so we can make unique names
	TSet<FString> ExistingLabels;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		ExistingLabels.Add(It->GetActorLabel());
	}

	GEditor->BeginTransaction(LOCTEXT("ConvertInstancedToSM", "Convert Instanced to SM Actors"));

	TArray<AActor*> AggregatesToDestroy;
	for (AActor* Aggregate : AggregatesToConvert)
	{
		TArray<UInstancedStaticMeshComponent*> ISMCs;
		Aggregate->GetComponents(ISMCs);

		for (UInstancedStaticMeshComponent* ISMC : ISMCs)
		{
			UStaticMesh* Mesh = ISMC->GetStaticMesh();
			if (!Mesh)
			{
				continue;
			}

			const int32 NumInstances = ISMC->GetInstanceCount();
			for (int32 i = 0; i < NumInstances; ++i)
			{
				FTransform InstanceTransform;
				if (!ISMC->GetInstanceTransform(i, InstanceTransform, /*bWorldSpace=*/true))
				{
					continue;
				}

				AStaticMeshActor* SMActor = World->SpawnActor<AStaticMeshActor>();
				SMActor->SetActorTransform(InstanceTransform);
				UStaticMeshComponent* NewSMC = SMActor->GetStaticMeshComponent();
				NewSMC->SetStaticMesh(Mesh);

				// Set the actor label to the mesh name (made unique)
				{
					FString BaseLabel = Mesh->GetName();
					FString FinalLabel = BaseLabel;
					int32 Suffix = 1;
					while (ExistingLabels.Contains(FinalLabel))
					{
						FinalLabel = FString::Printf(TEXT("%s_%d"), *BaseLabel, Suffix++);
					}
					SMActor->SetActorLabel(FinalLabel);
					ExistingLabels.Add(FinalLabel);
				}

				SMActor->Modify();

				// Copy included properties from the ISMC onto the new SMC
				for (FProperty* Prop : RelevantProperties)
				{
					if (!ShouldInclude(Prop))
					{
						continue;
					}
					Prop->CopyCompleteValue_InContainer(NewSMC, ISMC);
				}

				// Copy PerInstanceCustomData from the ISMC onto the new SMC's CustomPrimitiveData
				if (ISMC->NumCustomDataFloats > 0)
				{
					const float* InstanceDataStart = &ISMC->PerInstanceSMCustomData[i * ISMC->NumCustomDataFloats];
					NewSMC->SetDefaultCustomPrimitiveDataFloatArray(0, MakeConstArrayView(InstanceDataStart, ISMC->NumCustomDataFloats));
				}

				NewSMC->MarkRenderStateDirty();
			}
		}

		AggregatesToDestroy.Add(Aggregate);
	}

	for (AActor* Aggregate : AggregatesToDestroy)
	{
		World->DestroyActor(Aggregate);
	}

	GEditor->EndTransaction();

	// Refresh the World Outliner
	GEditor->BroadcastLevelActorListChanged();
}

void USeamlessInstancingEditorSubsystem::SetSeamlessEnabled(bool bEnabled)
{
	bCachedSeamlessEnabled = bEnabled;
	GConfig->SetBool(TEXT("SeamlessInstancing"), TEXT("bEnableSeamless"), bEnabled, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

// ============================================================================
// Lazy binding
// ============================================================================

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

			UE_LOG(LogSeamlessInstancing, Log, TEXT("SeamlessInstancingEditorSubsystem: selection events bound."));
			return;
		}
	}

	// Selection set not ready yet, retry via ticker
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &USeamlessInstancingEditorSubsystem::TickBindRetry), 0.5f);
	}
}

bool USeamlessInstancingEditorSubsystem::TickBindRetry(float DeltaTime)
{
	TryBindSelectionEvents();
	return !bSelectionEventsBound;
}

// ============================================================================
// Tick-based rect capture
// ============================================================================

bool USeamlessInstancingEditorSubsystem::TickSelectionCheck(float DeltaTime)
{
	if (!IsSeamlessEnabled() || !GEditor)
	{
		return true;
	}

	FViewport* ActiveViewport = GEditor->GetActiveViewport();
	if (!ActiveViewport)
	{
		return true;
	}

	FEditorViewportClient* EditorVC = static_cast<FEditorViewportClient*>(ActiveViewport->GetClient());
	if (!EditorVC)
	{
		return true;
	}

	FMouseDeltaTracker* MouseTracker = EditorVC->GetMouseDeltaTracker();
	if (!MouseTracker)
	{
		return true;
	}

	// Continuously rebuild the rect from live tracker state during any active drag
	if (MouseTracker->UsingDragTool())
	{
		FIntPoint Start = FIntPoint(FMath::FloorToInt32(MouseTracker->GetDragStartPos().X), FMath::FloorToInt32(MouseTracker->GetDragStartPos().Y));
		FIntPoint End;
		ActiveViewport->GetMousePos(End);
		const FIntPoint MinPt(FMath::Min(Start.X, End.X), FMath::Min(Start.Y, End.Y));
		const FIntPoint MaxPt(FMath::Max(Start.X, End.X), FMath::Max(Start.Y, End.Y));

		CachedSelectionRect = FIntRect(MinPt, MaxPt);

		// Clamp to screen
		CachedSelectionRect.Min.X = FMath::Max(CachedSelectionRect.Min.X, 0);
		CachedSelectionRect.Min.Y = FMath::Max(CachedSelectionRect.Min.Y, 0);
		CachedSelectionRect.Max.X = FMath::Min(CachedSelectionRect.Max.X, ActiveViewport->GetSizeXY().X);
		CachedSelectionRect.Max.Y = FMath::Min(CachedSelectionRect.Max.Y, ActiveViewport->GetSizeXY().Y);

		/*UE_LOG(LogSeamlessInstancing, Log,
			TEXT("TickRect: MouseTracker.Start=(%f,%f) Delta=(%f,%f) Cached=(%d,%d)-(%d,%d) MousePos=(%d,%d) ViewSize=%s"),
			Start.X, Start.Y,
			MouseTracker->GetAbsoluteDelta().X, MouseTracker->GetAbsoluteDelta().Y,
			CachedSelectionRect.Min.X, CachedSelectionRect.Min.Y,
			CachedSelectionRect.Max.X, CachedSelectionRect.Max.Y,
			MousePos.X, MousePos.Y,
			*ActiveViewport->GetSizeXY().ToString());*/
	}
	// Discard if rect < MOUSE_CLICK_DRAG_DELTA
	else if (CachedSelectionRect.Width() < 4 || CachedSelectionRect.Height() < 4)
	{
		CachedSelectionRect = FIntRect();
	}

	return true;
}

// ============================================================================
// Selection-change handler
// ============================================================================

void USeamlessInstancingEditorSubsystem::OnSelectionChanged(const UTypedElementSelectionSet* SelectionSet)
{
	if (!IsSeamlessEnabled())
	{
		return;
	}

	if (bIsConverting)
	{
		return;
	}

	// Consume the rect captured by TickSelectionCheck during the most recent drag.
	const FIntRect SelRect = CachedSelectionRect;
	CachedSelectionRect = FIntRect();

	// Snapshot the current selection set
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

	// Find actors that were selected before but aren't anymore (SM -> ISM)
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
			if (SMActor->IsHiddenEd())
			{
				continue;
			}
			UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
			if (SMC && SMC->GetStaticMesh() && SMC->Mobility == EComponentMobility::Static && SMActor->GetIsSpatiallyLoaded())
			{
				ActorsToConvert.Add(SMActor);
			}
		}
	}

	// Find newly selected aggregates (ISM -> SM)
	TArray<AActor*> NewlySelectedAggregates;
	for (const TWeakObjectPtr<AActor>& CurrentActor : CurrentSelection)
	{
		AActor* Actor = CurrentActor.Get();
		if (!Actor)
		{
			continue;
		}
		if (Actor->Tags.Contains(TEXT("SeamlessInstanceActor")) && !PreviousSelectedActors.Contains(Actor))
		{
			NewlySelectedAggregates.Add(Actor);
		}
	}

	// Update tracking before converting
	PreviousSelectedActors = CurrentSelection;

	if (ActorsToConvert.IsEmpty() && NewlySelectedAggregates.IsEmpty())
	{
		return;
	}

	TGuardValue<bool> Guard(bIsConverting, true);

	// Convert deselected SM actors to ISM instances
	if (!ActorsToConvert.IsEmpty())
	{
		ConvertSMToInstanced(ActorsToConvert);
	}

	// Convert clicked ISM instances back to SM actors
	if (!NewlySelectedAggregates.IsEmpty())
	{
		for (AActor* Aggregate : NewlySelectedAggregates)
		{
			// Detect box/frustum selection first using the cached selection-rect
			bool bSelectionHandled = false;

			if (GEditor)
			{
				if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
				{
					if (SelRect.Width() > 0 && SelRect.Height() > 0)
					{
						/*UE_LOG(LogSeamlessInstancing, Log, TEXT("OnSelectionChanged: Rect(%d,%d,%d,%d) ViewSize(%d,%d)"),
							SelRect.Min.X, SelRect.Min.Y, SelRect.Max.X, SelRect.Max.Y,
							ActiveViewport->GetSizeXY().X, ActiveViewport->GetSizeXY().Y);*/

						// Use the viewport's hit-proxy rect query
						TArray<TPair<UInstancedStaticMeshComponent*, int32>> Selected = FindSelectionInstances(ActiveViewport, Aggregate, SelRect);

						// DEBUG: draw a 3D box matching the viewport selection rect
						/*if (FEditorViewportClient* EditorVC = static_cast<FEditorViewportClient*>(ActiveViewport->GetClient()))
						{
							if (UWorld* World = GEditor->GetEditorWorldContext().World())
							{
								FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(ActiveViewport, EditorVC->GetScene(), EditorVC->EngineShowFlags).SetRealtimeUpdate(EditorVC->IsRealtime()));

								if (FSceneView* View = EditorVC->CalcSceneView(&ViewFamily))
								{
									// Build an oriented debug box that visually matches the screen rect
									const FVector ViewLocation = View->ViewMatrices.GetViewOrigin();
									const FVector ViewForward = View->GetViewDirection();
									const FIntRect ViewRect = View->UnscaledViewRect;
									const double CornerDistance = 1000.0;

									auto UnprojectToFrustum = [&](float PixelX, float PixelY, FVector& OutPoint) -> bool
									{
										FVector RayOrigin, RayDir;
										View->DeprojectFVector2D(FVector2D(PixelX, PixelY), RayOrigin, RayDir);
										const double Forward = FVector::DotProduct(RayDir, ViewForward);
										if (FMath::Abs(Forward) < UE_KINDA_SMALL_NUMBER)
										{
											return false;
										}
										OutPoint = RayOrigin + RayDir * (CornerDistance / Forward);
										return true;
									};

									FVector TL, TR, BR, BL;
									if (!UnprojectToFrustum(SelRect.Min.X, SelRect.Min.Y, TL) ||
										!UnprojectToFrustum(SelRect.Max.X, SelRect.Min.Y, TR) ||
										!UnprojectToFrustum(SelRect.Max.X, SelRect.Max.Y, BR) ||
										!UnprojectToFrustum(SelRect.Min.X, SelRect.Max.Y, BL))
									{
										return;
									}

									const FVector BoxCenter = (TL + TR + BR + BL) * 0.25;
									const FVector ScreenRight = ((TR - TL) + (BR - BL)).GetSafeNormal();
									const FVector ScreenUp = ((BL - TL) + (BR - TR)).GetSafeNormal();
									const FVector ScreenForward = FVector::CrossProduct(ScreenRight, ScreenUp).GetSafeNormal();

									// Per-edge depths (along ViewForward)
									const double DepthTL = FVector::DotProduct(TL - ViewLocation, ViewForward);
									const double DepthTR = FVector::DotProduct(TR - ViewLocation, ViewForward);
									const double DepthBL = FVector::DotProduct(BL - ViewLocation, ViewForward);
									const double DepthBR = FVector::DotProduct(BR - ViewLocation, ViewForward);

									auto HarmonicMean = [](double A, double B) -> double
									{
										return (FMath::Abs(A + B) > UE_KINDA_SMALL_NUMBER)
											? (2.0 * A * B) / (A + B)
											: (A + B) * 0.5;
									};
									const double WidthDepth  = HarmonicMean(HarmonicMean(DepthTL, DepthTR), HarmonicMean(DepthBL, DepthBR));
									const double HeightDepth = HarmonicMean(HarmonicMean(DepthTL, DepthBL), HarmonicMean(DepthTR, DepthBR));
									const double EdgeWidthDepth  = (DepthTL + DepthTR) * 0.5;
									const double EdgeHeightDepth = (DepthTL + DepthBL) * 0.5;
									const double WidthScale  = (EdgeWidthDepth  > UE_KINDA_SMALL_NUMBER) ? WidthDepth  / EdgeWidthDepth  : 1.0;
									const double HeightScale = (EdgeHeightDepth > UE_KINDA_SMALL_NUMBER) ? HeightDepth / EdgeHeightDepth : 1.0;

									const double HalfWidth  = ((TR - TL).Size() + (BR - BL).Size()) * 0.25 * WidthScale;
									const double HalfHeight = ((BL - TL).Size() + (BR - TR).Size()) * 0.25 * HeightScale;
									const double HalfDepth = 0.1f;

									const FVector Extent(FMath::Max(HalfWidth, 0.1), FMath::Max(HalfHeight, 0.1), HalfDepth);
									const FQuat BoxRotation = FQuat(FMatrix(
										FPlane(ScreenRight.X,   ScreenRight.Y,   ScreenRight.Z,   0),
										FPlane(ScreenUp.X,      ScreenUp.Y,      ScreenUp.Z,      0),
										FPlane(ScreenForward.X, ScreenForward.Y, ScreenForward.Z, 0),
										FPlane(0, 0, 0, 1)
									));

									DrawDebugBox(World, BoxCenter, Extent, BoxRotation, FColor::Red, false, 100.0f, SDPG_Foreground, 0.0f);
								}
							}
						}*/

						if (!Selected.IsEmpty())
						{
							// Break in inverse index order per ISMC so removal doesn't invalidate indices of instances we haven't processed yet
							Selected.Sort([](const TPair<UInstancedStaticMeshComponent*, int32>& A, const TPair<UInstancedStaticMeshComponent*, int32>& B)
							{
								if (A.Key != B.Key)
								{
									return A.Key < B.Key;
								}
								return A.Value > B.Value;
							});

							for (const TPair<UInstancedStaticMeshComponent*, int32>& Sel : Selected)
							{
								if (IsValid(Sel.Key) && IsValid(Aggregate))
								{
									//UE_LOG(LogSeamlessInstancing, Log, TEXT("FindSelectionInstances BreakInstance: %d"), Sel.Value);
									BreakInstance(Sel.Key, Sel.Value);
								}
							}

							bSelectionHandled = true;
						}
					}
				}
			}

			// If no drag tool was active, handle as a single-instance click
			if (!bSelectionHandled)
			{
				int32 InstanceIndex = INDEX_NONE;
				UInstancedStaticMeshComponent* HitISMC = nullptr;
				if (FindClickedInstance(Aggregate, InstanceIndex, HitISMC))
				{
					UE_LOG(LogSeamlessInstancing, Log, TEXT("FindClickedInstance BreakInstance: %d"), InstanceIndex);
					BreakInstance(HitISMC, InstanceIndex);
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
