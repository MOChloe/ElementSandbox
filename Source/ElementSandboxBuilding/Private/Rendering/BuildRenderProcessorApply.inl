// Build Render 的 MeshPool apply、Far transition、清理与普通缓存淘汰算法。
// 这些函数只消费 ProcessorData 已发布的选择结果。

namespace
{
bool RemoveRenderedEntity(FBuildRenderProcessorData& Data, UPresentationWorldSubsystem& Presentation,
							  const FBuildEntityHandle Entity)
{
	FRenderedEntity* Record = Data.FindRendered(Entity);
	if (!Record)
	{
		return true;
	}
	for (const FRenderedPart& Part : Record->Parts)
	{
		if (Presentation.IsInstancePhysicallyResident(Part.Instance))
		{
			Data.TrackRetiringInstance(Entity, Part.Instance);
		}
	}
	if (!FBuildPresentationMeshPoolApplicator::QueueRemoveEntity(Presentation, Record->Parts))
	{
		return false;
	}
	if (Data.RequiredNowEntities.Contains(Entity))
	{
		if (Record->bComplete)
		{
			Data.RequiredResidentEntityCount = FMath::Max(0, Data.RequiredResidentEntityCount - 1);
		}
		Data.RequiredResidentMeshPartCount = FMath::Max(0, Data.RequiredResidentMeshPartCount - Record->MeshPartCost);
	}
	Data.RenderedPartCount -= Record->Parts.Num();
	--Data.RenderedEntityCount;
	Data.RenderedEntities.Remove(Entity);
	return true;
}
bool AdvanceRenderedEntityAdd(FBuildRenderProcessorData& Data, const FBuildEntityRegistry& Registry,
							  UPresentationWorldSubsystem& Presentation, const FMeshPoolLayerHandle Layer,
							  const FBuildEntityHandle Entity, const double LastRequiredTimeSeconds,
							  const int32 MaximumMeshParts, const double DeadlineSeconds, int32& OutAddedPartCount,
								  bool& OutStartedEntity, bool& OutComplete, const bool bPrioritizePresentation = false)
{
	OutAddedPartCount = 0;
	OutStartedEntity = false;
	OutComplete = false;
	FRenderedEntity* Record = Data.FindRendered(Entity);
	if (Record && Record->bComplete)
	{
		Record->LastRequiredTimeSeconds = LastRequiredTimeSeconds;
		OutComplete = true;
		return true;
	}
	const int32 NextPartId = Record ? Record->NextPartId : 0;
	TArray<FBuildPresentationAppliedPart> SliceParts;
	int32 NewNextPartId = NextPartId;
	if (!FBuildPresentationMeshPoolApplicator::QueueAddEntitySlice(
		Registry, Presentation, Layer, Entity, Data.IsEntityHot(Entity), Data.ResidencyConfig,
		Data.ClusterConfig, NextPartId, MaximumMeshParts, DeadlineSeconds, NewNextPartId, OutComplete, SliceParts))
	{
		return false;
	}
	OutAddedPartCount = SliceParts.Num();
	if (bPrioritizePresentation)
	{
		for (const FBuildPresentationAppliedPart& Part : SliceParts)
		{
			if (!Presentation.PrioritizePendingInstance(Part.Instance)) return false;
		}
	}
	if (!Record && SliceParts.IsEmpty())
	{
		if (OutComplete) Data.NotifyEntityRendered(Entity);
		return true;
	}
	if (!Record)
	{
		FRenderedEntity NewRecord;
		NewRecord.LastRequiredTimeSeconds = LastRequiredTimeSeconds;
		Data.RenderedEntities.Add(Entity, MoveTemp(NewRecord));
		Record = Data.FindRendered(Entity);
		check(Record);
		++Data.RenderedEntityCount;
		OutStartedEntity = true;
	}
	Record->Parts.Reserve(Record->Parts.Num() + SliceParts.Num());
	for (FBuildPresentationAppliedPart& Part : SliceParts) Record->Parts.Add(MoveTemp(Part));
	Record->MeshPartCost = Record->Parts.Num();
	Record->NextPartId = NewNextPartId;
	Record->LastRequiredTimeSeconds = LastRequiredTimeSeconds;
	Record->bComplete = OutComplete;
	Data.RenderedPartCount += OutAddedPartCount;
	if (Data.RequiredNowEntities.Contains(Entity))
	{
		Data.RequiredResidentMeshPartCount += OutAddedPartCount;
		if (OutComplete)
		{
			++Data.RequiredResidentEntityCount;
		}
	}
	if (OutComplete) Data.NotifyEntityRendered(Entity);
	return true;
}

void UpdateAdaptiveWorkBudget(FBuildRenderProcessorData& Data, const UPresentationWorldSubsystem& Presentation)
{
	const FMeshPoolStats MeshPoolStats = Presentation.GetMeshPoolStats();
	if (MeshPoolStats.FlushCount == Data.LastObservedMeshPoolFlushCount)
	{
		return;
	}
	Data.LastObservedMeshPoolFlushCount = MeshPoolStats.FlushCount;
	if (MeshPoolStats.LastFlushInstanceCount <= 0 || MeshPoolStats.LastInstanceApplyMilliseconds <= 0.0)
	{
		return;
	}
	if (Data.RecentInstanceApplyMilliseconds.Num() == 5)
	{
		Data.RecentInstanceApplyMilliseconds.RemoveAt(0, 1, EAllowShrinking::No);
	}
	Data.RecentInstanceApplyMilliseconds.Add(MeshPoolStats.LastInstanceApplyMilliseconds);
	TArray<double, TInlineAllocator<5>> Sorted = Data.RecentInstanceApplyMilliseconds;
	Sorted.Sort();
	const double Median = Sorted[Sorted.Num() / 2];
	bool bEmergency = false;
	for (const TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : Data.SourceStates)
	{
		if (Pair.Value.Phase == EBuildPresentationTransitionPhase::CatchUpVisible)
		{
			bEmergency = true;
			break;
		}
	}
	const double TargetMilliseconds = bEmergency ? Data.ResidencyConfig.EmergencyInstanceApplyTargetMilliseconds
												 : Data.ResidencyConfig.NormalInstanceApplyTargetMilliseconds;
	if (Median > TargetMilliseconds)
	{
		Data.CurrentWorkBudgetParts =
			FMath::Max(Data.ResidencyConfig.MinimumMeshPoolWorkBudgetParts,
					   FMath::FloorToInt(static_cast<double>(Data.CurrentWorkBudgetParts) * 0.75));
	}
	else if (Median < TargetMilliseconds * 0.5)
	{
		Data.CurrentWorkBudgetParts =
			FMath::Min(Data.ResidencyConfig.MaximumMeshPoolWorkBudgetParts,
					   FMath::CeilToInt(static_cast<double>(Data.CurrentWorkBudgetParts) * 1.10));
	}
}

bool CanConsumeWorkBudget(const int32 Cost, const int32 Consumed, const int32 Budget)
{
	return Cost > 0 && (Cost <= Budget - Consumed || Consumed == 0);
}

/**
 * Mesh Part 预算限制一周期的吞吐量，时间预算限制一周期实际占用 Game Thread 的墙钟时间。
 * 两者必须同时满足；否则大量一 Part Entity 会绕过自适应预算并在一帧集中排队数千次实例变更。
 */
struct FBuildResidencyMutationBudget final
{
	explicit FBuildResidencyMutationBudget(const FBuildRenderProcessorData& Data)
		: StartSeconds(FPlatformTime::Seconds())
	{
		TargetMilliseconds = Data.ResidencyConfig.NormalInstanceApplyTargetMilliseconds;
		UpgradeForEmergency(Data);
	}

	void UpgradeForEmergency(const FBuildRenderProcessorData& Data)
	{
		for (const TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : Data.SourceStates)
		{
			if (Pair.Value.Phase == EBuildPresentationTransitionPhase::CatchUpVisible)
			{
				TargetMilliseconds = Data.ResidencyConfig.EmergencyInstanceApplyTargetMilliseconds;
				return;
			}
		}
	}

	bool CanVisitNextEntity() const
	{
		return VisitedEntityCount == 0 ||
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0 < TargetMilliseconds;
	}

	bool CanConsumeParts(const int32 Cost, const int32 PartBudget) const
	{
		return CanVisitNextEntity() && CanConsumeWorkBudget(Cost, ConsumedParts, PartBudget);
	}

	void VisitEntity() { ++VisitedEntityCount; }
	void ConsumeParts(const int32 Count) { ConsumedParts += Count; }

	double GetElapsedMilliseconds() const
	{
		return (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	}

	int32 ConsumedParts = 0;
	int32 VisitedEntityCount = 0;
	double GetDeadlineSeconds() const { return StartSeconds + TargetMilliseconds / 1000.0; }

private:
	double StartSeconds = 0.0;
	double TargetMilliseconds = 0.0;
};

bool ApplyResidentAddQueue(FBuildRenderProcessorData& Data, const FBuildEntityRegistry& Registry,
							 UPresentationWorldSubsystem& Presentation, const FMeshPoolLayerHandle Layer,
							 const double NowSeconds, FBuildResidencyMutationBudget& InOutBudget,
							 TArray<FBuildEntityHandle>& Queue, int32& Cursor, const bool bLive)
{
	constexpr int32 MaximumPartsPerEntitySlice = 16;
	while (Queue.IsValidIndex(Cursor) &&
		   InOutBudget.CanVisitNextEntity())
	{
		InOutBudget.VisitEntity();
		const FBuildEntityHandle Entity = Queue[Cursor];
		if (!Data.PendingUrgentResidentAddSet.Contains(Entity))
		{
			++Cursor;
			continue;
		}
		const FPresentationEntry* Entry = Data.PresentationIndex.FindEntry(Entity);
		if (!Entry || !Registry.IsAlive(Entity) || !Data.RequiredNowEntities.Contains(Entity))
		{
			Data.PendingUrgentResidentAddSet.Remove(Entity);
			++Cursor;
			continue;
		}
		if (FRenderedEntity* Rendered = Data.FindRendered(Entity); !bLive && Rendered && Rendered->bComplete)
		{
			Rendered->LastRequiredTimeSeconds = NowSeconds;
			Data.PendingUrgentResidentAddSet.Remove(Entity);
			++Cursor;
			continue;
		}
		if (!InOutBudget.CanConsumeParts(1, Data.CurrentWorkBudgetParts))
		{
			break;
		}
		const int32 RemainingPartBudget = FMath::Max(1, Data.CurrentWorkBudgetParts - InOutBudget.ConsumedParts);
		int32 AddedPartCount = 0;
		bool bStartedEntity = false;
		bool bComplete = false;
		if (!AdvanceRenderedEntityAdd(
			Data, Registry, Presentation, Layer, Entity, NowSeconds,
			FMath::Min(MaximumPartsPerEntitySlice, RemainingPartBudget), InOutBudget.GetDeadlineSeconds(),
			AddedPartCount, bStartedEntity, bComplete, bLive))
		{
			return false;
		}
		InOutBudget.ConsumeParts(AddedPartCount);
		Data.LastAddedResidentEntityCount += bStartedEntity ? 1 : 0;
		Data.LastAddedResidentPartCount += AddedPartCount;
		if (!bComplete) break;
		Data.PendingUrgentResidentAddSet.Remove(Entity);
		++Cursor;
	}
	if (!Queue.IsValidIndex(Cursor))
	{
		Queue.Reset();
		Cursor = 0;
	}
	return true;
}

bool ApplyUrgentResidentAdds(FBuildRenderProcessorData& Data, const FBuildEntityRegistry& Registry,
	UPresentationWorldSubsystem& Presentation, const FMeshPoolLayerHandle Layer,
	const double NowSeconds, FBuildResidencyMutationBudget& InOutBudget)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_ApplyUrgentResidentAdds);
	// 先消费近处新建实体，再推进普通 Local；两层队列共用实体去重集合及本帧预算。
	if (!ApplyResidentAddQueue(Data, Registry, Presentation, Layer, NowSeconds, InOutBudget,
		Data.PendingLiveResidentAdds, Data.PendingLiveResidentAddCursor, true)
		|| (Data.PendingLiveResidentAdds.IsEmpty()
			&& !ApplyResidentAddQueue(Data, Registry, Presentation, Layer, NowSeconds, InOutBudget,
				Data.PendingUrgentResidentAdds, Data.PendingUrgentResidentAddCursor, false)))
	{
		return false;
	}
	if (!Data.PendingUrgentResidentAdds.IsValidIndex(Data.PendingUrgentResidentAddCursor))
	{
		Data.PendingUrgentResidentAdds.Reset();
		Data.PendingUrgentResidentAddSet.Reset();
		Data.PendingUrgentResidentAddCursor = 0;
	}
	return true;
}

int64 GetNormalResidentHardLimit(const FBuildRenderProcessorData& Data)
{
	return static_cast<int64>(Data.SourceStates.Num()) *
		   static_cast<int64>(Data.ResidencyConfig.ResidentHardWatermarkMeshParts);
}

int64 GetTransitionResidentLimit(const FBuildRenderProcessorData& Data)
{
	const int64 PerSourceLimit =
		FMath::Min<int64>(Data.ResidencyConfig.ResidentHardWatermarkMeshParts,
						  static_cast<int64>(Data.ResidencyConfig.StableResidentTargetMeshParts) +
							  Data.ResidencyConfig.TransitionReserveMeshParts);
	return static_cast<int64>(Data.SourceStates.Num()) * PerSourceLimit;
}

bool RemoveActiveFarReference(FBuildRenderProcessorData& Data, FSourceResidencyState& State,
							  UPresentationWorldSubsystem& Presentation, const FBuildEntityHandle Entity,
							  const double NowSeconds, FBuildResidencyMutationBudget& InOutBudget)
{
	if (!State.ActiveFarSet.Remove(Entity))
	{
		return true;
	}
	const FPresentationEntry* Entry = Data.PresentationIndex.FindEntry(Entity);
	const int32 Cost = Entry ? Entry->MeshPartCost : 0;
	State.ActiveFarMeshPartCost = FMath::Max(0, State.ActiveFarMeshPartCost - Cost);
	if (State.TransitionFarSet.Contains(Entity))
	{
		State.OverlappingFarMeshPartCost = FMath::Max(0, State.OverlappingFarMeshPartCost - Cost);
	}
	Data.ReleaseResidencyReference(Entity, NowSeconds);
	if (Data.RequiredNowEntities.Contains(Entity) || Data.IsEntityHot(Entity))
	{
		return true;
	}
	const FRenderedEntity* Rendered = Data.FindRendered(Entity);
	const int32 RenderedCost = Rendered ? Rendered->MeshPartCost : Cost;
	if (Rendered && !InOutBudget.CanConsumeParts(RenderedCost, Data.CurrentWorkBudgetParts))
	{
		// 逻辑引用已经释放，实际实例进入下一次低优先级清理。
		Data.QueueDirectionalCleanup(Entity);
		return true;
	}
	if (Rendered && !RemoveRenderedEntity(Data, Presentation, Entity))
	{
		return false;
	}
	if (RenderedCost > 0 && Rendered)
	{
		InOutBudget.ConsumeParts(RenderedCost);
		++Data.LastEvictedEntityCount;
		Data.LastEvictedPartCount += RenderedCost;
	}
	return true;
}

bool ApplyFarTransition(FBuildRenderProcessorData& Data, FSourceResidencyState& State,
						const FBuildEntityRegistry& Registry, UPresentationWorldSubsystem& Presentation,
						const FMeshPoolLayerHandle Layer, const double NowSeconds,
						FBuildResidencyMutationBudget& InOutBudget)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_ApplyFarTransition);
	constexpr int32 MaximumPartsPerEntitySlice = 16;
	if (State.bRapidRotation || State.Phase == EBuildPresentationTransitionPhase::RapidSettling ||
		State.Phase == EBuildPresentationTransitionPhase::AsyncSelect ||
		State.Phase == EBuildPresentationTransitionPhase::Stable)
	{
		return true;
	}
	if (!State.bTargetRevisionCurrent)
	{
		// 相关 Cell 又发生变化时冻结旧 Transition，不继续把过期目标变成实例。
		// 当前 Active 与已加入的 Transition 都保持原样，直到静默后的新结果接管。
		return true;
	}

	const int64 TransitionLimit = GetTransitionResidentLimit(Data);
	const int64 EmergencyHardLimit =
		TransitionLimit + static_cast<int64>(Data.SourceStates.Num()) * Data.ResidencyConfig.EmergencyOverflowMeshParts;
	const auto IsCorePending = [&State]() { return State.TargetCursor < State.VisibleCoreEntryCount; };

	while (State.SupersededTransitionEntities.IsValidIndex(State.SupersededTransitionCursor) &&
		   InOutBudget.CanVisitNextEntity())
	{
		InOutBudget.VisitEntity();
		const FBuildEntityHandle Entity =
			State.SupersededTransitionEntities[State.SupersededTransitionCursor++];
		if (State.TargetSet.Contains(Entity) || !State.TransitionFarSet.Remove(Entity))
		{
			continue;
		}
		const FPresentationEntry* Entry = Data.PresentationIndex.FindEntry(Entity);
		const int32 Cost = Entry ? Entry->MeshPartCost : 0;
		State.TransitionFarMeshPartCost = FMath::Max(0, State.TransitionFarMeshPartCost - Cost);
		if (State.ActiveFarSet.Contains(Entity))
		{
			State.OverlappingFarMeshPartCost = FMath::Max(0, State.OverlappingFarMeshPartCost - Cost);
		}
		Data.ReleaseResidencyReference(Entity, NowSeconds);
		Data.QueueDirectionalCleanup(Entity);
	}
	if (State.SupersededTransitionCursor >= State.SupersededTransitionEntities.Num())
	{
		State.SupersededTransitionEntities.Reset();
		State.SupersededTransitionCursor = 0;
	}
	else
	{
		return true;
	}

	if (State.Phase == EBuildPresentationTransitionPhase::ReclaimOld)
	{
		const int32 NextCost = State.TargetEntries.IsValidIndex(State.TargetCursor)
								   ? State.TargetEntries[State.TargetCursor].MeshPartCost
								   : 0;
		while (State.ReclaimOrder.IsValidIndex(State.ReclaimCursor) && InOutBudget.CanVisitNextEntity() &&
			   static_cast<int64>(Data.RenderedPartCount + NextCost) > TransitionLimit)
		{
			InOutBudget.VisitEntity();
			const FBuildEntityHandle Entity = State.ReclaimOrder[State.ReclaimCursor++];
			if (!RemoveActiveFarReference(Data, State, Presentation, Entity, NowSeconds, InOutBudget))
			{
				return false;
			}
			if (InOutBudget.ConsumedParts >= Data.CurrentWorkBudgetParts)
			{
				return true;
			}
		}
		State.Phase = IsCorePending() ? EBuildPresentationTransitionPhase::CatchUpVisible
									  : EBuildPresentationTransitionPhase::FillTarget;
	}

	while (State.TargetEntries.IsValidIndex(State.TargetCursor) && InOutBudget.CanVisitNextEntity())
	{
		InOutBudget.VisitEntity();
		const bool bCore = IsCorePending();
		const FBuildPresentationSelectorEntry& Selected = State.TargetEntries[State.TargetCursor];
		const FPresentationEntry* Entry = Data.PresentationIndex.FindEntry(Selected.Entity);
		if (!Entry || !Registry.IsAlive(Selected.Entity) || State.Local.RequiredEntities.Contains(Selected.Entity))
		{
			FBuildRenderProcessorData::AdvanceFarTargetCursor(State);
			continue;
		}
		const bool bTransitionOwnsEntity = State.TransitionFarSet.Contains(Selected.Entity);
		if (bTransitionOwnsEntity && Data.IsRenderedComplete(Selected.Entity))
		{
			if (FRenderedEntity* Rendered = Data.FindRendered(Selected.Entity))
			{
				Rendered->LastRequiredTimeSeconds = NowSeconds;
			}
			FBuildRenderProcessorData::AdvanceFarTargetCursor(State);
			continue;
		}

		const bool bAlreadyResident = Data.FindRendered(Selected.Entity) != nullptr;
		const int64 CapacityLimit = bCore ? EmergencyHardLimit : TransitionLimit;
		if (!bAlreadyResident && static_cast<int64>(Data.RenderedPartCount + Entry->MeshPartCost) > CapacityLimit)
		{
			State.Phase = EBuildPresentationTransitionPhase::ReclaimOld;
			return true;
		}
		if (!Data.IsRenderedComplete(Selected.Entity) && !InOutBudget.CanConsumeParts(1, Data.CurrentWorkBudgetParts))
		{
			return true;
		}
		if (!bTransitionOwnsEntity)
		{
			Data.AddResidencyReference(Selected.Entity);
			State.TransitionFarSet.Add(Selected.Entity);
			State.TransitionFarMeshPartCost += Entry->MeshPartCost;
			if (State.ActiveFarSet.Contains(Selected.Entity))
			{
				State.OverlappingFarMeshPartCost += Entry->MeshPartCost;
			}
		}
		if (!Data.IsRenderedComplete(Selected.Entity))
		{
			const int32 RemainingPartBudget =
				FMath::Max(1, Data.CurrentWorkBudgetParts - InOutBudget.ConsumedParts);
			int32 AddedPartCount = 0;
			bool bStartedEntity = false;
			bool bComplete = false;
			if (!AdvanceRenderedEntityAdd(
				Data, Registry, Presentation, Layer, Selected.Entity, NowSeconds,
				FMath::Min(MaximumPartsPerEntitySlice, RemainingPartBudget), InOutBudget.GetDeadlineSeconds(),
				AddedPartCount, bStartedEntity, bComplete))
			{
				if (!bTransitionOwnsEntity)
				{
					State.TransitionFarSet.Remove(Selected.Entity);
					State.TransitionFarMeshPartCost =
						FMath::Max(0, State.TransitionFarMeshPartCost - Entry->MeshPartCost);
					if (State.ActiveFarSet.Contains(Selected.Entity))
					{
						State.OverlappingFarMeshPartCost =
							FMath::Max(0, State.OverlappingFarMeshPartCost - Entry->MeshPartCost);
					}
					Data.ReleaseResidencyReference(Selected.Entity, NowSeconds);
				}
				return false;
			}
			InOutBudget.ConsumeParts(AddedPartCount);
			Data.LastAddedResidentEntityCount += bStartedEntity ? 1 : 0;
			Data.LastAddedResidentPartCount += AddedPartCount;
			if (!bComplete) return true;
		}
		if (FRenderedEntity* Rendered = Data.FindRendered(Selected.Entity))
		{
			Rendered->LastRequiredTimeSeconds = NowSeconds;
		}
		FBuildRenderProcessorData::AdvanceFarTargetCursor(State);
		if (InOutBudget.ConsumedParts >= Data.CurrentWorkBudgetParts)
		{
			return true;
		}
		if (bCore && !IsCorePending())
		{
			State.Phase = EBuildPresentationTransitionPhase::FillTarget;
		}
	}

	if (!State.TargetEntries.IsValidIndex(State.TargetCursor))
	{
		if (State.TargetAcceptedTimeSeconds < 0.0)
		{
			State.TargetAcceptedTimeSeconds = NowSeconds;
		}
		const double RequiredStableSeconds = Data.ResidencyConfig.PromotionStableSeconds;
		if (State.bTargetRevisionCurrent &&
			NowSeconds - State.TargetAcceptedTimeSeconds + UE_SMALL_NUMBER >= RequiredStableSeconds &&
			NowSeconds + UE_SMALL_NUMBER >= State.PromotionLockedUntilSeconds)
		{
			for (const FBuildEntityHandle Entity : State.ActiveFarSet)
			{
				Data.ReleaseResidencyReference(Entity, NowSeconds);
				Data.QueueDirectionalCleanup(Entity);
			}
			State.ActiveFarSet = MoveTemp(State.TransitionFarSet);
			State.ActiveFarEntriesSnapshot = MoveTemp(State.TransitionFarEntriesSnapshot);
			State.ActiveFarMeshPartCost = State.TransitionFarMeshPartCost;
			State.ActiveFarRequestedMeshPartCost = State.TransitionFarRequestedMeshPartCost;
			State.ActiveFarBoundaryScore = State.TransitionFarBoundaryScore;
			State.ActiveDirection = State.TransitionDirection;
			State.ActiveViewLocation = State.TransitionViewLocation;
			State.ActiveSubjectLocation = State.TransitionSubjectLocation;
			State.ActiveFarSourceRevision = State.TransitionFarSourceRevision;
			State.bHasActiveDirection = true;
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
			State.SupersededTransitionEntities.Reset();
			State.SupersededTransitionCursor = 0;
			State.TargetCursor = 0;
			State.ReclaimCursor = 0;
			State.VisibleCoreEntryCount = 0;
			State.VisibleCoreRemainingMeshPartCost = 0;
			State.TargetAcceptedTimeSeconds = -1.0;
			State.Phase = EBuildPresentationTransitionPhase::CommitCleanup;
			State.bNeedsFarSelection = false;
			State.bRefreshAfterProvisionalResult = false;
		}
	}
	return true;
}

bool ApplyDirectionalCleanup(FBuildRenderProcessorData& Data, UPresentationWorldSubsystem& Presentation,
							 FBuildResidencyMutationBudget& InOutBudget)
{
	const int32 CleanupBudget = FMath::Max(1, Data.CurrentWorkBudgetParts / 2);
	while (Data.PendingDirectionalCleanupEntities.IsValidIndex(Data.PendingDirectionalCleanupCursor) &&
		   InOutBudget.ConsumedParts < CleanupBudget && InOutBudget.CanVisitNextEntity())
	{
		InOutBudget.VisitEntity();
		const FBuildEntityHandle Entity =
			Data.PendingDirectionalCleanupEntities[Data.PendingDirectionalCleanupCursor++];
		if (!Data.PendingDirectionalCleanupSet.Remove(Entity) || Data.RequiredNowEntities.Contains(Entity) ||
			Data.IsEntityHot(Entity))
		{
			continue;
		}
		const FRenderedEntity* Rendered = Data.FindRendered(Entity);
		if (!Rendered)
		{
			continue;
		}
		const int32 Cost = Rendered->MeshPartCost;
		if (!InOutBudget.CanConsumeParts(Cost, CleanupBudget))
		{
			--Data.PendingDirectionalCleanupCursor;
			Data.PendingDirectionalCleanupSet.Add(Entity);
			break;
		}
		if (!RemoveRenderedEntity(Data, Presentation, Entity))
		{
			return false;
		}
		InOutBudget.ConsumeParts(Cost);
		++Data.LastEvictedEntityCount;
		Data.LastEvictedPartCount += Cost;
	}
	if (!Data.PendingDirectionalCleanupEntities.IsValidIndex(Data.PendingDirectionalCleanupCursor))
	{
		Data.PendingDirectionalCleanupEntities.Reset();
		Data.PendingDirectionalCleanupSet.Reset();
		Data.PendingDirectionalCleanupCursor = 0;
		for (TPair<FPresentationSourceKey, FSourceResidencyState>& Pair : Data.SourceStates)
		{
			if (Pair.Value.Phase == EBuildPresentationTransitionPhase::CommitCleanup)
			{
				Pair.Value.Phase = EBuildPresentationTransitionPhase::Stable;
			}
		}
	}
	return true;
}

bool EvictOrdinaryCache(FBuildRenderProcessorData& Data, UPresentationWorldSubsystem& Presentation,
						const double NowSeconds, FBuildResidencyMutationBudget& InOutBudget)
{
	const int64 HighWatermark = GetNormalResidentHardLimit(Data);
	const bool bZeroSources = Data.SourceStates.IsEmpty();
	if (bZeroSources || static_cast<int64>(Data.RenderedPartCount) > HighWatermark)
	{
		Data.bOrdinaryEvictionPressureActive = true;
	}
	if (!Data.bOrdinaryEvictionPressureActive)
	{
		return true;
	}
	if (!Data.bOrdinaryEvictionSweepInProgress &&
		NowSeconds + UE_SMALL_NUMBER < Data.NextEvictionSweepTimeSeconds)
	{
		return true;
	}
	Data.bOrdinaryEvictionSweepInProgress = true;
	Data.LastEvictionGraceBlockedCount = 0;
	Data.LastEvictionCandidateCount = 0;
	const int64 Target =
		bZeroSources ? 0
					 : static_cast<int64>(Data.SourceStates.Num()) * Data.ResidencyConfig.StableResidentTargetMeshParts;
	bool bCompletedSweep = false;
	bool bRemovedAny = false;
	while (InOutBudget.CanVisitNextEntity() && static_cast<int64>(Data.RenderedPartCount) > Target)
	{
		const TArray<TBuildEntitySparseMap<FRenderedEntity>::FEntry, FBuildStableArrayAllocator>& Entries =
			Data.RenderedEntities.GetEntries();
		if (Entries.IsEmpty() || Data.OrdinaryEvictionSweepCursor >= Entries.Num())
		{
			Data.OrdinaryEvictionSweepCursor = 0;
			bCompletedSweep = true;
			break;
		}

		InOutBudget.VisitEntity();
		const FBuildEntityHandle Entity = Entries[Data.OrdinaryEvictionSweepCursor].Entity;
		const FRenderedEntity* Rendered = Data.FindRendered(Entity);
		if (!Rendered)
		{
			++Data.OrdinaryEvictionSweepCursor;
			continue;
		}
		if (Data.RequiredNowEntities.Contains(Entity) || Data.IsEntityHot(Entity))
		{
			++Data.OrdinaryEvictionSweepCursor;
			continue;
		}
		if (NowSeconds - Rendered->LastRequiredTimeSeconds + UE_SMALL_NUMBER <
			Data.ResidencyConfig.EvictionGraceSeconds)
		{
			++Data.LastEvictionGraceBlockedCount;
			++Data.OrdinaryEvictionSweepCursor;
			continue;
		}
		if (!Data.PresentationIndex.FindEntry(Entity))
		{
			++Data.OrdinaryEvictionSweepCursor;
			continue;
		}

		++Data.LastEvictionCandidateCount;
		const int32 Cost = Rendered->MeshPartCost;
		if (!InOutBudget.CanConsumeParts(Cost, Data.CurrentWorkBudgetParts))
		{
			break;
		}
		if (!RemoveRenderedEntity(Data, Presentation, Entity))
		{
			return false;
		}
		// Dense sparse-map 使用 swap-remove；不推进 Cursor，下一次先检查刚换入当前位置的尾元素。
		InOutBudget.ConsumeParts(Cost);
		bRemovedAny = true;
		++Data.LastEvictedEntityCount;
		Data.LastEvictedPartCount += Cost;
	}
	if (static_cast<int64>(Data.RenderedPartCount) <= Target)
	{
		Data.bOrdinaryEvictionPressureActive = false;
		Data.bOrdinaryEvictionSweepInProgress = false;
		Data.OrdinaryEvictionSweepCursor = 0;
		Data.NextEvictionSweepTimeSeconds = NowSeconds + 1.0 / Data.ResidencyConfig.EvictionFrequencyHz;
	}
	else if (bCompletedSweep || bRemovedAny)
	{
		// 扫描可以跨帧连续推进；一旦本轮真正淘汰过实例，仍保留配置的低频淘汰节奏。
		Data.bOrdinaryEvictionSweepInProgress = false;
		Data.NextEvictionSweepTimeSeconds = NowSeconds + 1.0 / Data.ResidencyConfig.EvictionFrequencyHz;
	}
	return true;
}

bool UpdateRenderedEntity(FBuildRenderProcessorData& Data, const FBuildEntityRegistry& Registry,
						  UPresentationWorldSubsystem& Presentation, const FMeshPoolLayerHandle Layer,
						  const FBuildEntityHandle Entity, const TConstArrayView<int32> RequestedParts)
{
	FRenderedEntity* Record = Data.FindRendered(Entity);
	if (!Record)
	{
		return true;
	}
	if (RequestedParts.IsEmpty())
	{
		return FBuildPresentationMeshPoolApplicator::QueueUpdateEntity(
			Registry, Presentation, Layer, Entity, Data.IsEntityHot(Entity), Data.ResidencyConfig,
			Data.ClusterConfig, RequestedParts, Record->Parts);
	}

	// 半装填实体只更新已经提交的 Part；尚未到达的 Part 会在后续 Slice 中直接读取最新 ECS 数据。
	TArray<int32, TInlineAllocator<8>> AppliedRequestedParts;
	for (const int32 PartId : RequestedParts)
	{
		if (Data.FindRenderedPart(*Record, PartId))
		{
			AppliedRequestedParts.Add(PartId);
		}
	}
	return AppliedRequestedParts.IsEmpty() || FBuildPresentationMeshPoolApplicator::QueueUpdateEntity(
		Registry, Presentation, Layer, Entity, Data.IsEntityHot(Entity), Data.ResidencyConfig,
		Data.ClusterConfig, AppliedRequestedParts, Record->Parts);
}

bool RefreshRenderedEntityStorage(FBuildRenderProcessorData& Data, const FBuildEntityRegistry& Registry,
								  UPresentationWorldSubsystem& Presentation, const FMeshPoolLayerHandle Layer,
								  const FBuildEntityHandle Entity)
{
	FRenderedEntity* Record = Data.FindRendered(Entity);
	if (!Record)
	{
		return true;
	}
	return FBuildPresentationMeshPoolApplicator::QueueStorageMigration(Registry, Presentation, Layer, Entity,
																	   Data.IsEntityHot(Entity), Data.ResidencyConfig,
																	   Data.ClusterConfig, Record->Parts);
}
} // namespace
