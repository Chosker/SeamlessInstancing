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

		ISMC->AddInstance(SMActor->GetTransform(), /*bWorldSpace=*/true);
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

	FMouseDeltaTracker* MDT = EditorVC->GetMouseDeltaTracker();
	if (!MDT)
	{
		return true;
	}

	// Continuously rebuild the rect from live tracker state during any active drag
	if (MDT->UsingDragTool())
	{
		const FVector Start = MDT->GetDragStartPos();
		const FVector End   = Start + MDT->GetAbsoluteDelta() * FVector(1, -1, 1);

		const FIntPoint MinPt(FMath::FloorToInt32(FMath::Min(Start.X, End.X)),
							  FMath::FloorToInt32(FMath::Min(Start.Y, End.Y)));
		const FIntPoint MaxPt(FMath::FloorToInt32(FMath::Max(Start.X, End.X)),
							  FMath::FloorToInt32(FMath::Max(Start.Y, End.Y)));
		CachedSelectionRect = FIntRect(MinPt, MaxPt);
		
		// Clamp to screen
		CachedSelectionRect.Min.X = FMath::Max(CachedSelectionRect.Min.X, 0);
		CachedSelectionRect.Min.Y = FMath::Max(CachedSelectionRect.Min.Y, 0);
		CachedSelectionRect.Max.X = FMath::Min(CachedSelectionRect.Max.X, ActiveViewport->GetSizeXY().X);
		CachedSelectionRect.Max.Y = FMath::Min(CachedSelectionRect.Max.Y, ActiveViewport->GetSizeXY().Y);

		//UE_LOG(LogSeamlessInstancing, Log, TEXT("TickSelectionCheck: Start(%f, %f), Delta(%f, %f), End(%f, %f), Cached (%d, %d)"),
		//	Start.X, Start.Y, MDT->GetAbsoluteDelta().X, MDT->GetAbsoluteDelta().Y, End.X, End.Y, CachedSelectionRect.Min.Y, CachedSelectionRect.Max.Y);
	}
	else if (CachedSelectionRect.Width() < 4 || CachedSelectionRect.Height() < 4)
	{
		// No drag in progress and the cached rect is degenerate — discard so the next
		// OnSelectionChanged falls through to the single-click path. The 4-px floor
		// matches the engine's own MOUSE_CLICK_DRAG_DELTA threshold and filters clicks
		// and synthetic drags (gizmo tweaks) that don't represent a real marquee.
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
						UE_LOG(LogSeamlessInstancing, Log, TEXT("OnSelectionChanged: Rect(%d,%d,%d,%d) ViewSize(%d,%d)"),
							SelRect.Min.X, SelRect.Min.Y, SelRect.Max.X, SelRect.Max.Y,
							ActiveViewport->GetSizeXY().X, ActiveViewport->GetSizeXY().Y);

						// Use the viewport's hit-proxy rect query
						TArray<TPair<UInstancedStaticMeshComponent*, int32>> Selected = FindSelectionInstances(ActiveViewport, Aggregate, SelRect);

						// DEBUG: draw a 3D box matching the viewport selection rect (visible for 100s)
						if (FEditorViewportClient* EditorVC = static_cast<FEditorViewportClient*>(ActiveViewport->GetClient()))
						{
							if (UWorld* World = GEditor->GetEditorWorldContext().World())
							{
								// Project the 4 pixel-space rect corners through the view's own
								// PixelToWorld path (same one FDragTool_BoxSelect::CalculateBox uses).
								// Z=0.5 picks mid-depth in NDC, so the resulting box sits in the
								// middle of the frustum and aligns with the rect at any camera
								// rotation — including steep pitches where world-up no longer
								// matches screen-up.
								FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
									ActiveViewport,
									EditorVC->GetScene(),
									EditorVC->EngineShowFlags)
									.SetRealtimeUpdate(EditorVC->IsRealtime()));

								if (FSceneView* View = EditorVC->CalcSceneView(&ViewFamily))
								{
									// Build an oriented debug box that visually matches the screen rect.
									// Use the engine's DeprojectScreenToWorld to get a world-space ray
									// (origin + direction) for each corner, then step a chosen world
									// distance along the ray to land each corner on a plane in front
									// of the camera. The box is camera-aligned (width = right, height
									// = up, depth = forward) so its screen-space silhouette matches
									// the marquee exactly.
									const FVector ViewLocation = View->ViewMatrices.GetViewOrigin();
									const FVector ViewForward = View->GetViewDirection();
									const FVector ViewRight = FVector::CrossProduct(FVector::UpVector, ViewForward).GetSafeNormal();
									const FVector ViewUp = FVector::CrossProduct(ViewForward, ViewRight).GetSafeNormal();
									const double CornerDistance = 1000.0;

									auto CornerAt = [&](float PixelX, float PixelY) -> FVector
									{
										FVector RayOrigin, RayDir;
										View->DeprojectFVector2D(FVector2D(PixelX, PixelY), RayOrigin, RayDir);
										return RayOrigin + RayDir * CornerDistance;
									};

									const FVector FarTL = CornerAt(SelRect.Min.X, SelRect.Min.Y);
									const FVector FarTR = CornerAt(SelRect.Max.X, SelRect.Min.Y);
									const FVector FarBL = CornerAt(SelRect.Min.X, SelRect.Max.Y);
									const FVector FarBR = CornerAt(SelRect.Max.X, SelRect.Max.Y);

									const FVector FarCenter = (FarTL + FarTR + FarBL + FarBR) * 0.25;

									// Half-extents in the view basis.
									const FVector LocalTL = FarTL - FarCenter;
									const FVector LocalTR = FarTR - FarCenter;
									const FVector LocalBL = FarBL - FarCenter;
									const FVector LocalBR = FarBR - FarCenter;
									const double HalfWidth  = FMath::Max<double>(FMath::Abs(FVector::DotProduct(LocalTR, ViewRight)) + FMath::Abs(FVector::DotProduct(LocalBR, ViewRight)), 1.0) * 0.5;
									const double HalfHeight = FMath::Max<double>(FMath::Abs(FVector::DotProduct(LocalBL, ViewUp))   + FMath::Abs(FVector::DotProduct(LocalBR, ViewUp)),   1.0) * 0.5;
									const double HalfDepth = 0.1f;

									const FVector BoxCenter = FarCenter;
									const FVector Extent(HalfWidth, HalfHeight, HalfDepth);
									const FQuat BoxRotation = FQuat(FMatrix(
										FPlane(ViewRight.X,   ViewRight.Y,   ViewRight.Z,   0),
										FPlane(ViewUp.X,      ViewUp.Y,      ViewUp.Z,      0),
										FPlane(ViewForward.X, ViewForward.Y, ViewForward.Z, 0),
										FPlane(0, 0, 0, 1)
									));

									UE_LOG(LogSeamlessInstancing, Log, TEXT("DebugBox: ViewLoc=%s Forward=%s FarTL=%s FarCenter=%s HalfW=%f HalfH=%f HalfD=%f CornerDist=%f"),
										*ViewLocation.ToString(), *ViewForward.ToString(),
										*FarTL.ToString(), *FarCenter.ToString(),
										HalfWidth, HalfHeight, HalfDepth, CornerDistance);

									/*DrawDebugBox(
										World,
										BoxCenter,
										Extent,
										BoxRotation,
										FColor::Red,
										false,    // bPersistentLines
										100.0f,   // LifeTime
										SDPG_Foreground, // DepthPriority — draw on top so scene geometry doesn't occlude the lines in perspective views
										0.0f      // Thickness (line thickness for visibility)
									);*/
								}
							}
						}

						if (!Selected.IsEmpty())
						{
							// Break in descending instance index order per ISMC so removal doesn't
							// invalidate indices of instances we haven't processed yet.
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
									UE_LOG(LogSeamlessInstancing, Log, TEXT("FindSelectionInstances BreakInstance: %d"), Sel.Value);
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
