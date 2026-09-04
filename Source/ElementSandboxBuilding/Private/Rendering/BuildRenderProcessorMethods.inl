// FBuildRenderProcessor 公共门面：DirtySet 执行、观察投影、查询与统计。
// 外部调用方不接触内部 Residency 容器。

FBuildRenderProcessor::FBuildRenderProcessor(const FBuildPresentationResidencyConfig& InResidencyConfig,
											 const FBuildRenderClusterConfig& InClusterConfig)
	: Data(MakeUnique<FBuildRenderProcessorData>(InResidencyConfig, InClusterConfig))
{
	check(IsInGameThread());
	check(InResidencyConfig.IsValid());
	check(InClusterConfig.IsValid());
}

FBuildRenderProcessor::~FBuildRenderProcessor() = default;

bool FBuildRenderClusterConfig::TryGetCellCoordinate(const FVector& WorldLocation, FIntVector& OutCellCoordinate) const
{
	return IsValid() && TryGetPresentationGridCoordinate(WorldLocation, StaticCellSize, OutCellCoordinate);
}

bool FBuildRenderProcessor::Execute(const FBuildEntityRegistry& Registry, FBuildRenderDirtySet& DirtySet,
									UPresentationWorldSubsystem& Presentation, const FMeshPoolLayerHandle Layer)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_IndexAndDirty);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, RenderResolve);
	if (!Data || !Layer.IsSet())
	{
		return false;
	}
	if (DirtySet.IsClearAllRequested())
	{
		TArray<FBuildEntityHandle> RenderedEntities;
		Data->RenderedEntities.GenerateKeyArray(RenderedEntities);
		for (const FBuildEntityHandle Entity : RenderedEntities)
		{
			if (!RemoveRenderedEntity(*Data, Presentation, Entity))
			{
				return false;
			}
		}
		Data->Clear();
		Data->RequiredNowEntities.Reset();
		Data->RequiredNowMeshPartCount = 0;
		Data->RequiredResidentEntityCount = 0;
		Data->RequiredResidentMeshPartCount = 0;
		Data->ResidencyReferenceCounts.Reset();
		Data->HotPinReferenceCounts.Reset();
		Data->ProvisionalHotEntities.Reset();
		Data->PendingLiveResidentAdds.Reset();
		Data->PendingLiveResidentAddCursor = 0;
		Data->MotionActiveEntities.Reset();
		Data->SourceStates.Reset();
		Data->SourceKeyByToken.Reset();
		Data->ResetAsyncState();
		Data->PendingUrgentResidentAdds.Reset();
		Data->PendingUrgentResidentAddSet.Reset();
		Data->PendingUrgentResidentAddCursor = 0;
		Data->PendingDirectionalCleanupEntities.Reset();
		Data->PendingDirectionalCleanupSet.Reset();
		Data->PendingDirectionalCleanupCursor = 0;
		Data->PendingStorageRefreshEntities.Reset();
		Data->PendingStorageRefreshSet.Reset();
		Data->PendingResidencyReleaseBatches.Reset();
		Data->PendingHotPinReleaseBatches.Reset();
		Data->LastViews.Reset();
		Data->CurrentViews.Reset();
		Data->LastRequiredEntityCount = 0;
		Data->LastRequiredPartCount = 0;
		Data->bOrdinaryEvictionPressureActive = false;
		Data->bOrdinaryEvictionSweepInProgress = false;
		Data->OrdinaryEvictionSweepCursor = 0;
		Data->bSelectionDirty = true;
	}

	for (const FBuildRenderDirtyEntry& Dirty : DirtySet.GetEntries())
	{
			Data->bSelectionDirty = true;
			if (!Registry.IsAlive(Dirty.Entity))
			{
				if (!RemoveRenderedEntity(*Data, Presentation, Dirty.Entity))
				{
					return false;
				}
			Data->PurgeEntityResidencyReferences(Dirty.Entity);
			Data->PendingStorageRefreshSet.Remove(Dirty.Entity);
			Data->RemoveEntryAndNotify(Dirty.Entity);
			continue;
		}

		const FBuildTransformFragment* Transform = nullptr;
		const UBuildingDefinition* Definition = nullptr;
		const FBuildPartTransformFragment* PartTransforms = nullptr;
		FBox Bounds(ForceInit);
		if (!TryResolveRenderEntity(Registry, Dirty.Entity, Transform, Definition, PartTransforms) ||
			!Definition->TryCalculateWorldBounds(Transform->WorldTransform,
												 PartTransforms
													 ? TConstArrayView<FTransform>(PartTransforms->LocalTransforms)
													 : TConstArrayView<FTransform>(),
												 Bounds) ||
			!Data->UpsertEntryAndNotify(
				Dirty.Entity, Bounds, FBuildPresentationMeshPoolApplicator::CountMeshParts(*Definition),
				FBuildPresentationMeshPoolApplicator::CountPromotableMeshParts(*Definition),
				Dirty.bPackedStatic.Get(Data->PresentationIndex.FindEntry(Dirty.Entity)
											? Data->PresentationIndex.FindEntry(Dirty.Entity)->bPackedStatic
											: true)))
		{
			return false;
		}
		FRenderedEntity* Rendered = Data->FindRendered(Dirty.Entity);
		if (!Rendered)
		{
			continue;
		}
		const int32 DefinitionMeshPartCount = FBuildPresentationMeshPoolApplicator::CountMeshParts(*Definition);
		bool bTopologyChanged = Dirty.Mode == EBuildRenderDirtyMode::Rebuild ||
			Rendered->NextPartId > Definition->MeshParts.Num() ||
			Rendered->MeshPartCost > DefinitionMeshPartCount ||
			(Rendered->bComplete && Rendered->MeshPartCost != DefinitionMeshPartCount);
		if (!bTopologyChanged)
		{
			for (const FRenderedPart& Part : Rendered->Parts)
			{
				if (!Definition->MeshParts.IsValidIndex(Part.PartId) ||
					Definition->MeshParts[Part.PartId].Mesh != Part.Cluster.Mesh)
				{
					bTopologyChanged = true;
					break;
				}
			}
		}
		if (bTopologyChanged)
		{
			if (!RemoveRenderedEntity(*Data, Presentation, Dirty.Entity))
			{
				return false;
			}
			if (Data->RequiredNowEntities.Contains(Dirty.Entity))
			{
				Data->QueueUrgentResidentAdd(Dirty.Entity);
			}
			continue;
		}
		const TConstArrayView<int32> PartIds = Dirty.Mode == EBuildRenderDirtyMode::PartSet
												   ? TConstArrayView<int32>(Dirty.PartIds)
												   : TConstArrayView<int32>();
		if (!UpdateRenderedEntity(*Data, Registry, Presentation, Layer, Dirty.Entity, PartIds))
		{
			return false;
		}
	}
	DirtySet.Clear();
	return true;
}

bool FBuildRenderProcessor::Project(const FBuildEntityRegistry& Registry, const FPresentationViewSnapshot& Views,
									UPresentationWorldSubsystem& Presentation, const FMeshPoolLayerHandle Layer)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_SelectResidency);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, RenderResidency);
	if (!Data || !Layer.IsSet())
	{
		return false;
	}
	const bool bSynchronizeLocalSelection = Data->bSynchronizeNextLocalSelection;
	Data->bSynchronizeNextLocalSelection = false;

	const double ProjectionStartSeconds = FPlatformTime::Seconds();
	const double NowSeconds = Data->GetNowSeconds();
	// 生产路径这里只发布完成结果并调度至多一个 Worker；Automation 同步模式保持确定性断言。
#if WITH_DEV_AUTOMATION_TESTS
	Data->PresentationIndex.ProcessAsyncPackedBuildWork(Data->bSynchronousResidencySelectionForTesting);
#else
	Data->PresentationIndex.ProcessAsyncPackedBuildWork();
#endif
	Data->LastSelectionMilliseconds = 0.0;
	Data->LastAddedResidentEntityCount = 0;
	Data->LastAddedResidentPartCount = 0;
	Data->LastEvictedEntityCount = 0;
	Data->LastEvictedPartCount = 0;
	Data->LastCandidateNodeCount = 0;
	Data->LastCandidateEntryCount = 0;
	Data->LastPrunedNodeCount = 0;
	Data->LastAcceptedSubtreeCount = 0;
	Data->LastLocalSelectionCacheHitSourceCount = 0;
	Data->LastHotPinMaintenanceEntityCount = 0;
	Data->LastResidencyMutationVisitedEntityCount = 0;
	Data->LastResidencyMutationMilliseconds = 0.0;
	Data->LastLocalResidentBoundary = 0.0;
	UpdateAdaptiveWorkBudget(*Data, Presentation);

	if (Presentation.ConsumeLayerReprojectionRequest(Layer))
	{
		TArray<TPair<FBuildEntityHandle, double>> CachedEntities;
		CachedEntities.Reserve(Data->RenderedEntities.Num());
		for (const TBuildEntitySparseMap<FRenderedEntity>::FEntry& Pair : Data->RenderedEntities.GetEntries())
		{
			CachedEntities.Emplace(Pair.Entity, Pair.Value.LastRequiredTimeSeconds);
		}
		for (const TPair<FBuildEntityHandle, double>& Cached : CachedEntities)
		{
			if (!RemoveRenderedEntity(*Data, Presentation, Cached.Key))
			{
				return false;
			}
		}
		for (const TPair<FBuildEntityHandle, double>& Cached : CachedEntities)
		{
			if (Registry.IsAlive(Cached.Key) && Data->PresentationIndex.FindEntry(Cached.Key) &&
				Data->RequiredNowEntities.Contains(Cached.Key))
			{
				Data->QueueUrgentResidentAdd(Cached.Key);
			}
		}
		Data->PendingStorageRefreshEntities.Reset();
		Data->PendingStorageRefreshSet.Reset();
	}

	const double SelectionStartSeconds = FPlatformTime::Seconds();
	Data->CurrentViews = Views.Sources;
	TSet<FPresentationSourceKey> SeenSources;
	SeenSources.Reserve(Views.Sources.Num());
	for (int32 SourceIndex = 0; SourceIndex < Views.Sources.Num(); ++SourceIndex)
	{
		const FPresentationViewSource& View = Views.Sources[SourceIndex];
		const FPresentationSourceKey Key = FBuildRenderProcessorData::MakeSourceKey(View, SourceIndex);
		SeenSources.Add(Key);
		FSourceResidencyState& State = Data->FindOrAddSourceState(Key);
		Data->UpdateSourceObservation(State, View, NowSeconds);
		Data->UpdateLocalSelectionState(State, NowSeconds);
		if (bSynchronizeLocalSelection && State.bNeedsLocalSelection && State.bLocalSelectionInFlight)
		{
			if (State.LatestLocalRequestId == State.InFlightLocalRequestId)
			{
				++State.LatestLocalRequestId;
			}
			State.InFlightLocalRequestId = 0;
			State.bLocalSelectionInFlight = false;
		}
	}
	TArray<FPresentationSourceKey> RemovedSources;
	for (const TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : Data->SourceStates)
	{
		if (!SeenSources.Contains(Pair.Key))
		{
			RemovedSources.Add(Pair.Key);
		}
	}
	for (const FPresentationSourceKey& Key : RemovedSources)
	{
		Data->RemoveSourceState(Key, NowSeconds);
	}

	TArray<FSourceResidencyState*> OrderedStates;
	OrderedStates.Reserve(Data->SourceStates.Num());
	for (TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : Data->SourceStates)
	{
		OrderedStates.Add(&Pair.Value);
	}
	OrderedStates.Sort([](const FSourceResidencyState& Left, const FSourceResidencyState& Right)
					   { return Left.Token < Right.Token; });

	Data->DrainLocalSelectionResults(NowSeconds);
	for (FSourceResidencyState* State : OrderedStates)
	{
		if (State->bNeedsLocalSelection && !State->bLocalSelectionInFlight && State->TargetLocal.SourceRevision == 0)
		{
			Data->DispatchLocalSelection(*State, NowSeconds, bSynchronizeLocalSelection);
		}
	}
	// 测试同步路径以及极短后台任务都允许在同一周期发布；真实线程仍只经 MPSC 返回。
	Data->DrainLocalSelectionResults(NowSeconds);

	int32 RemainingLocalPublishBudget = Data->ResidencyConfig.LocalTransitionPublishBudgetEntitiesPerCycle;
	const int32 DeferredReleaseBudget =
		FMath::Min(RemainingLocalPublishBudget,
				   FMath::Max(1, Data->ResidencyConfig.LocalTransitionPublishBudgetEntitiesPerCycle / 4));
	RemainingLocalPublishBudget -= Data->ProcessDeferredLocalReleases(NowSeconds, DeferredReleaseBudget);
	for (int32 StateIndex = 0; StateIndex < OrderedStates.Num() && RemainingLocalPublishBudget > 0; ++StateIndex)
	{
		const int32 RemainingSourceCount = OrderedStates.Num() - StateIndex;
		const int32 SourceBudget = FMath::Max(1, RemainingLocalPublishBudget / RemainingSourceCount);
		RemainingLocalPublishBudget -=
			Data->AdvanceTargetLocalPreparation(*OrderedStates[StateIndex], Registry, SourceBudget);
	}
	Data->MaintainProvisionalHotEntities(Registry, NowSeconds);

	FBuildResidencyMutationBudget MutationBudget(*Data);
	if (!ApplyUrgentResidentAdds(*Data, Registry, Presentation, Layer, NowSeconds, MutationBudget))
	{
		return false;
	}
	for (int32 StateIndex = 0; StateIndex < OrderedStates.Num() && RemainingLocalPublishBudget > 0; ++StateIndex)
	{
		const int32 RemainingSourceCount = OrderedStates.Num() - StateIndex;
		const int32 SourceBudget = FMath::Max(1, RemainingLocalPublishBudget / RemainingSourceCount);
		RemainingLocalPublishBudget -=
			Data->TryCommitLocalTransition(*OrderedStates[StateIndex], NowSeconds, SourceBudget);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_RefreshStorageClasses);
		for (const FBuildEntityHandle Entity : Data->PendingStorageRefreshEntities)
		{
			if (Data->PendingStorageRefreshSet.Contains(Entity) && Registry.IsAlive(Entity) &&
				!RefreshRenderedEntityStorage(*Data, Registry, Presentation, Layer, Entity))
			{
				return false;
			}
		}
		Data->PendingStorageRefreshEntities.Reset();
		Data->PendingStorageRefreshSet.Reset();
	}

	Data->DrainFarSelectionResults(NowSeconds);
	for (FSourceResidencyState* StatePtr : OrderedStates)
	{
		FSourceResidencyState& State = *StatePtr;
		if (State.bNeedsLocalSelection || State.bLocalSelectionInFlight || State.TargetLocal.SourceRevision != 0)
		{
			continue;
		}
		if (State.bRapidRotation || State.SettledSinceSeconds < 0.0 ||
			NowSeconds - State.SettledSinceSeconds + UE_SMALL_NUMBER < Data->ResidencyConfig.FarSettleSeconds)
		{
			State.Phase = EBuildPresentationTransitionPhase::RapidSettling;
			continue;
		}
		const bool bActiveCovered =
			State.bHasActiveDirection &&
			Data->IsDirectionCovered(State.ActiveDirection, State.ActiveViewLocation, State.ActiveSubjectLocation,
									 State.ActiveFarSourceRevision, State.FarContentRevision, State.LatestView);
		const bool bTransitionCovered =
			!State.TargetEntries.IsEmpty() &&
			FBuildRenderProcessorData::GetDirectionDeltaDegrees(
				State.TransitionDirection, FBuildRenderProcessorData::GetHorizontalForward(State.LatestView)) <=
				Data->GetRecenterThresholdDegrees(State.LatestView) &&
			FVector::DistSquared(State.TransitionViewLocation, State.LatestView.ViewLocation) <=
				FMath::Square(Data->ResidencyConfig.SourceMovementThreshold);
		if (bActiveCovered && !bTransitionCovered && State.Phase != EBuildPresentationTransitionPhase::Stable)
		{
			Data->CancelTransition(State, NowSeconds);
			continue;
		}
		if (bActiveCovered && State.Phase == EBuildPresentationTransitionPhase::Stable)
		{
			State.bNeedsFarSelection = false;
			continue;
		}
		const bool bFarContentSettled = Data->ResidencyConfig.FarSettleSeconds <= 0.0 ||
			State.LastFarContentChangeTimeSeconds < 0.0 ||
			NowSeconds - State.LastFarContentChangeTimeSeconds + UE_SMALL_NUMBER >=
				Data->ResidencyConfig.FarSettleSeconds;
		if (bFarContentSettled && State.bNeedsFarSelection && !State.bSelectionInFlight &&
			State.SupersededTransitionEntities.IsEmpty() &&
			(!State.bTargetRevisionCurrent || State.TargetEntries.IsEmpty() ||
			 State.TargetCursor >= State.TargetEntries.Num()))
		{
			Data->DispatchFarSelection(State);
		}
	}
	Data->DrainFarSelectionResults(NowSeconds);
	MutationBudget.UpgradeForEmergency(*Data);
	Data->LastSelectionMilliseconds = (FPlatformTime::Seconds() - SelectionStartSeconds) * 1000.0;

	for (FSourceResidencyState* State : OrderedStates)
	{
		if (State->bNeedsLocalSelection || State->bLocalSelectionInFlight || State->TargetLocal.SourceRevision != 0)
		{
			continue;
		}
		if (!ApplyFarTransition(*Data, *State, Registry, Presentation, Layer, NowSeconds, MutationBudget))
		{
			return false;
		}
	}
	bool bAnyRapidSource = false;
	for (const FSourceResidencyState* State : OrderedStates)
	{
		bAnyRapidSource |= State->bRapidRotation || State->Phase == EBuildPresentationTransitionPhase::RapidSettling;
	}
	if (!bAnyRapidSource && !ApplyDirectionalCleanup(*Data, Presentation, MutationBudget))
	{
		return false;
	}
	if (!bAnyRapidSource && !EvictOrdinaryCache(*Data, Presentation, NowSeconds, MutationBudget))
	{
		return false;
	}
	Data->LastResidencyMutationVisitedEntityCount = MutationBudget.VisitedEntityCount;
	Data->LastResidencyMutationMilliseconds = MutationBudget.GetElapsedMilliseconds();

	Data->LastRequiredEntityCount = Data->RequiredNowEntities.Num();
	Data->LastRequiredPartCount = Data->RequiredNowMeshPartCount;
	Data->LastActiveFarPartCount = 0;
	Data->LastTransitionLocalPartCount = 0;
	Data->LastTransitionFarPartCount = 0;
	Data->LastOverlappingFarPartCount = 0;
	Data->LastVisibleCoreMissingPartCount = 0;
	Data->LastRapidFrozenSourceCount = 0;
	Data->LastTransitionPhase = EBuildPresentationTransitionPhase::Stable;
	for (const FSourceResidencyState* State : OrderedStates)
	{
		Data->LastTransitionLocalPartCount += State->TargetLocal.MeshPartCost;
		Data->LastActiveFarPartCount += State->ActiveFarMeshPartCost;
		Data->LastTransitionFarPartCount += State->TransitionFarMeshPartCost;
		Data->LastRapidFrozenSourceCount +=
			State->bRapidRotation || State->Phase == EBuildPresentationTransitionPhase::RapidSettling ? 1 : 0;
		if (static_cast<uint8>(State->Phase) > static_cast<uint8>(Data->LastTransitionPhase))
		{
			Data->LastTransitionPhase = State->Phase;
		}
		Data->LastOverlappingFarPartCount += State->OverlappingFarMeshPartCost;
		Data->LastVisibleCoreMissingPartCount += State->VisibleCoreRemainingMeshPartCost;
	}
	Data->GetLocalTransitionBacklog(Data->LastPendingLocalPreparationEntityCount,
									Data->LastPendingLocalRenderEntityCount, Data->LastPendingLocalReleaseEntityCount);
	Data->LastProjectedIndexRevision = Data->PresentationIndex.GetIndexRevision();
	Data->bSelectionDirty = false;
	Data->LastViews = Views.Sources;

	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationResidentEntities, Data->RenderedEntityCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationResidentParts, Data->RenderedPartCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationRequiredParts, Data->LastRequiredPartCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationAddedParts, Data->LastAddedResidentPartCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationEvictedParts, Data->LastEvictedPartCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationCandidateNodes, Data->LastCandidateNodeCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationCandidateEntries, Data->LastCandidateEntryCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationPrunedNodes, Data->LastPrunedNodeCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationTransitionLocalParts, Data->LastTransitionLocalPartCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationVisibleCoreMissingParts, Data->LastVisibleCoreMissingPartCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationWorkBudgetParts, Data->CurrentWorkBudgetParts,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationMutationVisitedEntities,
					Data->LastResidencyMutationVisitedEntityCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationMutationMilliseconds,
					Data->LastResidencyMutationMilliseconds, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationLocalPreparePending,
					Data->LastPendingLocalPreparationEntityCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationLocalRenderPending, Data->LastPendingLocalRenderEntityCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationLocalReleasePending, Data->LastPendingLocalReleaseEntityCount,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationHotPinnedEntities, Data->HotPinReferenceCounts.Num(),
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationHotPinMaintenanceEntities,
					Data->LastHotPinMaintenanceEntityCount, ECsvCustomStatOp::Set);
	Data->LastProjectionMilliseconds = (FPlatformTime::Seconds() - ProjectionStartSeconds) * 1000.0;
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationSelectionMilliseconds, Data->LastSelectionMilliseconds,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationProjectionMilliseconds, Data->LastProjectionMilliseconds,
					ECsvCustomStatOp::Set);
	if (Data->LastProjectionMilliseconds >= 16.0 &&
		ProjectionStartSeconds - Data->LastSlowProjectionLogTimeSeconds >= 1.0)
	{
		Data->LastSlowProjectionLogTimeSeconds = ProjectionStartSeconds;
		UE_LOG(LogElementSandboxBuilding, Display,
			   TEXT("Slow presentation projection: total=%.2fms selection=%.2fms add=%d remove=%d resident=%d "
					"required=%d budget=%d phase=%d rapid=%d async=%d local_prepare=%d local_wait_render=%d "
					"local_release=%d hot=%d hot_ops=%d localPass=%llu farPass=%llu index=%llu cellSerial=%llu."),
			   Data->LastProjectionMilliseconds, Data->LastSelectionMilliseconds, Data->LastAddedResidentPartCount,
			   Data->LastEvictedPartCount, Data->RenderedPartCount, Data->LastRequiredPartCount,
			   Data->CurrentWorkBudgetParts, static_cast<int32>(Data->LastTransitionPhase),
			   Data->LastRapidFrozenSourceCount, Data->AsyncState->InFlightCount.Load(),
			   Data->LastPendingLocalPreparationEntityCount, Data->LastPendingLocalRenderEntityCount,
			   Data->LastPendingLocalReleaseEntityCount, Data->HotPinReferenceCounts.Num(),
			   Data->LastHotPinMaintenanceEntityCount, static_cast<unsigned long long>(Data->LocalSelectionPassCount),
			   static_cast<unsigned long long>(Data->SelectionPassCount),
			   static_cast<unsigned long long>(Data->PresentationIndex.GetIndexRevision()),
			   static_cast<unsigned long long>(Data->IndexChangeSerial));
	}
	return true;
}

bool FBuildRenderProcessor::TryGetInstanceHandle(const FBuildEntityHandle Entity, const int32 PartId,
												 FMeshPoolInstanceHandle& OutInstance) const
{
	OutInstance = {};
	const FRenderedEntity* Record = Data ? Data->FindRendered(Entity) : nullptr;
	const FRenderedPart* Part = Record ? Data->FindRenderedPart(*Record, PartId) : nullptr;
	if (!Part)
	{
		return false;
	}
	OutInstance = Part->Instance;
	return true;
}

bool FBuildRenderProcessor::TryGetPartStorageClass(const FBuildEntityHandle Entity, const int32 PartId,
												   EBuildRenderStorageClass& OutStorageClass) const
{
	const FRenderedEntity* Record = Data ? Data->FindRendered(Entity) : nullptr;
	const FRenderedPart* Part = Record ? Data->FindRenderedPart(*Record, PartId) : nullptr;
	if (!Part)
	{
		return false;
	}
	OutStorageClass = Part->StorageClass;
	return true;
}

bool FBuildRenderProcessor::SetPresentationMotionActive(const FBuildEntityHandle Entity, const bool bActive)
{
	if (!Data || !Entity.IsSet())
	{
		return false;
	}
	const bool bWasActive = Data->MotionActiveEntities.Contains(Entity);
	if (bWasActive == bActive)
	{
		return true;
	}
	const bool bWasHot = Data->IsEntityHot(Entity);
	if (bActive)
	{
		Data->MotionActiveEntities.Add(Entity);
		Data->AddResidencyReference(Entity);
		Data->QueueUrgentResidentAdd(Entity);
	}
	else
	{
		Data->MotionActiveEntities.Remove(Entity);
		Data->ReleaseResidencyReference(Entity, Data->GetNowSeconds());
	}
	if (bWasHot != Data->IsEntityHot(Entity))
	{
		Data->QueueStorageRefresh(Entity);
	}
	Data->bSelectionDirty = true;
	return true;
}

bool FBuildRenderProcessor::IsPresentationMotionActive(const FBuildEntityHandle Entity) const
{
	return Data && Data->MotionActiveEntities.Contains(Entity);
}

bool FBuildRenderProcessor::HasPendingProjectionWork() const
{
	if (!Data)
	{
		return false;
	}
	if (Data->bSelectionDirty || Data->bOrdinaryEvictionPressureActive ||
		Data->PendingUrgentResidentAdds.IsValidIndex(Data->PendingUrgentResidentAddCursor) ||
		Data->PendingDirectionalCleanupEntities.IsValidIndex(Data->PendingDirectionalCleanupCursor) ||
		!Data->PendingResidencyReleaseBatches.IsEmpty() || !Data->PendingHotPinReleaseBatches.IsEmpty() ||
		Data->AsyncState->InFlightCount.Load() > 0 || Data->PresentationIndex.HasPendingAsyncPackedBuildWork())
	{
		return true;
	}
	for (const TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : Data->SourceStates)
	{
		if (Pair.Value.bNeedsLocalSelection || Pair.Value.bLocalSelectionInFlight ||
			Pair.Value.TargetLocal.SourceRevision != 0 || Pair.Value.bNeedsFarSelection ||
			Pair.Value.bSelectionInFlight || Pair.Value.Phase != EBuildPresentationTransitionPhase::Stable)
		{
			return true;
		}
	}
	return false;
}

void FBuildRenderProcessor::RequestSynchronousLocalSelectionForNextProjection()
{
	check(IsInGameThread());
	if (Data)
	{
		Data->bSynchronizeNextLocalSelection = true;
	}
}

bool FBuildRenderProcessor::ApplyCustomDataChanges(const FBuildEntityRegistry& Registry,
												   UPresentationWorldSubsystem& Presentation,
												   const TConstArrayView<FBuildEntityHandle> Entities)
{
	if (!Data)
	{
		return false;
	}
	for (const FBuildEntityHandle Entity : Entities)
	{
		const FRenderedEntity* Record = Data->FindRendered(Entity);
		if (!Record || !Registry.IsAlive(Entity))
		{
			continue;
		}
		if (!FBuildPresentationMeshPoolApplicator::QueueCustomData(Registry, Presentation, Entity, Record->Parts))
		{
			return false;
		}
	}
	return true;
}

void FBuildRenderProcessor::ReserveEntityCapacity(const int32 EntityCapacity)
{
	check(IsInGameThread());
	if (Data && EntityCapacity > 0)
	{
		Data->PresentationIndex.ReserveEntityCapacity(EntityCapacity);
	}
}

int32 FBuildRenderProcessor::GetRenderedEntityCount() const { return Data ? Data->RenderedEntityCount : 0; }

int32 FBuildRenderProcessor::GetRenderedPartCount(const FBuildEntityHandle Entity) const
{
	const FRenderedEntity* Record = Data ? Data->FindRendered(Entity) : nullptr;
	return Record ? Record->Parts.Num() : 0;
}

bool FBuildRenderProcessor::HasRetiringInstances(const FBuildEntityHandle Entity) const
{
	return Data && Data->HasRetiringInstances(Entity);
}

void FBuildRenderProcessor::NotifyInstanceRetired(const FMeshPoolInstanceHandle Instance)
{
	if (Data)
	{
		Data->NotifyInstanceRetired(Instance);
	}
}

double FBuildRenderProcessor::GetStaticRenderCellSize() const
{
	return Data ? Data->ClusterConfig.StaticCellSize : 0.0;
}

SIZE_T FBuildRenderProcessor::GetEstimatedAllocatedSize() const
{
	if (!Data)
	{
		return 0;
	}
	const FBuildPresentationIndexStats IndexStats = Data->PresentationIndex.GetStats();
	SIZE_T Size = IndexStats.EstimatedAllocatedSize + Data->RenderedEntities.GetAllocatedSize() +
				  Data->RetiringEntities.GetAllocatedSize() + Data->RetiringInstanceOwners.GetAllocatedSize() +
				  Data->RequiredNowEntities.GetAllocatedSize() + Data->ResidencyReferenceCounts.GetAllocatedSize() +
				  Data->HotPinReferenceCounts.GetAllocatedSize() + Data->ProvisionalHotEntities.GetAllocatedSize() +
				  Data->PendingLiveResidentAdds.GetAllocatedSize() +
				  Data->MotionActiveEntities.GetAllocatedSize() +
				  Data->InvalidationCellRevisions.GetAllocatedSize() + Data->SourceStates.GetAllocatedSize() +
				  Data->SourceKeyByToken.GetAllocatedSize() + Data->PendingUrgentResidentAdds.GetAllocatedSize() +
				  Data->PendingUrgentResidentAddSet.GetAllocatedSize() +
				  Data->PendingDirectionalCleanupEntities.GetAllocatedSize() +
				  Data->PendingDirectionalCleanupSet.GetAllocatedSize() +
				  Data->PendingStorageRefreshEntities.GetAllocatedSize() +
				  Data->PendingStorageRefreshSet.GetAllocatedSize() + Data->LastViews.GetAllocatedSize() +
				  Data->CurrentViews.GetAllocatedSize();
	for (const TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : Data->SourceStates)
	{
		Size += Pair.Value.GetEstimatedAllocatedSize();
	}
	Size += Data->PendingResidencyReleaseBatches.GetAllocatedSize();
	for (const FDeferredResidencyReleaseBatch& Batch : Data->PendingResidencyReleaseBatches)
	{
		Size += Batch.Entities.GetAllocatedSize();
	}
	Size += Data->PendingHotPinReleaseBatches.GetAllocatedSize();
	for (const FDeferredHotPinReleaseBatch& Batch : Data->PendingHotPinReleaseBatches)
	{
		Size += Batch.Entities.GetAllocatedSize();
	}
	for (const TBuildEntitySparseMap<FRenderedEntity>::FEntry& Pair : Data->RenderedEntities.GetEntries())
	{
		Size += Pair.Value.Parts.GetAllocatedSize();
	}
	return Size;
}

FBuildPresentationSelectionStats FBuildRenderProcessor::GetSelectionStats() const
{
	FBuildPresentationSelectionStats Stats;
	if (Data)
	{
		const FBuildPresentationIndexStats IndexStats = Data->PresentationIndex.GetStats();
		Stats.RequiredEntityCount = Data->LastRequiredEntityCount;
		Stats.RequiredMeshPartCount = Data->LastRequiredPartCount;
		Stats.CachedOnlyEntityCount = FMath::Max(0, Data->RenderedEntityCount - Data->RequiredResidentEntityCount);
		Stats.CachedOnlyMeshPartCount = FMath::Max(0, Data->RenderedPartCount - Data->RequiredResidentMeshPartCount);
		Stats.ResidentEntityCount = Data->RenderedEntityCount;
		Stats.ResidentMeshPartCount = Data->RenderedPartCount;
		Stats.LocalResidentBoundary = Data->LastLocalResidentBoundary;
		Stats.EvictionCandidateCount = Data->LastEvictionCandidateCount;
		Stats.EvictionGraceBlockedCount = Data->LastEvictionGraceBlockedCount;
		Stats.LastEvictedEntityCount = Data->LastEvictedEntityCount;
		Stats.LastEvictedMeshPartCount = Data->LastEvictedPartCount;
		Stats.CandidateNodeCount = Data->LastCandidateNodeCount;
		Stats.CandidateEntryCount = Data->LastCandidateEntryCount;
		Stats.PrunedNodeCount = Data->LastPrunedNodeCount;
		Stats.AcceptedSubtreeCount = Data->LastAcceptedSubtreeCount;
		Stats.LocalSelectionCacheHitSourceCount = Data->LastLocalSelectionCacheHitSourceCount;
		Stats.LastAddedResidentEntityCount = Data->LastAddedResidentEntityCount;
		Stats.LastAddedResidentMeshPartCount = Data->LastAddedResidentPartCount;
		Stats.PendingRequiredEntityCount =
			FMath::Max(0, Data->RequiredNowEntities.Num() - Data->RequiredResidentEntityCount);
		Stats.PendingRequiredMeshPartCount =
			FMath::Max(0, Data->RequiredNowMeshPartCount - Data->RequiredResidentMeshPartCount);
		Stats.TransitionLocalMeshPartCount = Data->LastTransitionLocalPartCount;
		Stats.PendingLocalPreparationEntityCount = Data->LastPendingLocalPreparationEntityCount;
		Stats.PendingLocalRenderEntityCount = Data->LastPendingLocalRenderEntityCount;
		Stats.PendingLocalReleaseEntityCount = Data->LastPendingLocalReleaseEntityCount;
		Stats.HotPinnedEntityCount = Data->HotPinReferenceCounts.Num();
		Stats.HotPinMaintenanceEntityCount = Data->LastHotPinMaintenanceEntityCount;
		Stats.ActiveFarMeshPartCount = Data->LastActiveFarPartCount;
		Stats.TransitionFarMeshPartCount = Data->LastTransitionFarPartCount;
		Stats.OverlappingFarMeshPartCount = Data->LastOverlappingFarPartCount;
		Stats.VisibleCoreMissingMeshPartCount = Data->LastVisibleCoreMissingPartCount;
		Stats.AsyncSelectionInFlightCount = Data->AsyncState->InFlightCount.Load();
		Stats.LocalAsyncSelectionInFlightCount = Data->AsyncState->LocalInFlightCount.Load();
		Stats.StaleAsyncResultCount = Data->StaleAsyncResultCount;
		Stats.RapidRotationFrozenSourceCount = Data->LastRapidFrozenSourceCount;
		Stats.TransitionPhase = Data->LastTransitionPhase;
		Stats.CurrentMeshPoolWorkBudgetParts = Data->CurrentWorkBudgetParts;
		Stats.LastCycleAddedMeshPartCount = Data->LastAddedResidentPartCount;
		Stats.LastCycleRemovedMeshPartCount = Data->LastEvictedPartCount;
		Stats.LastSelectionMilliseconds = Data->LastSelectionMilliseconds;
		Stats.LastProjectionMilliseconds = Data->LastProjectionMilliseconds;
		Stats.SelectionPassCount = Data->SelectionPassCount;
		Stats.LocalSelectionPassCount = Data->LocalSelectionPassCount;
		Stats.StaticCellCount = IndexStats.StaticCellCount;
		Stats.MutableChunkCount = IndexStats.MutableChunkCount;
		Stats.StaticPackedEntryCount = IndexStats.StaticPackedEntryCount;
		Stats.StaticDeltaEntryCount = IndexStats.StaticDeltaEntryCount;
		Stats.StaticBVHBuildCount = IndexStats.StaticBVHBuildCount;
		Stats.IndexRevision = IndexStats.IndexRevision;
	}
	return Stats;
}

#if WITH_DEV_AUTOMATION_TESTS
void FBuildRenderProcessor::SetResidencyTimeSecondsForTesting(const double TimeSeconds)
{
	check(IsInGameThread());
	check(FMath::IsFinite(TimeSeconds));
	if (Data)
	{
		Data->TestingTimeSeconds = TimeSeconds;
	}
}

void FBuildRenderProcessor::SetResidencySelectionSynchronousForTesting(const bool bSynchronous)
{
	check(IsInGameThread());
	if (Data)
	{
		Data->bSynchronousResidencySelectionForTesting = bSynchronous;
	}
}

	#endif
