// Copyright Epic Games, Inc. All Rights Reserved.

#include "SeamlessInstancingEditorSubsystem.h"
#include "SeamlessInstancingEditorModule.h"
#include "SeamlessInstancingHelpers.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Selection.h"
#include "LevelEditorSubsystem.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorSpatialHash.h"
#include "Engine/StaticMeshActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"

#define LOCTEXT_NAMESPACE "SeamlessInstancing"


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

	UE_LOG(LogSeamlessInstancing, Log, TEXT("SeamlessInstancingEditorSubsystem initialized."));
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

	// Single world scan to find any pre-existing aggregate actors so per-cell
	// lookups are O(1) rather than a full TActorIterator scan per cell.
	TMap<FString, AActor*> ExistingAggregateActors;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(TEXT("SeamlessInstanceActor")))
		{
			ExistingAggregateActors.Add(It->GetActorLabel(), *It);
		}
	}

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
		AActor* SingleActor = FindOrCreateAggregateActor(World, TEXT("SeamlessInstanceActor"), {}, ExistingAggregateActors);
		CellToAggregate.Add(FCachedCellCoord{0, 0}, SingleActor);
	}

	TMap<AActor*, TMap<FInstanceGroupKey, UInstancedStaticMeshComponent*>> AggregateToMeshMap;
	TArray<AActor*> ActorsToDestroy;

	GEditor->BeginTransaction(LOCTEXT("ConvertSMToInstanced", "Convert SM Actors to Instanced"));

	for (AStaticMeshActor* SMActor : ActorsToConvert)
	{
		const FCachedCellCoord CellKey = bUseCellGrouping ? ActorToCell[SMActor] : FCachedCellCoord{0, 0};
		AActor* AggregateActor = CellToAggregate[CellKey];

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

		for (UInstancedStaticMeshComponent* ISMC : ISMCs)
		{
			UStaticMesh* Mesh = ISMC->GetStaticMesh();
			if (!Mesh) continue;

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

			UE_LOG(LogSeamlessInstancing, Log, TEXT("SeamlessInstancingEditorSubsystem: selection events bound."));
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
