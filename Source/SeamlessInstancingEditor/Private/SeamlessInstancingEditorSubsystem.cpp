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
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "EditorViewportClient.h"
#include "SceneView.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"

#define LOCTEXT_NAMESPACE "SeamlessInstancing"

// Subsystem implementation

void USeamlessInstancingEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (GEditor)
	{
		Collection.InitializeDependency<ULevelEditorSubsystem>();

		// Cache the toggle from config
		GConfig->GetBool(TEXT("SeamlessInstancing"), TEXT("bEnableSeamless"), bCachedSeamlessEnabled, GEditorPerProjectIni);
		{
			int32 CachedType = static_cast<int32>(ESeamlessComponentType::Auto);
			GConfig->GetInt(TEXT("SeamlessInstancing"), TEXT("ComponentType"), CachedType, GEditorPerProjectIni);
			ComponentType = static_cast<ESeamlessComponentType>(CachedType);
		}

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

	// Subscribe to PreSave to stamp aggregate actors with fingerprint tags
	PreSavePackageHandle = UPackage::PreSavePackageWithContextEvent.AddStatic(USeamlessInstancingEditorSubsystem::OnPreSavePackage);

	UE_LOG(LogSeamlessInstancing, Log, TEXT("SeamlessInstancingEditorSubsystem initialized."));
}

void USeamlessInstancingEditorSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	TickerHandle.Reset();

	FTSTicker::GetCoreTicker().RemoveTicker(SelectionTickerHandle);
	SelectionTickerHandle.Reset();

	if (PreSavePackageHandle.IsValid())
	{
		UPackage::PreSavePackageWithContextEvent.Remove(PreSavePackageHandle);
		PreSavePackageHandle.Reset();
	}

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

	TMultiMap<FString, AActor*> ExistingAggregateActors;
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
	TMultiMap<FString, AActor*> CellToAggregate;
	TMap<AStaticMeshActor*, FString> ActorToCell;
	TMap<AStaticMeshActor*, AActor*> ActorToAggregate;

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

					// Build a label that encodes cell coords and RuntimeGrid
					FString Label;
					FName RuntimeGrid = SMActor->GetRuntimeGrid();
					if (RuntimeGrid != NAME_None)
					{
						Label = FString::Printf(TEXT("InstancedActor_%lld_%lld_%s"), Cell.X, Cell.Y, *RuntimeGrid.ToString());
					}
					else
					{
						Label = FString::Printf(TEXT("InstancedActor_%lld_%lld"), Cell.X, Cell.Y);
					}
					ActorToCell.Add(SMActor, Label);

					const TArray<const UDataLayerAsset*> SrcDLs = SMActor->GetDataLayerAssets();

					// Look for an existing aggregate in this cell with matching data layers. Only relevant when multiple Actors are converted at once
					AActor* InstancedActor = nullptr;
					{
						TArray<AActor*> CellAggs;
						CellToAggregate.MultiFind(Label, CellAggs);
						for (AActor* Agg : CellAggs)
						{
							const TArray<const UDataLayerAsset*> AggDLs = Agg->GetDataLayerAssets();
							if (AggDLs.Num() == SrcDLs.Num())
							{
								bool bMatch = true;
								for (const UDataLayerAsset* DL : SrcDLs)
								{
									if (!AggDLs.Contains(DL))
									{
										bMatch = false;
										break;
									}
								}
								if (bMatch)
								{
									InstancedActor = Agg;
									break;
								}
							}
						}
					}

					if (!InstancedActor)
					{
						// Center aggregate on its WP tile
						const FVector CellCenter = FVector(double(Cell.X) * CellSize + CellSize * 0.5, double(Cell.Y) * CellSize + CellSize * 0.5, 0.0);
						InstancedActor = FindOrCreateAggregateActor(World, Label, SrcDLs, ExistingAggregateActors, RuntimeGrid, SMActor->GetLevel(), CellCenter);
						InstancedActor->SetIsTemporarilyHiddenInEditor(false);

						CellToAggregate.Add(Label, InstancedActor);
					}
					ActorToAggregate.Add(SMActor, InstancedActor);
				}
			}
		}
	}

	if (!bUseCellGrouping)
	{
		// Build a level-indexed multi-map of existing aggregates
		TMap<ULevel*, TArray<AActor*>> AggregatesByLevel;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(TEXT("SeamlessInstanceActor")))
			{
				if (ULevel* L = It->GetLevel())
				{
					AggregatesByLevel.FindOrAdd(L).Add(*It);
				}
			}
		}

		// For each actor find an existing aggregate in its level with matching data layers and actor layers, or create one
		for (AStaticMeshActor* SMActor : ActorsToConvert)
		{
			ULevel* ActorLevel = SMActor->GetLevel();
			if (!ActorLevel || ActorToAggregate.Contains(SMActor))
			{
				continue;
			}

			const TArray<const UDataLayerAsset*> SrcDataLayers = SMActor->GetDataLayerAssets();
			const TArray<FName> SrcActorLayers = SMActor->Layers;

			// Find an existing aggregate in the same level with matching data layers and actor layers
			AActor* InstancedActor = nullptr;
			if (TArray<AActor*>* LevelAggs = AggregatesByLevel.Find(ActorLevel))
			{
				for (AActor* Agg : *LevelAggs)
				{
					// Check data layers match
					const TArray<const UDataLayerAsset*> AggDataLayers = Agg->GetDataLayerAssets();
					if (AggDataLayers.Num() != SrcDataLayers.Num())
					{
						continue;
					}
					bool bDataMatch = true;
					for (const UDataLayerAsset* DL : SrcDataLayers)
					{
						if (!AggDataLayers.Contains(DL))
						{
							bDataMatch = false;
							break;
						}
					}
					if (!bDataMatch)
					{
						continue;
					}

					// Check actor layers match
					const TArray<FName> AggActorLayers = Agg->Layers;
					if (AggActorLayers.Num() != SrcActorLayers.Num())
					{
						continue;
					}
					bool bLayerMatch = true;
					for (const FName& Layer : SrcActorLayers)
					{
						if (!AggActorLayers.Contains(Layer))
						{
							bLayerMatch = false;
							break;
						}
					}
					if (!bLayerMatch)
					{
						continue;
					}

					InstancedActor = Agg;
					break;
				}
			}

			if (InstancedActor)
			{
				ActorToAggregate.Add(SMActor, InstancedActor);
				InstancedActor->SetIsTemporarilyHiddenInEditor(false);
			}
			else
			{
				// Pass an empty existing map so FindOrCreateAggregateActor doesn't match an aggregate from a different level
				TArray<const UDataLayerAsset*> AllLevelDataLayers = SrcDataLayers;
				TMultiMap<FString, AActor*> NoExisting;
				InstancedActor = FindOrCreateAggregateActor(World, TEXT("InstancedActor"), AllLevelDataLayers, NoExisting, NAME_None, ActorLevel);
				InstancedActor->SetIsTemporarilyHiddenInEditor(false);
				// Copy actor layers from source so future lookups match
				InstancedActor->Layers = SrcActorLayers;
				ActorToAggregate.Add(SMActor, InstancedActor);
				// Register the new aggregate so subsequent actors in the same level + matching layers find it
				AggregatesByLevel.FindOrAdd(ActorLevel).Add(InstancedActor);
			}
		}
	}

	TArray<AActor*> ActorsToDestroy;
	TArray<AActor*> EditedAggregates;
	GEditor->BeginTransaction(LOCTEXT("ConvertSMToInstanced", "Convert SM Actors to Instanced"));

	for (AStaticMeshActor* SMActor : ActorsToConvert)
	{
		AActor* AggregateActor = ActorToAggregate[SMActor];

		if (SMActor == AggregateActor)
		{
			continue;
		}

		EditedAggregates.AddUnique(AggregateActor);

		UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
		SMActor->Modify();

		FInstanceGroupKey InstanceKey;
		InstanceKey.Mesh = SMC->GetStaticMesh();
		InstanceKey.PropertiesHash = HashComponentProperties(SMC, RelevantProperties);

		const bool bUseHISM = (ComponentType == ESeamlessComponentType::HISM) || (ComponentType == ESeamlessComponentType::Auto && InstanceKey.Mesh && !InstanceKey.Mesh->NaniteSettings.bEnabled);

		// Scan the aggregate for an existing ISMC with matching mesh, properties, and component type
		UInstancedStaticMeshComponent* ISMC = nullptr;
		{
			TArray<UInstancedStaticMeshComponent*> ExistingISMCs;
			AggregateActor->GetComponents(ExistingISMCs);

			const FName IncomingHashTag = FName(*FString::Printf(TEXT("SrcHash_%u"), InstanceKey.PropertiesHash));
			for (UInstancedStaticMeshComponent* Existing : ExistingISMCs)
			{
				const bool bExistingIsHISM = (Cast<UHierarchicalInstancedStaticMeshComponent>(Existing) != nullptr);
				if (bExistingIsHISM != bUseHISM)
				{
					continue;
				}
				if (Existing->GetStaticMesh() == InstanceKey.Mesh && Existing->ComponentTags.Contains(IncomingHashTag))
				{
					ISMC = Existing;
					break;
				}
			}
		}

		if (!ISMC)
		{
			if (bUseHISM)
			{
				const FName ISMCName = *FString::Printf(TEXT("HISM_%s_%u"), *InstanceKey.Mesh->GetName(), InstanceKey.PropertiesHash);
				ISMC = NewObject<UHierarchicalInstancedStaticMeshComponent>(AggregateActor, ISMCName, RF_Transactional);
			}
			else
			{
				const FName ISMCName = *FString::Printf(TEXT("ISM_%s_%u"), *InstanceKey.Mesh->GetName(), InstanceKey.PropertiesHash);
				ISMC = NewObject<UInstancedStaticMeshComponent>(AggregateActor, ISMCName, RF_Transactional);
			}
			ISMC->SetStaticMesh(InstanceKey.Mesh);
			ISMC->SetupAttachment(AggregateActor->GetRootComponent());
			ISMC->bHasPerInstanceHitProxies = true;
			AggregateActor->AddInstanceComponent(ISMC);
			ISMC->RegisterComponent();

			// Copy all included properties from source SMC onto the new ISMC
			CopyRelevantProperties(SMC, ISMC, RelevantProperties);

			// Restore bHasPerInstanceHitProxies as it has been overwritten
			ISMC->bHasPerInstanceHitProxies = true;

			// Stamp the source properties hash so future calls can find this ISMC
			ISMC->ComponentTags.Add(FName(*FString::Printf(TEXT("SrcHash_%u"), InstanceKey.PropertiesHash)));
		}

		// Safeguard to ensure correct Static Mobility
		if (ISMC->Mobility != EComponentMobility::Static)
		{
			ISMC->SetMobility(EComponentMobility::Static);
		}

		AddInstanceDeterministic(ISMC, SMActor->GetTransform(), SMC->GetCustomPrimitiveData().Data);
		ActorsToDestroy.Add(SMActor);
	}

	for (AActor* Actor : ActorsToDestroy)
	{
		World->DestroyActor(Actor);
	}

	for (AActor* EditedAggregate : EditedAggregates)
	{
		// Fingerprint: check if this actor's state matches the stored tag from BreakInstance
		for (const FName& Tag : EditedAggregate->Tags)
		{
			FString TagStr = Tag.ToString();
			if (TagStr.StartsWith(TEXT("SeamlessFingerprint_")))
			{
				const uint32 StoredFP = (uint32)FCString::Strtoui64(*TagStr.RightChop(20), nullptr, 10);
				const uint32 CurrentFP = ComputeActorFingerprint(EditedAggregate);
				EditedAggregate->GetOutermost()->SetDirtyFlag(StoredFP != CurrentFP);
				break;
			}
		}
	}

	GEditor->EndTransaction();

	// Refresh the World Outliner
	GEditor->BroadcastLevelActorListChanged();
}

TArray<AStaticMeshActor*> USeamlessInstancingEditorSubsystem::ConvertAllInstancedToSM()
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return {};
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
		return {};
	}

	return ConvertInstancedToSM(AggregatesToConvert);
}

TArray<AStaticMeshActor*> USeamlessInstancingEditorSubsystem::ConvertInstancedToSM(const TArray<AActor*>& AggregatesToConvert)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return {};
	}

	GEditor->BeginTransaction(LOCTEXT("ConvertInstancedToSM", "Convert Instanced to SM Actors"));
	TArray<AStaticMeshActor*> CreatedActors;

	for (AActor* Aggregate : AggregatesToConvert)
	{
		if (!IsValid(Aggregate))
		{
			continue;
		}

		TArray<UInstancedStaticMeshComponent*> ISMCs;
		Aggregate->GetComponents(ISMCs);

		for (UInstancedStaticMeshComponent* ISMC : ISMCs)
		{
			if (!IsValid(ISMC))
			{
				continue;
			}
			UStaticMesh* Mesh = ISMC->GetStaticMesh();
			if (!Mesh)
			{
				continue;
			}

			// Break instances in reverse order so removal doesn't invalidate indices
			const int32 NumInstances = ISMC->GetInstanceCount();
			for (int32 i = NumInstances - 1; i >= 0; --i)
			{
				if (AStaticMeshActor* NewActor = BreakInstance(ISMC, i, false))
				{
					CreatedActors.Add(NewActor);
				}
			}
		}
	}

	GEditor->EndTransaction();
	return CreatedActors;
}

void USeamlessInstancingEditorSubsystem::SetSeamlessEnabled(bool bEnabled)
{
	bCachedSeamlessEnabled = bEnabled;
	GConfig->SetBool(TEXT("SeamlessInstancing"), TEXT("bEnableSeamless"), bEnabled, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void USeamlessInstancingEditorSubsystem::SetComponentType(ESeamlessComponentType InType)
{
	ComponentType = InType;
	GConfig->SetInt(TEXT("SeamlessInstancing"), TEXT("ComponentType"), static_cast<int32>(InType), GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

// Lazy binding

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

// Tick-based rect capture

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
	}
	// Discard if rect < MOUSE_CLICK_DRAG_DELTA
	else if (CachedSelectionRect.Width() < 4 || CachedSelectionRect.Height() < 4)
	{
		CachedSelectionRect = FIntRect();
	}

	return true;
}

// Selection-change handler

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
						// Use the viewport's hit-proxy rect query
						TArray<TPair<UInstancedStaticMeshComponent*, int32>> Selected = FindSelectionInstances(ActiveViewport, Aggregate, SelRect);

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
									BreakInstance(Sel.Key, Sel.Value, false);
									Aggregate->GetOutermost()->SetDirtyFlag(true);
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
					//UE_LOG(LogSeamlessInstancing, Log, TEXT("FindClickedInstance BreakInstance: %d"), InstanceIndex);
					BreakInstance(HitISMC, InstanceIndex);
					Aggregate->GetOutermost()->SetDirtyFlag(true);
				}
			}
		}
	}
}

// PreSave fingerprint tagging

void USeamlessInstancingEditorSubsystem::OnPreSavePackage(UPackage* Package, FObjectPreSaveContext Context)
{
	if (!Package || Context.IsCooking())
	{
		return;
	}

	// Collect all objects in the package: WP actor packages and non-WP level packages
	TArray<UObject*> Objects;
	GetObjectsWithOuter(Package, Objects, true);

	for (UObject* Obj : Objects)
	{
		AActor* InstancedActor = Cast<AActor>(Obj);
		if (!InstancedActor)
		{
			continue;
		}
		if (!InstancedActor->Tags.Contains(TEXT("SeamlessInstanceActor")))
		{
			continue;
		}

		// Compute the current fingerprint
		const uint32 Fingerprint = ComputeActorFingerprint(InstancedActor);
		FName FingerprintTag = FName(*FString::Printf(TEXT("SeamlessFingerprint_%u"), Fingerprint));

		// Find Fingerprint tag to see if it needs updating
		bool bNeedsTag = true;
		for (const FName& Tag : InstancedActor->Tags)
		{
			FString TagStr = Tag.ToString();
			if (TagStr.StartsWith(TEXT("SeamlessFingerprint_")))
			{
				if (Tag != FingerprintTag)
				{
					InstancedActor->Tags.RemoveAll([](const FName& Tag)
					{
						return Tag.ToString().StartsWith(TEXT("SeamlessFingerprint_"));
					});
				}
				else
				{
					bNeedsTag = false;
				}
				break;
			}
		}

		if (bNeedsTag)
		{
			InstancedActor->Tags.Add(FingerprintTag);
		}
	}
}

#undef LOCTEXT_NAMESPACE
