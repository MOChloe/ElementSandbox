// FBuildRenderProcessorData：Presentation Residency/Selection 状态机。
// FBuildPresentationIndex 以组合持有；本类不继承索引，也不拥有 Building Gameplay 真值。

class FBuildRenderProcessorData final
{
public:
	FBuildRenderProcessorData(const FBuildPresentationResidencyConfig& InResidencyConfig,
							  const FBuildRenderClusterConfig& InClusterConfig)
		: PresentationIndex(InResidencyConfig), ResidencyConfig(InResidencyConfig), ClusterConfig(InClusterConfig),
		  AsyncState(MakeShared<FBuildPresentationSelectionAsyncState, ESPMode::ThreadSafe>()),
		  CurrentWorkBudgetParts(InResidencyConfig.InitialMeshPoolWorkBudgetParts)
	{
	}

	~FBuildRenderProcessorData() { AsyncState->bActive.Store(false); }

	FRenderedEntity* FindRendered(const FBuildEntityHandle Entity)
	{
		return Entity.IsSet() ? RenderedEntities.Find(Entity) : nullptr;
	}

	const FRenderedEntity* FindRendered(const FBuildEntityHandle Entity) const
	{
		return const_cast<FBuildRenderProcessorData*>(this)->FindRendered(Entity);
	}

	void TrackRetiringInstance(
			const FBuildEntityHandle Entity,
			const FMeshPoolInstanceHandle Instance)
	{
		if (!Entity.IsSet() || !Instance.IsSet() || RetiringInstanceOwners.Contains(Instance))
		{
			return;
		}
		FRetiringRenderedEntity* Retiring = RetiringEntities.Find(Entity);
		if (!Retiring)
		{
			RetiringEntities.Add(Entity, FRetiringRenderedEntity());
			Retiring = RetiringEntities.Find(Entity);
		}
		check(Retiring);
		Retiring->Instances.Add(Instance);
		RetiringInstanceOwners.Add(Instance, Entity);
	}

	void NotifyInstanceRetired(const FMeshPoolInstanceHandle Instance)
	{
		const FBuildEntityHandle* Owner = RetiringInstanceOwners.Find(Instance);
		if (!Owner)
		{
			return;
		}
		const FBuildEntityHandle Entity = *Owner;
		RetiringInstanceOwners.Remove(Instance);
		FRetiringRenderedEntity* Retiring = RetiringEntities.Find(Entity);
		if (!Retiring)
		{
			return;
		}
		Retiring->Instances.RemoveSingleSwap(Instance, EAllowShrinking::No);
		if (Retiring->Instances.IsEmpty())
		{
			RetiringEntities.Remove(Entity);
		}
	}

	bool HasRetiringInstances(const FBuildEntityHandle Entity) const
	{
		const FRetiringRenderedEntity* Retiring = Entity.IsSet()
			? RetiringEntities.Find(Entity) : nullptr;
		return Retiring && !Retiring->Instances.IsEmpty();
	}

	bool IsRenderedComplete(const FBuildEntityHandle Entity) const
	{
		const FRenderedEntity* Rendered = FindRendered(Entity);
		return Rendered && Rendered->bComplete;
	}

	void QueueStorageRefresh(const FBuildEntityHandle Entity)
	{
		if (!Entity.IsSet() || PendingStorageRefreshSet.Contains(Entity))
		{
			return;
		}
		PendingStorageRefreshSet.Add(Entity);
		PendingStorageRefreshEntities.Add(Entity);
		bSelectionDirty = true;
	}

	FRenderedPart* FindRenderedPart(FRenderedEntity& Entity, const int32 PartId)
	{
		return Entity.Parts.FindByPredicate([PartId](const FRenderedPart& Part) { return Part.PartId == PartId; });
	}

	const FRenderedPart* FindRenderedPart(const FRenderedEntity& Entity, const int32 PartId) const
	{
		return const_cast<FBuildRenderProcessorData*>(this)->FindRenderedPart(const_cast<FRenderedEntity&>(Entity),
																			  PartId);
	}

	double GetNowSeconds() const
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (TestingTimeSeconds.IsSet())
		{
			return TestingTimeSeconds.GetValue();
		}
#endif
		return FPlatformTime::Seconds();
	}

	static void AdvanceNonZeroRevision(uint64& Revision)
	{
		if (++Revision == 0)
		{
			++Revision;
		}
	}

	double GetLocalSubscriptionRadius(const FLocalResidencyCache& Cache) const
	{
		return FMath::Max3(Cache.Boundary, ResidencyConfig.MinimumLocalRadius, ResidencyConfig.HotPromotionRadius);
	}

	FBox GetInvalidationCellBounds(const FIntVector& Coordinate) const
	{
		const double CellSize = ResidencyConfig.GameplayChunkSize;
		const FVector Min(static_cast<double>(Coordinate.X) * CellSize, static_cast<double>(Coordinate.Y) * CellSize,
						  static_cast<double>(Coordinate.Z) * CellSize);
		return FBox(Min, Min + FVector(CellSize));
	}

	bool TryAccumulateInvalidationCells(const FBox& Bounds, TSet<FIntVector>& OutCells) const
	{
		constexpr int64 MaximumCellsPerChangeBatch = 65536;
		FIntVector MinimumCell = FIntVector::ZeroValue;
		FIntVector MaximumCell = FIntVector::ZeroValue;
		if (!Bounds.IsValid ||
			!TryGetPresentationGridCoordinate(Bounds.Min, ResidencyConfig.GameplayChunkSize, MinimumCell) ||
			!TryGetPresentationGridCoordinate(Bounds.Max, ResidencyConfig.GameplayChunkSize, MaximumCell))
		{
			return false;
		}
		const int64 CountX = static_cast<int64>(MaximumCell.X) - MinimumCell.X + 1;
		const int64 CountY = static_cast<int64>(MaximumCell.Y) - MinimumCell.Y + 1;
		const int64 CountZ = static_cast<int64>(MaximumCell.Z) - MinimumCell.Z + 1;
		if (CountX <= 0 || CountY <= 0 || CountZ <= 0 || CountX > MaximumCellsPerChangeBatch ||
			CountY > MaximumCellsPerChangeBatch / CountX || CountZ > MaximumCellsPerChangeBatch / (CountX * CountY) ||
			CountX * CountY * CountZ + OutCells.Num() > MaximumCellsPerChangeBatch)
		{
			return false;
		}
		for (int64 Z = MinimumCell.Z; Z <= MaximumCell.Z; ++Z)
		{
			for (int64 Y = MinimumCell.Y; Y <= MaximumCell.Y; ++Y)
			{
				for (int64 X = MinimumCell.X; X <= MaximumCell.X; ++X)
				{
					OutCells.Add(FIntVector(static_cast<int32>(X), static_cast<int32>(Y), static_cast<int32>(Z)));
				}
			}
		}
		return true;
	}

	bool IsLocalSourceSubscribedToAnyCell(const FLocalResidencyCache& Cache, const TSet<FIntVector>& ChangedCells) const
	{
		if (Cache.SourceRevision == 0 || ChangedCells.IsEmpty())
		{
			return false;
		}
		// 未填满 Local 预算时，任意新 Cell 都可能提供下一条最近候选。
		if (Cache.MeshPartCost < ResidencyConfig.LocalResidentTargetMeshParts)
		{
			return true;
		}
		const double RadiusSquared = FMath::Square(GetLocalSubscriptionRadius(Cache));
		for (const FIntVector& Cell : ChangedCells)
		{
			if (GetInvalidationCellBounds(Cell).ComputeSquaredDistanceToPoint(Cache.SubjectLocation) <= RadiusSquared)
			{
				return true;
			}
		}
		return false;
	}

	bool IsFarSelectionSubscribedToAnyCell(const uint64 SourceRevision, const FVector2D& Direction,
										   const FVector& ViewLocation, const FVector& SubjectLocation,
										   const double BoundaryScore, const int32 SelectedMeshPartCost,
										   const int32 RequestedMeshPartCost,
										   const TSet<FIntVector>& ChangedCells) const
	{
		if (SourceRevision == 0 || RequestedMeshPartCost <= 0 || ChangedCells.IsEmpty())
		{
			return false;
		}
		const bool bUnderfilled = SelectedMeshPartCost < RequestedMeshPartCost;
		const FVector2D UnitDirection = Direction.GetSafeNormal();
		for (const FIntVector& Cell : ChangedCells)
		{
			const FBuildPresentationSectorVisibility Visibility =
				FBuildPresentationResidencySelector::EvaluateSectorBounds(
					GetInvalidationCellBounds(Cell), ViewLocation, SubjectLocation, UnitDirection,
					ResidencyConfig.ForwardCoverageAngleDegrees * 0.5);
			if (Visibility.bIntersects && (bUnderfilled || Visibility.Score <= BoundaryScore))
			{
				return true;
			}
		}
		return false;
	}

	bool IsFarSourceSubscribedToAnyCell(const FSourceResidencyState& State, const TSet<FIntVector>& ChangedCells) const
	{
		return IsFarSelectionSubscribedToAnyCell(State.ActiveFarSourceRevision, State.ActiveDirection,
												 State.ActiveViewLocation, State.ActiveSubjectLocation,
												 State.ActiveFarBoundaryScore, State.ActiveFarMeshPartCost,
												 State.ActiveFarRequestedMeshPartCost, ChangedCells) ||
			   IsFarSelectionSubscribedToAnyCell(
				   State.TransitionFarSourceRevision, State.TransitionDirection, State.TransitionViewLocation,
				   State.TransitionSubjectLocation, State.TransitionFarBoundaryScore,
				   State.TransitionFarTargetMeshPartCost, State.TransitionFarRequestedMeshPartCost, ChangedCells);
	}

	void InvalidateFarContent(FSourceResidencyState& State, const bool bAffectsTransition)
	{
		AdvanceNonZeroRevision(State.FarContentRevision);
		State.LastFarContentChangeTimeSeconds = GetNowSeconds();
		State.bNeedsFarSelection = true;
		State.bRefreshAfterProvisionalResult |= bAffectsTransition;
		if (bAffectsTransition)
		{
			State.bTargetRevisionCurrent = false;
		}
	}

	void PublishIndexChanges(TSet<FIntVector>&& ChangedCells, const bool bGlobalChange)
	{
		if (!bGlobalChange && ChangedCells.IsEmpty())
		{
			return;
		}
		AdvanceNonZeroRevision(IndexChangeSerial);
		if (bGlobalChange)
		{
			LastGlobalIndexChangeSerial = IndexChangeSerial;
		}
		else
		{
			for (const FIntVector& Cell : ChangedCells)
			{
				InvalidationCellRevisions.Add(Cell, IndexChangeSerial);
			}
		}
		for (TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : SourceStates)
		{
			FSourceResidencyState& State = Pair.Value;
			const bool bAffectsLocal = bGlobalChange || IsLocalSourceSubscribedToAnyCell(State.Local, ChangedCells);
			const bool bAffectsTarget =
				bGlobalChange || IsLocalSourceSubscribedToAnyCell(State.TargetLocal, ChangedCells);
			if (bAffectsLocal || bAffectsTarget)
			{
				AdvanceNonZeroRevision(State.LocalContentRevision);
				State.bNeedsLocalSelection = true;
				State.bRefreshLocalAfterProvisionalResult |= bAffectsTarget;
			}
			const bool bAffectsFar = bGlobalChange || IsFarSourceSubscribedToAnyCell(State, ChangedCells);
			if (bAffectsFar)
			{
				InvalidateFarContent(State, State.TransitionFarSourceRevision != 0);
			}
		}
	}

	void RecordIndexChanges(const FBox& FirstBounds, const FBox* SecondBounds = nullptr)
	{
		TSet<FIntVector> ChangedCells;
		const bool bGlobalChange = !TryAccumulateInvalidationCells(FirstBounds, ChangedCells) ||
								   (SecondBounds && !TryAccumulateInvalidationCells(*SecondBounds, ChangedCells));
		PublishIndexChanges(MoveTemp(ChangedCells), bGlobalChange);
	}

	bool HasLocalIndexChangesSince(const uint64 ChangeSerial, const FVector& SubjectLocation, const double Boundary,
								   const int32 TargetMeshPartCost) const
	{
		if (ChangeSerial == IndexChangeSerial)
		{
			return false;
		}

		if (LastGlobalIndexChangeSerial > ChangeSerial ||
			TargetMeshPartCost < ResidencyConfig.LocalResidentTargetMeshParts)
		{
			return true;
		}
		const double Radius =
			FMath::Max3(Boundary, ResidencyConfig.MinimumLocalRadius, ResidencyConfig.HotPromotionRadius);
		const double RadiusSquared = FMath::Square(Radius);
		for (const TPair<FIntVector, uint64>& Pair : InvalidationCellRevisions)
		{
			if (Pair.Value > ChangeSerial &&
				GetInvalidationCellBounds(Pair.Key).ComputeSquaredDistanceToPoint(SubjectLocation) <= RadiusSquared)
			{
				return true;
			}
		}
		return false;
	}

	bool HasFarIndexChangesSince(const FBuildFarSelectionResult& Result) const
	{
		if (Result.IndexChangeSerial == IndexChangeSerial)
		{
			return false;
		}
		if (LastGlobalIndexChangeSerial > Result.IndexChangeSerial)
		{
			return true;
		}
		const bool bUnderfilled = Result.TargetMeshPartCost < Result.RequestedMeshPartCost;
		for (const TPair<FIntVector, uint64>& Pair : InvalidationCellRevisions)
		{
			if (Pair.Value <= Result.IndexChangeSerial)
			{
				continue;
			}
			const FBuildPresentationSectorVisibility Visibility =
				FBuildPresentationResidencySelector::EvaluateSectorBounds(
					GetInvalidationCellBounds(Pair.Key), Result.ViewLocation, Result.SubjectLocation, Result.Forward,
					Result.CoverageAngleDegrees * 0.5);
			if (Visibility.bIntersects && (bUnderfilled || Visibility.Score <= Result.BoundaryScore))
			{
				return true;
			}
		}
		return false;
	}

	bool UpsertEntryAndNotify(const FBuildEntityHandle Entity, const FBox& Bounds, const int32 MeshPartCost,
							  const int32 DynamicPartCost, const bool bPackedStatic)
	{
		const FPresentationEntry* Existing = PresentationIndex.FindEntry(Entity);
		const bool bHadExisting = Existing != nullptr;
		const bool bChanged = !Existing || Existing->Bounds != Bounds || Existing->MeshPartCost != MeshPartCost ||
							  Existing->DynamicPartCost != DynamicPartCost || Existing->bPackedStatic != bPackedStatic;
		const int32 PreviousMeshPartCost = Existing ? Existing->MeshPartCost : 0;
		const FBox PreviousBounds = Existing ? Existing->Bounds : FBox(ForceInit);
		if (!PresentationIndex.UpsertEntry(Entity, Bounds, MeshPartCost, DynamicPartCost, bPackedStatic))
		{
			return false;
		}
		if (bChanged)
		{
			if (RequiredNowEntities.Contains(Entity))
			{
				RequiredNowMeshPartCount =
					FMath::Max(0, RequiredNowMeshPartCount + MeshPartCost - PreviousMeshPartCost);
			}
			RecordIndexChanges(Bounds, bHadExisting ? &PreviousBounds : nullptr);
			if (!FindRendered(Entity))
			{
				PinProvisionalHotEntity(Entity, Bounds);
			}
		}
		return true;
	}

	void RemoveEntryAndNotify(const FBuildEntityHandle Entity)
	{
		const FPresentationEntry* Existing = PresentationIndex.FindEntry(Entity);
		if (!Existing)
		{
			return;
		}
		const FBox PreviousBounds = Existing->Bounds;
		PresentationIndex.RemoveEntry(Entity);
		RecordIndexChanges(PreviousBounds);
	}

	void Clear()
	{
		PresentationIndex.Clear();
		InvalidationCellRevisions.Reset();
		AdvanceNonZeroRevision(IndexChangeSerial);
		LastGlobalIndexChangeSerial = IndexChangeSerial;
	}

	static FPresentationSourceKey MakeSourceKey(const FPresentationViewSource& View, const int32 SourceIndex)
	{
		FPresentationSourceKey Key;
		Key.Handle = View.SourceHandle;
		Key.FallbackIndex = View.SourceHandle.IsSet() ? INDEX_NONE : SourceIndex;
		return Key;
	}

	static FVector2D GetHorizontalForward(const FPresentationViewSource& View)
	{
		FVector2D Forward(View.Forward.X, View.Forward.Y);
		Forward = Forward.GetSafeNormal();
		return Forward.IsNearlyZero() ? FVector2D(1.0, 0.0) : Forward;
	}

	static double GetDirectionDeltaDegrees(const FVector2D& Left, const FVector2D& Right)
	{
		return FMath::RadiansToDegrees(
			FMath::Acos(FMath::Clamp(FVector2D::DotProduct(Left.GetSafeNormal(), Right.GetSafeNormal()), -1.0, 1.0)));
	}

	double GetRecenterThresholdDegrees(const FPresentationViewSource& View) const
	{
		return FMath::Max(
			ResidencyConfig.MinimumRecenterAngleDegrees,
			(ResidencyConfig.ForwardCoverageAngleDegrees - static_cast<double>(View.HorizontalFOVDegrees)) * 0.5 -
				ResidencyConfig.FOVSafetyAngleDegrees);
	}

	void AddResidencyReference(const FBuildEntityHandle Entity)
	{
		if (!Entity.IsSet())
		{
			return;
		}
		int32& Count = ResidencyReferenceCounts.FindOrAdd(Entity);
		if (Count++ == 0)
		{
			AddRequiredNowEntity(Entity);
		}
	}

	void ReleaseResidencyReference(const FBuildEntityHandle Entity, const double NowSeconds)
	{
		int32* Count = ResidencyReferenceCounts.Find(Entity);
		if (!Count)
		{
			return;
		}
		check(*Count > 0);
		if (--(*Count) == 0)
		{
			ResidencyReferenceCounts.Remove(Entity);
			RemoveRequiredNowEntity(Entity, NowSeconds);
		}
	}

	void AddHotPinReference(const FBuildEntityHandle Entity)
	{
		if (!Entity.IsSet())
		{
			return;
		}
		const bool bWasHot = IsEntityHot(Entity);
		int32& Count = HotPinReferenceCounts.FindOrAdd(Entity);
		check(Count >= 0);
		++Count;
		if (!bWasHot && IsEntityHot(Entity))
		{
			QueueStorageRefresh(Entity);
		}
	}

	void ReleaseHotPinReference(const FBuildEntityHandle Entity)
	{
		int32* Count = HotPinReferenceCounts.Find(Entity);
		if (!Count)
		{
			return;
		}
		const bool bWasHot = IsEntityHot(Entity);
		check(*Count > 0);
		if (--(*Count) == 0)
		{
			HotPinReferenceCounts.Remove(Entity);
		}
		if (bWasHot && !IsEntityHot(Entity))
		{
			QueueStorageRefresh(Entity);
		}
	}

	void QueueDeferredResidencyReleases(TArray<FBuildEntityHandle>&& Entities, const bool bQueueDirectionalCleanup)
	{
		if (!Entities.IsEmpty())
		{
			FDeferredResidencyReleaseBatch Batch;
			Batch.Entities = MoveTemp(Entities);
			Batch.bQueueDirectionalCleanup = bQueueDirectionalCleanup;
			PendingResidencyReleaseBatches.Add(MoveTemp(Batch));
		}
	}

	void QueueDeferredHotPinReleases(TArray<FBuildEntityHandle>&& Entities)
	{
		if (!Entities.IsEmpty())
		{
			FDeferredHotPinReleaseBatch Batch;
			Batch.Entities = MoveTemp(Entities);
			PendingHotPinReleaseBatches.Add(MoveTemp(Batch));
		}
	}

	int32 ProcessDeferredResidencyReleases(const double NowSeconds, const int32 MaxEntityCount)
	{
		int32 Processed = 0;
		while (Processed < MaxEntityCount && !PendingResidencyReleaseBatches.IsEmpty())
		{
			FDeferredResidencyReleaseBatch& Batch = PendingResidencyReleaseBatches.Last();
			if (Batch.Entities.IsEmpty())
			{
				PendingResidencyReleaseBatches.Pop(EAllowShrinking::No);
				continue;
			}
			const FBuildEntityHandle Entity = Batch.Entities.Pop(EAllowShrinking::No);
			ReleaseResidencyReference(Entity, NowSeconds);
			if (Batch.bQueueDirectionalCleanup)
			{
				QueueDirectionalCleanup(Entity);
			}
			++Processed;
			// 最后一项恰好耗尽本周期预算时也立即删除空壳；否则 HasPendingProjectionWork
			// 会凭一个空 Batch 额外唤醒下一周期。
			if (Batch.Entities.IsEmpty())
			{
				PendingResidencyReleaseBatches.Pop(EAllowShrinking::No);
			}
		}
		return Processed;
	}

	int32 ProcessDeferredHotPinReleases(const int32 MaxEntityCount)
	{
		int32 Processed = 0;
		while (Processed < MaxEntityCount && !PendingHotPinReleaseBatches.IsEmpty())
		{
			FDeferredHotPinReleaseBatch& Batch = PendingHotPinReleaseBatches.Last();
			if (Batch.Entities.IsEmpty())
			{
				PendingHotPinReleaseBatches.Pop(EAllowShrinking::No);
				continue;
			}
			ReleaseHotPinReference(Batch.Entities.Pop(EAllowShrinking::No));
			++Processed;
			++LastHotPinMaintenanceEntityCount;
			if (Batch.Entities.IsEmpty())
			{
				PendingHotPinReleaseBatches.Pop(EAllowShrinking::No);
			}
		}
		return Processed;
	}

	int32 ProcessDeferredLocalReleases(const double NowSeconds, const int32 MaxEntityCount)
	{
		if (MaxEntityCount <= 0)
		{
			return 0;
		}

		const bool bHasHotPinWork = !PendingHotPinReleaseBatches.IsEmpty();
		const bool bHasResidencyWork = !PendingResidencyReleaseBatches.IsEmpty();
		int32 HotPinBudget = bHasHotPinWork ? MaxEntityCount : 0;
		int32 ResidencyBudget = bHasResidencyWork ? MaxEntityCount : 0;
		if (bHasHotPinWork && bHasResidencyWork)
		{
			HotPinBudget = MaxEntityCount / 2;
			if ((MaxEntityCount & 1) != 0 && bPreferHotPinDeferredRelease)
			{
				++HotPinBudget;
			}
			ResidencyBudget = MaxEntityCount - HotPinBudget;
			bPreferHotPinDeferredRelease = !bPreferHotPinDeferredRelease;
		}

		int32 Processed = ProcessDeferredHotPinReleases(HotPinBudget);
		Processed += ProcessDeferredResidencyReleases(NowSeconds, ResidencyBudget);
		if (Processed < MaxEntityCount)
		{
			Processed += ProcessDeferredHotPinReleases(MaxEntityCount - Processed);
		}
		if (Processed < MaxEntityCount)
		{
			Processed += ProcessDeferredResidencyReleases(NowSeconds, MaxEntityCount - Processed);
		}
		return Processed;
	}

	void NotifyEntityRendered(const FBuildEntityHandle Entity)
	{
		for (TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : SourceStates)
		{
			Pair.Value.TargetLocal.PendingRenderEntities.Remove(Entity);
		}
	}

	void GetLocalTransitionBacklog(int32& OutPreparationCount, int32& OutWaitingRenderCount,
								   int32& OutReleaseCount) const
	{
		OutPreparationCount = 0;
		OutWaitingRenderCount = 0;
		OutReleaseCount = 0;
		for (const FDeferredResidencyReleaseBatch& Batch : PendingResidencyReleaseBatches)
		{
			OutReleaseCount += Batch.Entities.Num();
		}
		for (const FDeferredHotPinReleaseBatch& Batch : PendingHotPinReleaseBatches)
		{
			OutReleaseCount += Batch.Entities.Num();
		}
		for (const TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : SourceStates)
		{
			const FSourceResidencyState& State = Pair.Value;
			const FLocalResidencyCache& Target = State.TargetLocal;
			if (Target.SourceRevision == 0)
			{
				continue;
			}
			OutPreparationCount += FMath::Max(0, Target.OrderedEntities.Num() - Target.PreparationReadCursor);
			OutWaitingRenderCount += Target.PendingRenderEntities.Num();
			if (Target.bPreparationComplete && Target.PendingRenderEntities.IsEmpty())
			{
				OutReleaseCount += FMath::Max(0, State.Local.OrderedEntities.Num() - Target.PreviousLocalReleaseCursor);
			}
		}
	}

	void AddRequiredNowEntity(const FBuildEntityHandle Entity)
	{
		if (!Entity.IsSet() || RequiredNowEntities.Contains(Entity))
		{
			return;
		}
		RequiredNowEntities.Add(Entity);
		if (const FPresentationEntry* Entry = PresentationIndex.FindEntry(Entity))
		{
			RequiredNowMeshPartCount += Entry->MeshPartCost;
		}
		if (const FRenderedEntity* Rendered = FindRendered(Entity))
		{
			if (Rendered->bComplete)
			{
				++RequiredResidentEntityCount;
			}
			RequiredResidentMeshPartCount += Rendered->MeshPartCost;
		}
	}

	void RemoveRequiredNowEntity(const FBuildEntityHandle Entity, const double NowSeconds)
	{
		if (!RequiredNowEntities.Contains(Entity))
		{
			return;
		}
		if (const FPresentationEntry* Entry = PresentationIndex.FindEntry(Entity))
		{
			RequiredNowMeshPartCount = FMath::Max(0, RequiredNowMeshPartCount - Entry->MeshPartCost);
		}
		if (FRenderedEntity* Rendered = FindRendered(Entity))
		{
			if (Rendered->bComplete)
			{
				RequiredResidentEntityCount = FMath::Max(0, RequiredResidentEntityCount - 1);
			}
			RequiredResidentMeshPartCount = FMath::Max(0, RequiredResidentMeshPartCount - Rendered->MeshPartCost);
			Rendered->LastRequiredTimeSeconds = NowSeconds;
		}
		RequiredNowEntities.Remove(Entity);
	}

	static void AdvanceFarTargetCursor(FSourceResidencyState& State)
	{
		if (!State.TargetEntries.IsValidIndex(State.TargetCursor))
		{
			return;
		}
		if (State.TargetCursor < State.VisibleCoreEntryCount)
		{
			State.VisibleCoreRemainingMeshPartCost = FMath::Max(
				0, State.VisibleCoreRemainingMeshPartCost - State.TargetEntries[State.TargetCursor].MeshPartCost);
		}
		++State.TargetCursor;
	}

	void QueueUrgentResidentAdd(const FBuildEntityHandle Entity)
	{
		if (Entity.IsSet() && !IsRenderedComplete(Entity) && !PendingUrgentResidentAddSet.Contains(Entity))
		{
			PendingUrgentResidentAddSet.Add(Entity);
			PendingUrgentResidentAdds.Add(Entity);
		}
	}

	bool IsInsideCurrentHotCoverage(const FBox& Bounds) const
	{
		if (!Bounds.IsValid || ResidencyConfig.HotPromotionRadius <= 0.0)
		{
			return false;
		}
		const double RadiusSquared = FMath::Square(ResidencyConfig.HotPromotionRadius);
		for (const FPresentationViewSource& View : CurrentViews)
		{
			if (Bounds.ComputeSquaredDistanceToPoint(View.SubjectLocation) <= RadiusSquared)
			{
				return true;
			}
		}
		return false;
	}

	void PinProvisionalHotEntity(const FBuildEntityHandle Entity, const FBox& Bounds)
	{
		if (!Entity.IsSet() || ProvisionalHotEntities.Contains(Entity) || !IsInsideCurrentHotCoverage(Bounds))
		{
			return;
		}
		// Live Restore 可以在一个数万 Entity 的 Local 重选尚未完成时插入玩家脚边。
		// 临时所有权让它先进入 MeshPool；正常 Local/Hot 选择取得所有权后立即释放。
		ProvisionalHotEntities.Add(Entity);
		AddResidencyReference(Entity);
		AddHotPinReference(Entity);
		QueueUrgentResidentAdd(Entity);
		// 近处新插入实体不能排在普通 Local 准备队列之后；消费时仍共享本帧预算。
		PendingLiveResidentAdds.Add(Entity);
	}

	void MaintainProvisionalHotEntities(const FBuildEntityRegistry& Registry, const double NowSeconds)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_MaintainProvisionalHot);
		for (auto It = ProvisionalHotEntities.CreateIterator(); It; ++It)
		{
			const FBuildEntityHandle Entity = *It;
			const FPresentationEntry* Entry = PresentationIndex.FindEntry(Entity);
			const int32* ResidencyCount = ResidencyReferenceCounts.Find(Entity);
			const int32* HotPinCount = HotPinReferenceCounts.Find(Entity);
			const bool bNormalSelectionOwnsEntity = ResidencyCount && *ResidencyCount > 1 && HotPinCount && *HotPinCount > 1;
			const bool bStillHot = Entry && Registry.IsAlive(Entity) && IsInsideCurrentHotCoverage(Entry->Bounds);
			if (bStillHot && !bNormalSelectionOwnsEntity)
			{
				continue;
			}
			It.RemoveCurrent();
			ReleaseHotPinReference(Entity);
			ReleaseResidencyReference(Entity, NowSeconds);
		}
	}

	void QueueDirectionalCleanup(const FBuildEntityHandle Entity)
	{
		if (Entity.IsSet() && !RequiredNowEntities.Contains(Entity) && !PendingDirectionalCleanupSet.Contains(Entity))
		{
			PendingDirectionalCleanupSet.Add(Entity);
			PendingDirectionalCleanupEntities.Add(Entity);
		}
	}

	FBuildLocalSelectionRequest BuildLocalSelectionRequest(const FSourceResidencyState& State, const uint64 RequestId)
	{
		FBuildLocalSelectionRequest Request;
		Request.SourceToken = State.Token;
		Request.RequestId = RequestId;
		Request.SourceRevision = State.LocalContentRevision;
		Request.IndexChangeSerial = IndexChangeSerial;
		Request.SubjectLocation = State.LatestView.SubjectLocation;
		Request.MinimumLocalRadius = ResidencyConfig.MinimumLocalRadius;
		Request.HotPromotionRadius = ResidencyConfig.HotPromotionRadius;
		Request.TargetLocalMeshParts = ResidencyConfig.LocalResidentTargetMeshParts;
		PresentationIndex.CaptureSelectionSources(Request);
		return Request;
	}

	bool DispatchLocalSelection(FSourceResidencyState& State, const double NowSeconds,
								const bool bSynchronousForProjection)
	{
		if (State.bLocalSelectionInFlight || State.TargetLocal.SourceRevision != 0 ||
			(!bSynchronousForProjection && AsyncState->InFlightCount.Load() >= 2))
		{
			return false;
		}
		if (State.Phase != EBuildPresentationTransitionPhase::Stable || !State.TargetEntries.IsEmpty())
		{
			CancelTransition(State, NowSeconds);
		}
		else
		{
			InvalidateFarRequest(State);
		}

		const uint64 RequestId = ++State.LatestLocalRequestId;
		State.InFlightLocalRequestId = RequestId;
		State.bLocalSelectionInFlight = true;
		State.bNeedsLocalSelection = false;
		++LocalSelectionPassCount;
		FBuildLocalSelectionRequest Request = BuildLocalSelectionRequest(State, RequestId);
		TSharedRef<FBuildPresentationSelectionAsyncState, ESPMode::ThreadSafe> SharedAsyncState =
			AsyncState.ToSharedRef();
		++SharedAsyncState->InFlightCount;
		++SharedAsyncState->LocalInFlightCount;
		bool bRunSynchronously = bSynchronousForProjection;
#if WITH_DEV_AUTOMATION_TESTS
		bRunSynchronously |= bSynchronousResidencySelectionForTesting;
#endif
		if (bRunSynchronously)
		{
			SharedAsyncState->CompletedLocalResults.Enqueue(MakeUnique<FBuildLocalSelectionResult>(
				FBuildPresentationResidencySelector::SelectLocal(MoveTemp(Request))));
			--SharedAsyncState->LocalInFlightCount;
			--SharedAsyncState->InFlightCount;
			return true;
		}
		Async(EAsyncExecution::ThreadPool,
			  [SharedAsyncState, Request = MoveTemp(Request)]() mutable
			  {
				  TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_AsyncLocalSelect);
				  TUniquePtr<FBuildLocalSelectionResult> Result = MakeUnique<FBuildLocalSelectionResult>(
					  FBuildPresentationResidencySelector::SelectLocal(MoveTemp(Request)));
				  if (SharedAsyncState->bActive.Load())
				  {
					  SharedAsyncState->CompletedLocalResults.Enqueue(MoveTemp(Result));
				  }
				  --SharedAsyncState->LocalInFlightCount;
				  --SharedAsyncState->InFlightCount;
			  });
		return true;
	}

	void DiscardTargetLocal(FSourceResidencyState& State)
	{
		QueueDeferredResidencyReleases(MoveTemp(State.TargetLocal.AcquiredEntities), true);
		QueueDeferredHotPinReleases(MoveTemp(State.TargetLocal.AcquiredHotPinnedEntities));
		State.TargetLocal.Reset();
	}

	void DrainLocalSelectionResults(const double NowSeconds)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_DrainLocalResults);
		TUniquePtr<FBuildLocalSelectionResult> Result;
		while (AsyncState->CompletedLocalResults.Dequeue(Result))
		{
			FSourceResidencyState* State = FindSourceStateByToken(Result->SourceToken);
			if (!State)
			{
				++StaleAsyncResultCount;
				continue;
			}
			if (State->InFlightLocalRequestId == Result->RequestId)
			{
				State->bLocalSelectionInFlight = false;
				State->InFlightLocalRequestId = 0;
			}
			if (Result->RequestId != State->LatestLocalRequestId)
			{
				++StaleAsyncResultCount;
				continue;
			}

			const bool bRelevantIndexChangeDuringSelection = HasLocalIndexChangesSince(
				Result->IndexChangeSerial, Result->SubjectLocation, Result->Boundary, Result->TargetMeshPartCost);
			if (bRelevantIndexChangeDuringSelection && Result->SourceRevision == State->LocalContentRevision)
			{
				AdvanceNonZeroRevision(State->LocalContentRevision);
			}
			bool bRefreshAfterResult =
				State->bRefreshLocalAfterProvisionalResult || bRelevantIndexChangeDuringSelection ||
				Result->SourceRevision != State->LocalContentRevision ||
				FVector::DistSquared(Result->SubjectLocation, State->LatestView.SubjectLocation) >
					FMath::Square(ResidencyConfig.SourceMovementThreshold);
			State->bRefreshLocalAfterProvisionalResult = false;
			DiscardTargetLocal(*State);

			// 小结果保留原来的纯追加快路；大结果绝不能为了判断前缀而在一帧扫描整个 Local。
			const int32 ImmediatePublishLimit = ResidencyConfig.LocalTransitionPublishBudgetEntitiesPerCycle;
			bool bCanAppendToActive = State->Local.SourceRevision != 0 &&
									  !State->bRequiresLocalSnapshotReplacement &&
									  Result->OrderedTargetEntities.Num() <= ImmediatePublishLimit &&
									  State->Local.OrderedEntities.Num() <= Result->OrderedTargetEntities.Num();
			for (int32 Index = 0; bCanAppendToActive && Index < State->Local.OrderedEntities.Num(); ++Index)
			{
				bCanAppendToActive = State->Local.OrderedEntities[Index] == Result->OrderedTargetEntities[Index];
			}
			if (bCanAppendToActive)
			{
				FLocalResidencyCache& Local = State->Local;
				const int32 ExistingCount = Local.OrderedEntities.Num();
				const int32 PreviousMeshPartCost = Local.MeshPartCost;
				const FVector PreviousSubjectLocation = Local.SubjectLocation;
				Local.SourceRevision = Result->SourceRevision;
				Local.IndexChangeSerial = Result->IndexChangeSerial;
				Local.SubjectLocation = Result->SubjectLocation;
				Local.Boundary = Result->Boundary;
				TSet<FBuildEntityHandle> NewHotPinnedEntities = MoveTemp(Result->HotPinnedEntities);
				bool bMissingEntity = false;
				for (int32 Index = ExistingCount; Index < Result->OrderedTargetEntities.Num(); ++Index)
				{
					const FBuildEntityHandle Entity = Result->OrderedTargetEntities[Index];
					const FPresentationEntry* Entry = PresentationIndex.FindEntry(Entity);
					if (!Entry)
					{
						NewHotPinnedEntities.Remove(Entity);
						bMissingEntity = true;
						continue;
					}
					if (!Local.RequiredEntities.Contains(Entity))
					{
						Local.RequiredEntities.Add(Entity);
						Local.OrderedEntities.Add(Entity);
						Local.MeshPartCost += Entry->MeshPartCost;
						AddResidencyReference(Entity);
						QueueUrgentResidentAdd(Entity);
					}
				}
				if (!bMissingEntity)
				{
					Local.MeshPartCost = Result->TargetMeshPartCost;
					Local.RequiredEntitiesSnapshot = MoveTemp(Result->TargetEntitiesSnapshot);
				}
				else
				{
					Local.RefreshRequiredEntitiesSnapshot();
				}

				TArray<FBuildEntityHandle> NewOwnedHotPinnedEntities;
				NewOwnedHotPinnedEntities.Reserve(NewHotPinnedEntities.Num());
				for (const FBuildEntityHandle Entity : Local.OrderedEntities)
				{
					if (NewHotPinnedEntities.Contains(Entity))
					{
						NewOwnedHotPinnedEntities.Add(Entity);
					}
				}
				for (const FBuildEntityHandle Entity : Local.OwnedHotPinnedEntities)
				{
					if (!NewHotPinnedEntities.Contains(Entity))
					{
						ReleaseHotPinReference(Entity);
					}
				}
				for (const FBuildEntityHandle Entity : NewOwnedHotPinnedEntities)
				{
					if (!Local.HotPinnedEntities.Contains(Entity))
					{
						AddHotPinReference(Entity);
					}
				}
				Local.HotPinnedEntities = MoveTemp(NewHotPinnedEntities);
				Local.OwnedHotPinnedEntities = MoveTemp(NewOwnedHotPinnedEntities);
				LastHotPinMaintenanceEntityCount += Result->OrderedTargetEntities.Num();
				State->bNeedsLocalSelection = bRefreshAfterResult || bMissingEntity;
				if (Local.OrderedEntities.Num() != ExistingCount || Local.MeshPartCost != PreviousMeshPartCost ||
					FVector::DistSquared(Local.SubjectLocation, PreviousSubjectLocation) >
						FMath::Square(ResidencyConfig.SourceMovementThreshold))
				{
					InvalidateFarContent(*State, State->TransitionFarSourceRevision != 0);
				}
				LastLocalResidentBoundary = FMath::Max(LastLocalResidentBoundary, Local.Boundary);
			}
			else
			{
				FLocalResidencyCache& Target = State->TargetLocal;
				Target.SourceRevision = Result->SourceRevision;
				Target.IndexChangeSerial = Result->IndexChangeSerial;
				Target.SubjectLocation = Result->SubjectLocation;
				Target.Boundary = Result->Boundary;
				Target.RequiredEntities = MoveTemp(Result->TargetEntities);
				Target.RequiredEntitiesSnapshot = MoveTemp(Result->TargetEntitiesSnapshot);
				Target.OrderedEntities = MoveTemp(Result->OrderedTargetEntities);
				Target.HotPinnedEntities = MoveTemp(Result->HotPinnedEntities);
				Target.MeshPartCost = 0;
				Target.bPreparationComplete = Target.OrderedEntities.IsEmpty();
				// Tombstone 强制的整体换代必须持续到 TargetLocal 真正提交；若在这里只因
				// Worker 结果本身是 current 就清标志，下一周期会把旧 Local 误判为 cache hit
				// 并取消尚未提交的新 Target。provisional 结果提交后也仍须再次整体换代。
				State->bRequiresLocalSnapshotReplacement |= bRefreshAfterResult;
				State->bRefreshLocalAfterProvisionalResult = bRefreshAfterResult;
				State->bNeedsLocalSelection = false;
			}
			LastCandidateNodeCount += Result->CandidateNodeCount;
			LastCandidateEntryCount += Result->CandidateEntryCount;
			LastPrunedNodeCount += Result->PrunedNodeCount;
		}
	}

	int32 AdvanceTargetLocalPreparation(FSourceResidencyState& State, const FBuildEntityRegistry& Registry,
									const int32 MaxEntityCount)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_AdvanceLocalPreparation);
		FLocalResidencyCache& Target = State.TargetLocal;
		if (Target.SourceRevision == 0 || Target.bPreparationComplete || MaxEntityCount <= 0)
		{
			return 0;
		}

		int32 Processed = 0;
		while (Target.OrderedEntities.IsValidIndex(Target.PreparationReadCursor) && Processed < MaxEntityCount)
		{
			const FBuildEntityHandle Entity = Target.OrderedEntities[Target.PreparationReadCursor++];
			++Processed;
			const FPresentationEntry* Entry = PresentationIndex.FindEntry(Entity);
			if (!Entry || !Registry.IsAlive(Entity) || !Target.RequiredEntities.Contains(Entity))
			{
				// Worker 结果在该 SourceRevision 内保持不可变。Index/Registry 是 liveness gate；
				// 下一次完整 Local 选择负责整体替换含 stale Handle 的快照。
				State.bRefreshLocalAfterProvisionalResult = true;
				continue;
			}

			Target.OrderedEntities[Target.PreparationWriteCursor++] = Entity;
			Target.MeshPartCost += Entry->MeshPartCost;
			++LastHotPinMaintenanceEntityCount;
			if (Target.HotPinnedEntities.Contains(Entity))
			{
				Target.OwnedHotPinnedEntities.Add(Entity);
				if (!State.Local.HotPinnedEntities.Contains(Entity))
				{
					AddHotPinReference(Entity);
					Target.AcquiredHotPinnedEntities.Add(Entity);
				}
			}
			if (!State.Local.RequiredEntities.Contains(Entity))
			{
				AddResidencyReference(Entity);
				Target.AcquiredEntities.Add(Entity);
			}
			if (!IsRenderedComplete(Entity))
			{
				Target.PendingRenderEntities.Add(Entity);
				QueueUrgentResidentAdd(Entity);
			}
		}

		if (!Target.OrderedEntities.IsValidIndex(Target.PreparationReadCursor))
		{
			Target.OrderedEntities.SetNum(Target.PreparationWriteCursor, EAllowShrinking::No);
			Target.bPreparationComplete = true;
		}
		return Processed;
	}

	FBuildFarSelectionRequest BuildFarSelectionRequest(const FSourceResidencyState& State, const uint64 RequestId)
	{
		FBuildFarSelectionRequest Request;
		Request.SourceToken = State.Token;
		Request.RequestId = RequestId;
		Request.SourceRevision = State.FarContentRevision;
		Request.IndexChangeSerial = IndexChangeSerial;
		Request.ViewLocation = State.LatestView.ViewLocation;
		Request.SubjectLocation = State.LatestView.SubjectLocation;
		Request.Forward = GetHorizontalForward(State.LatestView);
		Request.HorizontalFOVDegrees = State.LatestView.HorizontalFOVDegrees;
		Request.CoverageAngleDegrees = ResidencyConfig.ForwardCoverageAngleDegrees;
		Request.FOVSafetyAngleDegrees = ResidencyConfig.FOVSafetyAngleDegrees;
		Request.TargetFarMeshParts =
			FMath::Max(0, ResidencyConfig.StableResidentTargetMeshParts - State.Local.MeshPartCost);
		Request.LocalExclusions = State.Local.RequiredEntitiesSnapshot;
		PresentationIndex.CaptureSelectionSources(Request);
		Request.ExistingActiveEntries = State.ActiveFarEntriesSnapshot;
		Request.ExistingTransitionEntries = State.TransitionFarEntriesSnapshot;
		Request.AllSubjectLocations.Reserve(CurrentViews.Num());
		for (const FPresentationViewSource& View : CurrentViews)
		{
			Request.AllSubjectLocations.Add(View.SubjectLocation);
		}
		return Request;
	}

	bool DispatchFarSelection(FSourceResidencyState& State)
	{
		if (State.bSelectionInFlight || AsyncState->InFlightCount.Load() >= 2)
		{
			return false;
		}
		const uint64 RequestId = ++State.LatestRequestId;
		State.InFlightRequestId = RequestId;
		State.InFlightDirection = GetHorizontalForward(State.LatestView);
		State.InFlightViewLocation = State.LatestView.ViewLocation;
		State.InFlightSubjectLocation = State.LatestView.SubjectLocation;
		State.bSelectionInFlight = true;
		State.bNeedsFarSelection = false;
		State.Phase = EBuildPresentationTransitionPhase::AsyncSelect;
		++SelectionPassCount;
		FBuildFarSelectionRequest Request = BuildFarSelectionRequest(State, RequestId);
		TSharedRef<FBuildPresentationSelectionAsyncState, ESPMode::ThreadSafe> SharedAsyncState =
			AsyncState.ToSharedRef();
		++SharedAsyncState->InFlightCount;
#if WITH_DEV_AUTOMATION_TESTS
		if (bSynchronousResidencySelectionForTesting)
		{
			SharedAsyncState->CompletedFarResults.Enqueue(
				MakeUnique<FBuildFarSelectionResult>(FBuildPresentationResidencySelector::Select(MoveTemp(Request))));
			--SharedAsyncState->InFlightCount;
			return true;
		}
#endif
		Async(EAsyncExecution::ThreadPool,
			  [SharedAsyncState, Request = MoveTemp(Request)]() mutable
			  {
				  TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_AsyncFarSelect);
				  TUniquePtr<FBuildFarSelectionResult> Result = MakeUnique<FBuildFarSelectionResult>(
					  FBuildPresentationResidencySelector::Select(MoveTemp(Request)));
				  if (SharedAsyncState->bActive.Load())
				  {
					  SharedAsyncState->CompletedFarResults.Enqueue(MoveTemp(Result));
				  }
				  --SharedAsyncState->InFlightCount;
			  });
		return true;
	}

	FSourceResidencyState* FindSourceStateByToken(const uint64 Token)
	{
		const FPresentationSourceKey* Key = SourceKeyByToken.Find(Token);
		return Key ? SourceStates.Find(*Key) : nullptr;
	}

	void DrainFarSelectionResults(const double NowSeconds)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_DrainFarResults);
		TUniquePtr<FBuildFarSelectionResult> Result;
		while (AsyncState->CompletedFarResults.Dequeue(Result))
		{
			FSourceResidencyState* State = FindSourceStateByToken(Result->SourceToken);
			if (!State)
			{
				++StaleAsyncResultCount;
				continue;
			}
			if (State->InFlightRequestId == Result->RequestId)
			{
				State->bSelectionInFlight = false;
				State->InFlightRequestId = 0;
			}
			if (Result->RequestId != State->LatestRequestId)
			{
				++StaleAsyncResultCount;
				continue;
			}

			const bool bRelevantIndexChangeDuringSelection = HasFarIndexChangesSince(*Result);
			if (bRelevantIndexChangeDuringSelection && Result->SourceRevision == State->FarContentRevision)
			{
				AdvanceNonZeroRevision(State->FarContentRevision);
			}
			const bool bRequestViewCurrent =
				GetDirectionDeltaDegrees(Result->Forward, GetHorizontalForward(State->LatestView)) <=
					GetRecenterThresholdDegrees(State->LatestView) &&
				FVector::DistSquared(Result->ViewLocation, State->LatestView.ViewLocation) <=
					FMath::Square(ResidencyConfig.SourceMovementThreshold) &&
				FVector::DistSquared(Result->SubjectLocation, State->LatestView.SubjectLocation) <=
					FMath::Square(ResidencyConfig.SourceMovementThreshold) &&
				FMath::IsNearlyEqual(Result->HorizontalFOVDegrees,
									 static_cast<double>(State->LatestView.HorizontalFOVDegrees), 0.01);
			const bool bResultCurrent = !bRelevantIndexChangeDuringSelection &&
				Result->SourceRevision == State->FarContentRevision && bRequestViewCurrent;
			// Select Worker 本来就用 Selected 集合去重；直接接管它。索引在 Worker 期间的
			// 变化由 HasFarIndexChangesSince 精确判定并触发下一次选择，不能在 GT 再扫描
			// 30 万/60 万个 Entry 重建同一份 TSet。
			TSet<FBuildEntityHandle> NewTargetSet = MoveTemp(Result->TargetEntities);
			const bool bAllTargetEntriesValid = NewTargetSet.Num() == Result->OrderedTargetEntries.Num();
			if (!bResultCurrent || !bAllTargetEntriesValid)
			{
				// 继续增长的索引不能发布为可见换代目标。保留当前 Active/Transition，
				// 等相关 Cell 静默后只选择一次当前快照，避免黄色实例反复 Add/Remove。
				State->bTargetRevisionCurrent = false;
				State->bNeedsFarSelection = true;
				continue;
			}
			if (State->SupersededTransitionCursor >= State->SupersededTransitionEntities.Num())
			{
				State->SupersededTransitionEntities = MoveTemp(Result->SupersededTransitionEntities);
				State->SupersededTransitionCursor = 0;
			}
			else
			{
				// 正常调度不会在旧差集未消费完时再发请求；保留防御性合并，重复 Handle
				// 在消费时由 TransitionFarSet.Remove 幂等过滤。
				State->SupersededTransitionEntities.Append(Result->SupersededTransitionEntities);
			}

			State->TransitionFarEntriesSnapshot = MoveTemp(Result->OrderedTargetEntriesSnapshot);
			State->TargetEntries = MoveTemp(Result->OrderedTargetEntries);
			State->TargetSet = MoveTemp(NewTargetSet);
			State->ReclaimOrder = MoveTemp(Result->ReclaimOrder);
			State->TargetCursor = 0;
			State->ReclaimCursor = 0;
			State->VisibleCoreEntryCount = Result->VisibleCoreEntryCount;
			State->VisibleCoreRemainingMeshPartCost = Result->VisibleCoreMeshPartCost;
			State->TransitionDirection = Result->Forward;
			State->TransitionViewLocation = Result->ViewLocation;
			State->TransitionSubjectLocation = Result->SubjectLocation;
			State->TransitionFarSourceRevision = Result->SourceRevision;
			State->TransitionFarBoundaryScore = Result->BoundaryScore;
			State->TransitionFarTargetMeshPartCost = Result->TargetMeshPartCost;
			State->TransitionFarRequestedMeshPartCost = Result->RequestedMeshPartCost;
			State->bTargetRevisionCurrent = true;
			State->bRefreshAfterProvisionalResult = false;
			State->TargetAcceptedTimeSeconds = -1.0;
			State->Phase = EBuildPresentationTransitionPhase::CatchUpVisible;
			LastCandidateNodeCount += Result->CandidateNodeCount;
			LastCandidateEntryCount += Result->CandidateEntryCount;
			LastPrunedNodeCount += Result->PrunedNodeCount;
			LastAcceptedSubtreeCount += Result->AcceptedSubtreeCount;
		}
	}

	bool IsDirectionCovered(const FVector2D& AnchorDirection, const FVector& AnchorViewLocation,
							const FVector& AnchorSubjectLocation, const uint64 AnchorSourceRevision,
							const uint64 CurrentSourceRevision, const FPresentationViewSource& View) const
	{
		return AnchorSourceRevision != 0 && AnchorSourceRevision == CurrentSourceRevision &&
			   GetDirectionDeltaDegrees(AnchorDirection, GetHorizontalForward(View)) <=
				   GetRecenterThresholdDegrees(View) &&
			   FVector::DistSquared(AnchorViewLocation, View.ViewLocation) <=
				   FMath::Square(ResidencyConfig.SourceMovementThreshold) &&
			   FVector::DistSquared(AnchorSubjectLocation, View.SubjectLocation) <=
				   FMath::Square(ResidencyConfig.SourceMovementThreshold);
	}

	void InvalidateFarRequest(FSourceResidencyState& State)
	{
		if (State.bSelectionInFlight && State.LatestRequestId == State.InFlightRequestId)
		{
			++State.LatestRequestId;
		}
		State.bNeedsFarSelection = true;
	}

	void CancelTransition(FSourceResidencyState& State, const double NowSeconds)
	{
		InvalidateFarRequest(State);
		for (const FBuildEntityHandle Entity : State.TransitionFarSet)
		{
			ReleaseResidencyReference(Entity, NowSeconds);
			QueueDirectionalCleanup(Entity);
		}
		State.TransitionFarSet.Reset();
		State.TransitionFarEntriesSnapshot.Reset();
		State.SupersededTransitionEntities.Reset();
		State.SupersededTransitionCursor = 0;
		State.TransitionFarMeshPartCost = 0;
		State.OverlappingFarMeshPartCost = 0;
		State.TransitionFarTargetMeshPartCost = 0;
		State.TransitionFarRequestedMeshPartCost = 0;
		State.TransitionFarBoundaryScore = 0.0;
		State.TransitionFarSourceRevision = 0;
		State.TransitionSubjectLocation = FVector::ZeroVector;
		State.TargetEntries.Reset();
		State.TargetSet.Reset();
		State.ReclaimOrder.Reset();
		State.TargetCursor = 0;
		State.ReclaimCursor = 0;
		State.VisibleCoreEntryCount = 0;
		State.VisibleCoreRemainingMeshPartCost = 0;
		State.TargetAcceptedTimeSeconds = -1.0;
		State.bRefreshAfterProvisionalResult = false;
		State.Phase = EBuildPresentationTransitionPhase::Stable;
		State.bNeedsFarSelection = false;
	}

	void UpdateSourceObservation(FSourceResidencyState& State, const FPresentationViewSource& View,
								 const double NowSeconds)
	{
		const FVector2D NewDirection = GetHorizontalForward(View);
		if (State.LastObservationTimeSeconds < 0.0)
		{
			State.LatestView = View;
			State.LastObservationTimeSeconds = NowSeconds;
			State.SettledSinceSeconds = NowSeconds;
			State.bNeedsFarSelection = true;
			return;
		}

		const FVector2D PreviousDirection = GetHorizontalForward(State.LatestView);
		const double DeltaSeconds = FMath::Max(UE_DOUBLE_SMALL_NUMBER, NowSeconds - State.LastObservationTimeSeconds);
		const double Cross = PreviousDirection.X * NewDirection.Y - PreviousDirection.Y * NewDirection.X;
		const double Dot = FVector2D::DotProduct(PreviousDirection, NewDirection);
		const double SignedDeltaDegrees = FMath::RadiansToDegrees(FMath::Atan2(Cross, Dot));
		const double AngularSpeed = FMath::Abs(SignedDeltaDegrees) / DeltaSeconds;
		const int32 TurnSign = SignedDeltaDegrees > 0.5 ? 1 : (SignedDeltaDegrees < -0.5 ? -1 : 0);
		while (!State.ReversalTimes.IsEmpty() &&
			   NowSeconds - State.ReversalTimes[0] > ResidencyConfig.RotationReversalWindowSeconds)
		{
			State.ReversalTimes.RemoveAt(0, 1, EAllowShrinking::No);
		}
		if (TurnSign != 0)
		{
			if (State.LastTurnSign != 0 && TurnSign != State.LastTurnSign &&
				NowSeconds - State.LastTurnTimeSeconds <= ResidencyConfig.RotationReversalWindowSeconds)
			{
				State.ReversalTimes.Add(NowSeconds);
				if (State.ReversalTimes.Num() >= 2)
				{
					State.PromotionLockedUntilSeconds = FMath::Max(
						State.PromotionLockedUntilSeconds, NowSeconds + ResidencyConfig.UnstablePromotionLockSeconds);
				}
			}
			State.LastTurnSign = TurnSign;
			State.LastTurnTimeSeconds = NowSeconds;
		}

		const bool bRapid = AngularSpeed >= ResidencyConfig.RapidRotationThresholdDegreesPerSecond;
		const bool bViewShapeChanged =
			!FMath::IsNearlyEqual(View.HorizontalFOVDegrees, State.LatestView.HorizontalFOVDegrees, 0.01f) ||
			!FMath::IsNearlyEqual(View.AspectRatio, State.LatestView.AspectRatio, 0.001f) ||
			View.ViewportSize != State.LatestView.ViewportSize;
		State.LatestView = View;
		State.LastObservationTimeSeconds = NowSeconds;
		if (bRapid)
		{
			if (!State.bRapidRotation)
			{
				InvalidateFarRequest(State);
			}
			State.bRapidRotation = true;
			State.SettledSinceSeconds = -1.0;
			State.PromotionLockedUntilSeconds = FMath::Max(State.PromotionLockedUntilSeconds,
														   NowSeconds + ResidencyConfig.UnstablePromotionLockSeconds);
			State.Phase = EBuildPresentationTransitionPhase::RapidSettling;
			return;
		}

		if (State.bRapidRotation)
		{
			State.bRapidRotation = false;
			State.SettledSinceSeconds = NowSeconds;
		}
		else if (State.SettledSinceSeconds < 0.0 || FMath::Abs(SignedDeltaDegrees) > 0.5)
		{
			State.SettledSinceSeconds = NowSeconds;
		}

		const bool bActiveCovered =
			State.bHasActiveDirection &&
			IsDirectionCovered(State.ActiveDirection, State.ActiveViewLocation, State.ActiveSubjectLocation,
							   State.ActiveFarSourceRevision, State.FarContentRevision, View);
		const bool bTransitionCovered =
			!State.TargetEntries.IsEmpty() &&
			GetDirectionDeltaDegrees(State.TransitionDirection, NewDirection) <= GetRecenterThresholdDegrees(View);
		const bool bTransitionLocationCovered =
			bTransitionCovered && FVector::DistSquared(State.TransitionViewLocation, View.ViewLocation) <=
									  FMath::Square(ResidencyConfig.SourceMovementThreshold);
		if (bViewShapeChanged || (!bActiveCovered && !bTransitionLocationCovered))
		{
			if (State.bSelectionInFlight &&
				GetDirectionDeltaDegrees(State.InFlightDirection, NewDirection) > GetRecenterThresholdDegrees(View))
			{
				InvalidateFarRequest(State);
			}
			State.bNeedsFarSelection = true;
		}
	}

	void CancelLocalTransition(FSourceResidencyState& State, const double NowSeconds)
	{
		if (State.bLocalSelectionInFlight && State.LatestLocalRequestId == State.InFlightLocalRequestId)
		{
			++State.LatestLocalRequestId;
		}
		DiscardTargetLocal(State);
		State.bRefreshLocalAfterProvisionalResult = false;
		State.bNeedsLocalSelection = false;
	}

	void UpdateLocalSelectionState(FSourceResidencyState& State, const double NowSeconds)
	{
		const bool bActiveCovered =
			!State.bRequiresLocalSnapshotReplacement && State.Local.SourceRevision != 0 &&
			State.Local.SourceRevision == State.LocalContentRevision &&
			FVector::DistSquared(State.Local.SubjectLocation, State.LatestView.SubjectLocation) <=
				FMath::Square(ResidencyConfig.SourceMovementThreshold);
		if (bActiveCovered)
		{
			if (State.TargetLocal.SourceRevision != 0 || State.bLocalSelectionInFlight)
			{
				CancelLocalTransition(State, NowSeconds);
			}
			State.bNeedsLocalSelection = false;
			++LastLocalSelectionCacheHitSourceCount;
			LastLocalResidentBoundary = FMath::Max(LastLocalResidentBoundary, State.Local.Boundary);
			return;
		}

		State.bNeedsLocalSelection = true;
	}

	int32 TryCommitLocalTransition(FSourceResidencyState& State, const double NowSeconds, const int32 MaxEntityCount)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_CommitLocalTransition);
		FLocalResidencyCache& Target = State.TargetLocal;
		if (Target.SourceRevision == 0 || !Target.bPreparationComplete || !Target.PendingRenderEntities.IsEmpty() ||
			MaxEntityCount <= 0)
		{
			return 0;
		}

		int32 Processed = 0;
		while (State.Local.OrderedEntities.IsValidIndex(Target.PreviousLocalReleaseCursor) &&
			   Processed < MaxEntityCount)
		{
			const FBuildEntityHandle Entity = State.Local.OrderedEntities[Target.PreviousLocalReleaseCursor++];
			if (!Target.RequiredEntities.Contains(Entity))
			{
				ReleaseResidencyReference(Entity, NowSeconds);
			}
			if (State.Local.HotPinnedEntities.Contains(Entity) && !Target.HotPinnedEntities.Contains(Entity))
			{
				ReleaseHotPinReference(Entity);
			}
			++LastHotPinMaintenanceEntityCount;
			++Processed;
		}
		if (State.Local.OrderedEntities.IsValidIndex(Target.PreviousLocalReleaseCursor))
		{
			return Processed;
		}

		State.Local = MoveTemp(Target);
		State.Local.AcquiredEntities.Reset();
		State.Local.AcquiredHotPinnedEntities.Reset();
		State.Local.PendingRenderEntities.Reset();
		State.Local.PreparationReadCursor = 0;
		State.Local.PreparationWriteCursor = 0;
		State.Local.PreviousLocalReleaseCursor = 0;
		State.Local.bPreparationComplete = false;
		State.TargetLocal.Reset();
		LastLocalResidentBoundary = FMath::Max(LastLocalResidentBoundary, State.Local.Boundary);
		State.bRequiresLocalSnapshotReplacement = State.bRefreshLocalAfterProvisionalResult;
		State.bNeedsLocalSelection =
			State.bRequiresLocalSnapshotReplacement || State.Local.SourceRevision != State.LocalContentRevision ||
			FVector::DistSquared(State.Local.SubjectLocation, State.LatestView.SubjectLocation) >
				FMath::Square(ResidencyConfig.SourceMovementThreshold);
		State.bRefreshLocalAfterProvisionalResult = false;
		InvalidateFarContent(State, State.TransitionFarSourceRevision != 0);
		return Processed;
	}

	FSourceResidencyState& FindOrAddSourceState(const FPresentationSourceKey& Key)
	{
		if (FSourceResidencyState* Existing = SourceStates.Find(Key))
		{
			return *Existing;
		}
		FSourceResidencyState& State = SourceStates.Add(Key);
		State.Token = NextSourceToken++;
		if (State.Token == 0)
		{
			State.Token = NextSourceToken++;
		}
		SourceKeyByToken.Add(State.Token, Key);
		return State;
	}

	void RemoveSourceState(const FPresentationSourceKey& Key, const double NowSeconds)
	{
		FSourceResidencyState* State = SourceStates.Find(Key);
		if (!State)
		{
			return;
		}
		++State->LatestRequestId;
		++State->LatestLocalRequestId;
		QueueDeferredResidencyReleases(MoveTemp(State->Local.OrderedEntities), false);
		QueueDeferredResidencyReleases(MoveTemp(State->TargetLocal.AcquiredEntities), false);
		QueueDeferredHotPinReleases(MoveTemp(State->Local.OwnedHotPinnedEntities));
		QueueDeferredHotPinReleases(MoveTemp(State->TargetLocal.AcquiredHotPinnedEntities));
		for (const FBuildEntityHandle Entity : State->ActiveFarSet)
		{
			ReleaseResidencyReference(Entity, NowSeconds);
		}
		for (const FBuildEntityHandle Entity : State->TransitionFarSet)
		{
			ReleaseResidencyReference(Entity, NowSeconds);
		}
		SourceKeyByToken.Remove(State->Token);
		SourceStates.Remove(Key);
	}

	void PurgeEntityResidencyReferences(const FBuildEntityHandle Entity)
	{
		const FPresentationEntry* ExistingEntry = PresentationIndex.FindEntry(Entity);
		const int32 MeshPartCost = ExistingEntry ? ExistingEntry->MeshPartCost : 0;
		if (const int32* ResidencyCount = ResidencyReferenceCounts.Find(Entity))
		{
			checkf(*ResidencyCount > 0, TEXT("Building residency reference count underflow before tombstone purge."));
		}
		if (const int32* HotPinCount = HotPinReferenceCounts.Find(Entity))
		{
			checkf(*HotPinCount > 0, TEXT("Building hot-pin reference count underflow before tombstone purge."));
		}
		ResidencyReferenceCounts.Remove(Entity);
		RemoveRequiredNowEntity(Entity, GetNowSeconds());
		HotPinReferenceCounts.Remove(Entity);
		ProvisionalHotEntities.Remove(Entity);
		MotionActiveEntities.Remove(Entity);
		PendingUrgentResidentAddSet.Remove(Entity);
		PendingDirectionalCleanupSet.Remove(Entity);
		PendingStorageRefreshSet.Remove(Entity);

		// Local/Target/Far 集合是某个 SourceRevision 的选择快照，Tombstone 不能逐实体改写。
		// 实际表现和全局引用已经同步退出；旧 Handle 由 Registry/PresentationIndex liveness gate 过滤，
		// 下一次有预算的选择整体换代。只有 PendingRenderEntities 是 GT preparation 的等待真值。
		for (TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : SourceStates)
		{
			FSourceResidencyState& State = Pair.Value;
			const bool bWasInLocal = State.Local.RequiredEntities.Contains(Entity);
			const bool bWasInTargetLocal = State.TargetLocal.RequiredEntities.Contains(Entity);
			const bool bWasInActiveFar = State.ActiveFarSet.Contains(Entity);
			const bool bWasInTransitionFar = State.TransitionFarSet.Contains(Entity);
			const bool bWasInFarTarget = State.TargetSet.Contains(Entity);

			// ExistingEntry 只在第一次真实 purge 时存在，因此每份缓存成本最多递减一次。
			if (ExistingEntry && bWasInLocal)
			{
				State.Local.MeshPartCost = FMath::Max(0, State.Local.MeshPartCost - MeshPartCost);
			}
			if (bWasInLocal || bWasInTargetLocal)
			{
				State.Local.SourceRevision = 0;
				State.bNeedsLocalSelection = true;
				State.bRequiresLocalSnapshotReplacement = true;
			}

			if (bWasInTargetLocal)
			{
				State.TargetLocal.PendingRenderEntities.Remove(Entity);
				if (ExistingEntry && State.TargetLocal.bPreparationComplete)
				{
					State.TargetLocal.MeshPartCost = FMath::Max(0, State.TargetLocal.MeshPartCost - MeshPartCost);
				}
				State.bRefreshLocalAfterProvisionalResult = true;
				State.bNeedsLocalSelection = true;
			}

			if (ExistingEntry && bWasInActiveFar)
			{
				State.ActiveFarMeshPartCost = FMath::Max(0, State.ActiveFarMeshPartCost - MeshPartCost);
			}
			if (ExistingEntry && bWasInTransitionFar)
			{
				State.TransitionFarMeshPartCost = FMath::Max(0, State.TransitionFarMeshPartCost - MeshPartCost);
			}
			if (ExistingEntry && bWasInActiveFar && bWasInTransitionFar)
			{
				State.OverlappingFarMeshPartCost = FMath::Max(0, State.OverlappingFarMeshPartCost - MeshPartCost);
			}
			if (bWasInFarTarget)
			{
				State.bTargetRevisionCurrent = false;
				State.bRefreshAfterProvisionalResult = true;
			}

			if (bWasInLocal || bWasInTargetLocal || bWasInActiveFar || bWasInTransitionFar || bWasInFarTarget)
			{
				InvalidateFarContent(State, State.TransitionFarSourceRevision != 0);
			}
		}
	}

	void ResetAsyncState()
	{
		AsyncState->bActive.Store(false);
		AsyncState = MakeShared<FBuildPresentationSelectionAsyncState, ESPMode::ThreadSafe>();
	}

	void SelectHotPinnedEntities(const TConstArrayView<FPresentationViewSource> Views,
								 TSet<FBuildEntityHandle>& OutHotPinned) const
	{
		OutHotPinned.Reset();
		PresentationIndex.GatherHotPinnedEntities(Views, ResidencyConfig.HotPromotionRadius, OutHotPinned);
	}

	bool IsEntityHot(const FBuildEntityHandle Entity) const
	{
		if (MotionActiveEntities.Contains(Entity))
		{
			return true;
		}
		return HotPinReferenceCounts.Contains(Entity);
	}

	FBuildPresentationIndex PresentationIndex;
	FBuildPresentationResidencyConfig ResidencyConfig;
	FBuildRenderClusterConfig ClusterConfig;
	/** 驻留记录使用 Dense TArray + Entity Slot 稀疏列；增长不 rehash，也不搬迁历史记录。 */
	TBuildEntitySparseMap<FRenderedEntity> RenderedEntities;
	TBuildEntitySparseMap<FRetiringRenderedEntity> RetiringEntities;
	TMap<FMeshPoolInstanceHandle, FBuildEntityHandle> RetiringInstanceOwners;
	FBuildEntitySparseSet RequiredNowEntities;
	int32 RequiredNowMeshPartCount = 0;
	int32 RequiredResidentEntityCount = 0;
	int32 RequiredResidentMeshPartCount = 0;
	TBuildEntitySparseMap<int32> ResidencyReferenceCounts;
	/** 所有 Source Local/Target Hot 所有权的全局引用计数；稳定周期不做集合重建。 */
	TBuildEntitySparseMap<int32> HotPinReferenceCounts;
	/** 尚未被异步 Local 选择接管的玩家近处新实体；只持有短期表现引用。 */
	TSet<FBuildEntityHandle> ProvisionalHotEntities;
	TArray<FBuildEntityHandle> PendingLiveResidentAdds;
	int32 PendingLiveResidentAddCursor = 0;
	TSet<FBuildEntityHandle> MotionActiveEntities;
	/** Local/Far 失效按 Gameplay Cell 记录最近变更；无关城市批次不再冲掉 Source 缓存。 */
	TMap<FIntVector, uint64> InvalidationCellRevisions;
	uint64 IndexChangeSerial = 1;
	uint64 LastGlobalIndexChangeSerial = 0;
	TMap<FPresentationSourceKey, FSourceResidencyState> SourceStates;
	TMap<uint64, FPresentationSourceKey> SourceKeyByToken;
	TSharedPtr<FBuildPresentationSelectionAsyncState, ESPMode::ThreadSafe> AsyncState;
	uint64 NextSourceToken = 1;
	TArray<FBuildEntityHandle> PendingUrgentResidentAdds;
	TSet<FBuildEntityHandle> PendingUrgentResidentAddSet;
	int32 PendingUrgentResidentAddCursor = 0;
	TArray<FBuildEntityHandle> PendingDirectionalCleanupEntities;
	TSet<FBuildEntityHandle> PendingDirectionalCleanupSet;
	int32 PendingDirectionalCleanupCursor = 0;
	TArray<FBuildEntityHandle> PendingStorageRefreshEntities;
	TSet<FBuildEntityHandle> PendingStorageRefreshSet;
	TArray<FDeferredResidencyReleaseBatch> PendingResidencyReleaseBatches;
	TArray<FDeferredHotPinReleaseBatch> PendingHotPinReleaseBatches;
	bool bPreferHotPinDeferredRelease = true;
	TArray<FPresentationViewSource> LastViews;
	TArray<FPresentationViewSource> CurrentViews;
	uint64 LastProjectedIndexRevision = 0;
	bool bSelectionDirty = true;
	int32 RenderedEntityCount = 0;
	int32 RenderedPartCount = 0;
	int32 LastCandidateNodeCount = 0;
	int32 LastCandidateEntryCount = 0;
	int32 LastPrunedNodeCount = 0;
	int32 LastAcceptedSubtreeCount = 0;
	int32 LastLocalSelectionCacheHitSourceCount = 0;
	int32 LastAddedResidentEntityCount = 0;
	int32 LastAddedResidentPartCount = 0;
	int32 CurrentWorkBudgetParts = 0;
	TArray<double, TInlineAllocator<5>> RecentInstanceApplyMilliseconds;
	uint64 LastObservedMeshPoolFlushCount = 0;
	uint64 StaleAsyncResultCount = 0;
	int32 LastActiveFarPartCount = 0;
	int32 LastTransitionLocalPartCount = 0;
	int32 LastTransitionFarPartCount = 0;
	int32 LastOverlappingFarPartCount = 0;
	int32 LastVisibleCoreMissingPartCount = 0;
	int32 LastRapidFrozenSourceCount = 0;
	int32 LastPendingLocalPreparationEntityCount = 0;
	int32 LastPendingLocalRenderEntityCount = 0;
	int32 LastPendingLocalReleaseEntityCount = 0;
	int32 LastHotPinMaintenanceEntityCount = 0;
	int32 LastResidencyMutationVisitedEntityCount = 0;
	EBuildPresentationTransitionPhase LastTransitionPhase = EBuildPresentationTransitionPhase::Stable;
	double LastSelectionMilliseconds = 0.0;
	double LastProjectionMilliseconds = 0.0;
	double LastResidencyMutationMilliseconds = 0.0;
	double LastSlowProjectionLogTimeSeconds = -TNumericLimits<double>::Max();
	int32 LastRequiredEntityCount = 0;
	int32 LastRequiredPartCount = 0;
	double LastLocalResidentBoundary = 0.0;
	int32 LastEvictionCandidateCount = 0;
	int32 LastEvictionGraceBlockedCount = 0;
	int32 LastEvictedEntityCount = 0;
	int32 LastEvictedPartCount = 0;
	double NextEvictionSweepTimeSeconds = 0.0;
	bool bOrdinaryEvictionPressureActive = false;
	bool bOrdinaryEvictionSweepInProgress = false;
	int32 OrdinaryEvictionSweepCursor = 0;
	uint64 SelectionPassCount = 0;
	uint64 LocalSelectionPassCount = 0;
	bool bSynchronizeNextLocalSelection = false;
#if WITH_DEV_AUTOMATION_TESTS
	TOptional<double> TestingTimeSeconds;
	bool bSynchronousResidencySelectionForTesting = false;
#endif
};
