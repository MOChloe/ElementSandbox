#include "Runtime/ElementAuthorityExecution.h"

#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Query/ElementCentralQuery.h"
#include "Spatial/ElementBvh.h"

namespace
{
	uint64 AdvanceNonZero(const uint64 Value)
	{
		return Value == MAX_uint64 ? 1 : Value + 1;
	}

	bool MatchesDomain(const EElementTargetDomain Mask, const EElementTargetDomain Domain)
	{
		return EnumHasAnyFlags(Mask, Domain);
	}

	struct FSourceKey final
	{
		FElementEntityHandle Element;
		FName ProcessorId = NAME_None;

		friend bool operator==(const FSourceKey& Left, const FSourceKey& Right)
		{
			return Left.Element == Right.Element && Left.ProcessorId == Right.ProcessorId;
		}

		friend uint32 GetTypeHash(const FSourceKey& Key)
		{
			return HashCombineFast(GetTypeHash(Key.Element), GetTypeHash(Key.ProcessorId));
		}
	};

	struct FSourceRecord final
	{
		FElementInfluenceSnapshot Snapshot;
		FBox Bounds = FBox(ForceInit);
	};

	struct FTargetRecord final
	{
		FElementTargetSnapshot Snapshot;
		FBox Bounds = FBox(ForceInit);
	};

	struct FTargetState final
	{
		uint64 Revision = 1;
		int64 LastSettlementMilliseconds = 0;
		TMap<FName, double> NumericValues;
		TMap<FName, FElementStateValue> StateValues;
	};

	struct FQueuedMotion final
	{
		FElementMotionSubmission Submission;
		uint64 QueuedAuthorityCycle = 0;
	};

	struct FWakeKey final
	{
		FElementTargetKey Target;
		FName ProcessorId = NAME_None;

		friend bool operator==(const FWakeKey& Left, const FWakeKey& Right)
		{
			return Left.Target == Right.Target && Left.ProcessorId == Right.ProcessorId;
		}

		friend uint32 GetTypeHash(const FWakeKey& Key)
		{
			return HashCombineFast(GetTypeHash(Key.Target), GetTypeHash(Key.ProcessorId));
		}
	};

	struct FWakeRecord final
	{
		FWakeKey Key;
		uint64 Token = 0;
		uint64 ExpectedTargetRevision = 0;
		int64 DueTimeMilliseconds = 0;
	};

	struct FWakeMinHeapPredicate final
	{
		bool operator()(const FWakeRecord& Left, const FWakeRecord& Right) const
		{
			return Left.DueTimeMilliseconds < Right.DueTimeMilliseconds;
		}
	};

	struct FPendingTargetWrite final
	{
		struct FSourceDependency final
		{
			FElementEntityHandle Source;
			uint64 ExpectedRevision = 0;
		};

		FElementTargetKey Target;
		uint64 ExpectedSnapshotRevision = 0;
		uint64 ExpectedStateRevision = 0;
		int64 SettlementMilliseconds = 0;
		TMap<FName, double> NumericValues;
		TArray<FElementStateProcessorOutput> StateOutputs;
		TArray<FSourceDependency, TInlineAllocator<4>> SourceDependencies;
	};

	struct FReadyPage final
	{
		uint64 CycleToken = 0;
		TArray<FPendingTargetWrite> Targets;
	};

	struct FOffsetGroup final
	{
		FElementTargetKey Target;
		int32 Begin = 0;
		int32 End = 0;
	};

	struct FQueryTaskResult final
	{
		TArray<FElementQueryStatistics> Statistics;
		FElementBvhStats BvhStats;
		uint64 NarrowPhaseCount = 0;
	};

	FBox CalculateMotionBounds(
		const FElementTargetSnapshot& Target,
		const FElementMotionSubmission& Motion)
	{
		FBox Bounds(ForceInit);
		for (const FElementMotionSegment& Segment : Motion.Segments)
		{
			FElementCompoundShape Shape = Target.Shape;
			Shape.WorldTransform = Segment.PreviousTransform;
			Bounds += Shape.CalculateWorldBounds();
			Shape.WorldTransform = Segment.CurrentTransform;
			Bounds += Shape.CalculateWorldBounds();
		}
		return Bounds;
	}

	bool OffsetLess(const FElementOffset& Left, const FElementOffset& Right)
	{
		return Left.Target != Right.Target ? Left.Target < Right.Target : Left.Channel.LexicalLess(Right.Channel);
	}
}

class FElementAuthorityExecutionData final
{
public:
	explicit FElementAuthorityExecutionData(const FElementAuthorityExecutionConfig& InConfig)
		: Config(InConfig)
	{
		check(Config.IsValid());
	}

	const FTargetRecord* FindTarget(const FElementTargetKey Target) const
	{
		return Targets.Find(Target);
	}

	void RefreshStatCounts()
	{
		Stats.PendingWakeCount = ActiveWakes.Num();
		Stats.InfluenceCount = Sources.Num();
		Stats.TargetCount = Targets.Num();
	}

	/** 调用方必须持有 MotionSubmissionLock。 */
	void RefreshMotionStatCountsLocked()
	{
		Stats.PendingCriticalCount = CriticalMotions.Num();
		Stats.PendingNormalCount = NormalMotions.Num();
		Stats.PendingBackgroundCount = BackgroundMotions.Num();
	}

	void RequestTargetBvhPublish()
	{
		if (TargetSnapshotBatchDepth > 0)
		{
			bTargetBvhPublishPending = true;
			return;
		}
		TargetBvh.PublishSnapshot(EElementBvhPublishMode::DeferredLargeTopology);
	}

	void RemoveSourceRecord(const FSourceKey& Key, TArray<FBox>& DirtyRegions)
	{
		FSourceRecord Record;
		if (!Sources.RemoveAndCopyValue(Key, Record)) return;
		DirtyRegions.Add(Record.Bounds);
		SourceBySpatial.Remove(Record.Snapshot.SpatialHandle);
		InfluenceBvh.Remove(Record.Snapshot.SpatialHandle);
	}

	void CaptureDirtySources(TArray<FBox>& DirtyRegions)
	{
		FElementDirtyPage Page;
		if (!Registry.SealDirtyPage(Page)) return;
		for (const FElementEntityHandle Element : Page.Entities)
		{
			TSet<FName> CapturedProcessors;
			if (Registry.IsAlive(Element))
			{
				for (const TUniquePtr<FElementNumericProcessor>& Processor : NumericProcessors)
				{
					FElementInfluenceSnapshot Snapshot;
					if (!Processor->CaptureInfluence(Registry, Element, Snapshot)) continue;
					const FElementProcessorDescriptor& Descriptor = Processor->GetDescriptor();
					Snapshot.Source = Element;
					Snapshot.ProcessorId = Descriptor.ProcessorId;
					Snapshot.FragmentRevision = Registry.GetEntityRevision(Element);
					if (!Snapshot.IsValid()) continue;
					CapturedProcessors.Add(Descriptor.ProcessorId);
					const FSourceKey Key{Element, Descriptor.ProcessorId};
					const FBox NewBounds = Snapshot.Shape.CalculateWorldBounds();
					if (FSourceRecord* Existing = Sources.Find(Key))
					{
						DirtyRegions.Add(Existing->Bounds);
						Snapshot.SpatialHandle = Existing->Snapshot.SpatialHandle;
						InfluenceBvh.Update(Snapshot.SpatialHandle, NewBounds);
						Existing->Snapshot = MoveTemp(Snapshot);
						Existing->Bounds = NewBounds;
					}
					else
					{
						Snapshot.SpatialHandle = InfluenceBvh.Insert(NewBounds);
						if (!Snapshot.SpatialHandle.IsSet()) continue;
						FSourceRecord& Record = Sources.Add(Key);
						Record.Snapshot = MoveTemp(Snapshot);
						Record.Bounds = NewBounds;
						SourceBySpatial.Add(Record.Snapshot.SpatialHandle, Key);
					}
					DirtyRegions.Add(NewBounds);
				}
			}
			TArray<FSourceKey, TInlineAllocator<4>> Removed;
			for (const TPair<FSourceKey, FSourceRecord>& Pair : Sources)
			{
				if (Pair.Key.Element == Element && !CapturedProcessors.Contains(Pair.Key.ProcessorId))
				{
					Removed.Add(Pair.Key);
				}
			}
			for (const FSourceKey& Key : Removed) RemoveSourceRecord(Key, DirtyRegions);
		}
		InfluenceBvh.PublishSnapshot();
	}

	void CancelWake(const FWakeKey& Key)
	{
		if (ActiveWakes.Remove(Key) > 0) ++Stats.WakeCancelledCount;
	}

	void ScheduleWake(
		const FWakeKey& Key,
		const uint64 TargetRevision,
		const TOptional<int64>& DueTime)
	{
		if (!DueTime.IsSet())
		{
			CancelWake(Key);
			return;
		}
		const uint64 Token = AdvanceNonZero(NextWakeToken);
		NextWakeToken = Token;
		FWakeRecord Record;
		Record.Key = Key;
		Record.Token = Token;
		Record.ExpectedTargetRevision = TargetRevision;
		Record.DueTimeMilliseconds = DueTime.GetValue();
		if (ActiveWakes.Contains(Key)) ++Stats.WakeReplacedCount;
		else ++Stats.WakeScheduledCount;
		ActiveWakes.Add(Key, Record);
		WakeHeap.HeapPush(Record, FWakeMinHeapPredicate());
	}

	void PopDueWakes(const int64 Now, TArray<FWakeRecord>& OutDue)
	{
		OutDue.Reset();
		while (!WakeHeap.IsEmpty() && WakeHeap.HeapTop().DueTimeMilliseconds <= Now)
		{
			FWakeRecord Record;
			WakeHeap.HeapPop(Record, FWakeMinHeapPredicate(), EAllowShrinking::No);
			const FWakeRecord* Active = ActiveWakes.Find(Record.Key);
			if (!Active || Active->Token != Record.Token) continue;
			ActiveWakes.Remove(Record.Key);
			OutDue.Add(Record);
		}
	}

	void GatherAffectedTargets(
		const TArray<FBox>& DirtyRegions,
		TSet<FElementTargetKey>& OutTargets)
	{
		if (DirtyRegions.IsEmpty()) return;
		const TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> Snapshot = TargetBvh.GetPublishedSnapshot();
		if (Snapshot)
		{
			TArray<FElementSpatialSnapshotHandle> Candidates;
			for (const FBox& Region : DirtyRegions)
			{
				Snapshot->Query(Region, Candidates, &QueryStats);
				for (const FElementSpatialSnapshotHandle Handle : Candidates)
				{
					if (const FElementTargetKey* Target = TargetBySpatial.Find(Handle)) OutTargets.Add(*Target);
				}
			}
		}
		// Character 不建第二棵树；所有脏 Influence 区域先合并，再让连续数组每批只遍历一次。
		for (const FElementTargetKey& Character : CharacterTargets)
		{
			++Stats.CharacterSnapshotVisits;
			const FTargetRecord* Record = Targets.Find(Character);
			if (!Record) continue;
			for (const FBox& Region : DirtyRegions)
			{
				if (Record->Bounds.Intersect(Region))
				{
					OutTargets.Add(Character);
					break;
				}
			}
		}
	}

	void QueryCurrentTarget(
		const FElementTargetKey TargetKey,
		const int64 WindowStart,
		const int64 WindowEnd,
		FQueryTaskResult& OutResult) const
	{
		const FTargetRecord* Target = Targets.Find(TargetKey);
		const TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> Snapshot = InfluenceBvh.GetPublishedSnapshot();
		if (!Target || !Snapshot) return;
		TArray<FElementSpatialSnapshotHandle> Candidates;
		Snapshot->Query(Target->Bounds, Candidates, &OutResult.BvhStats);
		for (const FElementSpatialSnapshotHandle Handle : Candidates)
		{
			const FSourceKey* SourceKey = SourceBySpatial.Find(Handle);
			const FSourceRecord* Source = SourceKey ? Sources.Find(*SourceKey) : nullptr;
			const FElementNumericProcessor* const* Processor = SourceKey
				? NumericById.Find(SourceKey->ProcessorId) : nullptr;
				if (!Source || !Processor || Source->Snapshot.HostTarget == TargetKey
				|| !MatchesDomain((*Processor)->GetDescriptor().TargetDomains, TargetKey.Domain)) continue;
			++OutResult.NarrowPhaseCount;
			FElementQueryStatistics Statistics;
			if (WindowEnd > WindowStart)
			{
				FElementMotionSubmission Stationary;
				Stationary.Target = TargetKey;
				Stationary.ExpectedTargetRevision = Target->Snapshot.Revision;
				FElementMotionSegment& Segment = Stationary.Segments.AddDefaulted_GetRef();
				Segment.PreviousTransform = Target->Snapshot.Shape.WorldTransform;
				Segment.CurrentTransform = Target->Snapshot.Shape.WorldTransform;
				Segment.StartTimeMilliseconds = WindowStart;
				Segment.EndTimeMilliseconds = WindowEnd;
				if (!FElementCentralQuery::EvaluateMotion(
					Stationary, Target->Snapshot, Source->Snapshot,
					(*Processor)->GetDescriptor().WeightMode, Statistics)) continue;
			}
			else if (!FElementCentralQuery::EvaluateStatic(
				Target->Snapshot, Source->Snapshot, (*Processor)->GetDescriptor().WeightMode,
				WindowEnd, Statistics))
			{
				continue;
			}
			OutResult.Statistics.Add(MoveTemp(Statistics));
		}
	}

	void QueryMotion(
		const FElementMotionSubmission& Motion,
		FQueryTaskResult& OutResult) const
	{
		const FTargetRecord* Target = Targets.Find(Motion.Target);
		const TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> Snapshot = InfluenceBvh.GetPublishedSnapshot();
		if (!Target || !Snapshot || Target->Snapshot.Revision != Motion.ExpectedTargetRevision) return;
		TArray<FElementSpatialSnapshotHandle> Candidates;
		Snapshot->Query(CalculateMotionBounds(Target->Snapshot, Motion), Candidates, &OutResult.BvhStats);
		for (const FElementSpatialSnapshotHandle Handle : Candidates)
		{
			const FSourceKey* SourceKey = SourceBySpatial.Find(Handle);
			const FSourceRecord* Source = SourceKey ? Sources.Find(*SourceKey) : nullptr;
			const FElementNumericProcessor* const* Processor = SourceKey
				? NumericById.Find(SourceKey->ProcessorId) : nullptr;
				if (!Source || !Processor || Source->Snapshot.HostTarget == Motion.Target
				|| !MatchesDomain((*Processor)->GetDescriptor().TargetDomains, Motion.Target.Domain)) continue;
			++OutResult.NarrowPhaseCount;
			FElementQueryStatistics Statistics;
			if (FElementCentralQuery::EvaluateMotion(
				Motion, Target->Snapshot, Source->Snapshot,
				(*Processor)->GetDescriptor().WeightMode, Statistics))
			{
				OutResult.Statistics.Add(MoveTemp(Statistics));
			}
		}
	}

	FElementAuthorityExecutionConfig Config;
	FElementEntityRegistry Registry;
	FElementBvh InfluenceBvh;
	FElementBvh TargetBvh;
	FElementBvhStats QueryStats;
	TMap<FSourceKey, FSourceRecord> Sources;
	TMap<FElementSpatialSnapshotHandle, FSourceKey> SourceBySpatial;
	TMap<FElementTargetKey, FTargetRecord> Targets;
	TMap<FElementSpatialSnapshotHandle, FElementTargetKey> TargetBySpatial;
	TArray<FElementTargetKey> CharacterTargets;
	TMap<FElementTargetKey, int32> CharacterIndices;
	TMap<FElementTargetKey, FTargetState> TargetStates;

	TArray<TUniquePtr<FElementNumericProcessor>> NumericProcessors;
	TArray<TUniquePtr<FElementStateProcessor>> StateProcessors;
	TMap<FName, FElementNumericProcessor*> NumericById;
	TMap<FName, FElementStateProcessor*> StateById;
	TMap<FName, FElementStateProcessor*> StateByChannel;

		TArray<FQueuedMotion> CriticalMotions;
		TArray<FQueuedMotion> NormalMotions;
		TArray<FQueuedMotion> BackgroundMotions;
		FCriticalSection MotionSubmissionLock;
		TSet<FElementTargetKey> PendingTargetRefresh;
		TArray<FBox> ExplicitDirtySourceRegions;
		int32 TargetSnapshotBatchDepth = 0;
		bool bTargetBvhPublishPending = false;

	TArray<FWakeRecord> WakeHeap;
	TMap<FWakeKey, FWakeRecord> ActiveWakes;
	uint64 NextWakeToken = 0;
		uint64 NextCycleToken = 1;
		uint64 LastCommittedCycleToken = 0;
	TOptional<FReadyPage> Ready;

	TArray<FElementProjectionCommand> CommittedProjectionCommands;
	TArray<FElementStructuralCommand> CommittedStructuralCommands;
	FElementAuthorityExecutionStats Stats;
};

FElementAuthorityExecution::FElementAuthorityExecution(const FElementAuthorityExecutionConfig& Config)
{
	check(Config.IsValid());
	Data = MakeUnique<FElementAuthorityExecutionData>(Config);
}

FElementAuthorityExecution::~FElementAuthorityExecution() = default;

bool FElementAuthorityExecution::RegisterNumericProcessor(TUniquePtr<FElementNumericProcessor> Processor)
{
	check(IsInGameThread());
	if (!Data || !Processor || !Processor->GetDescriptor().IsNumericValid()
		|| Data->NumericById.Contains(Processor->GetDescriptor().ProcessorId)) return false;
	FElementNumericProcessor* Raw = Processor.Get();
	Data->NumericById.Add(Processor->GetDescriptor().ProcessorId, Raw);
	Data->NumericProcessors.Add(MoveTemp(Processor));
	return true;
}

bool FElementAuthorityExecution::RegisterStateProcessor(TUniquePtr<FElementStateProcessor> Processor)
{
	check(IsInGameThread());
	if (!Data || !Processor || !Processor->GetDescriptor().IsStateValid()
		|| Data->StateById.Contains(Processor->GetDescriptor().ProcessorId)
		|| Data->StateByChannel.Contains(Processor->GetDescriptor().OwnedStateChannel)) return false;
	FElementStateProcessor* Raw = Processor.Get();
	Data->StateById.Add(Processor->GetDescriptor().ProcessorId, Raw);
	Data->StateByChannel.Add(Processor->GetDescriptor().OwnedStateChannel, Raw);
	Data->StateProcessors.Add(MoveTemp(Processor));
	return true;
}

bool FElementAuthorityExecution::ValidateProcessorRegistry(FString* OutError) const
{
	if (!Data)
	{
		if (OutError) *OutError = TEXT("Authority Execution 未初始化。");
		return false;
	}
	for (const TUniquePtr<FElementNumericProcessor>& Processor : Data->NumericProcessors)
	{
		if (!Processor || !Processor->GetDescriptor().IsNumericValid())
		{
			if (OutError) *OutError = TEXT("存在无效 Numeric Processor 描述。");
			return false;
		}
	}
	for (const TUniquePtr<FElementStateProcessor>& Processor : Data->StateProcessors)
	{
		if (!Processor || !Processor->GetDescriptor().IsStateValid()
			|| Data->StateByChannel.FindRef(Processor->GetDescriptor().OwnedStateChannel) != Processor.Get())
		{
			if (OutError) *OutError = TEXT("同一 State Channel 必须只有一个写入者。");
			return false;
		}
	}
	return true;
}

FElementEntityHandle FElementAuthorityExecution::CreateElement(const FWorldEntityId PersistentId)
{
	check(IsInGameThread());
	return Data ? Data->Registry.CreateEntity(PersistentId) : FElementEntityHandle();
}

bool FElementAuthorityExecution::DestroyElement(const FElementEntityHandle Element)
{
	check(IsInGameThread());
	if (!Data || !Data->Registry.IsAlive(Element)) return false;
	TArray<FSourceKey, TInlineAllocator<4>> Removed;
	for (const TPair<FSourceKey, FSourceRecord>& Pair : Data->Sources)
	{
		if (Pair.Key.Element == Element) Removed.Add(Pair.Key);
	}
	for (const FSourceKey& Key : Removed)
	{
		Data->RemoveSourceRecord(Key, Data->ExplicitDirtySourceRegions);
	}
	Data->InfluenceBvh.PublishSnapshot();
	return Data->Registry.DestroyEntity(Element);
}

const FElementEntityRegistry& FElementAuthorityExecution::ReadRegistry() const
{
	check(Data);
	return Data->Registry;
}

FElementEntityRegistry& FElementAuthorityExecution::Registry()
{
	check(Data);
	return Data->Registry;
}

void FElementAuthorityExecution::BeginTargetSnapshotBatch()
{
	check(IsInGameThread());
	if (Data) ++Data->TargetSnapshotBatchDepth;
}

void FElementAuthorityExecution::EndTargetSnapshotBatch()
{
	check(IsInGameThread());
	if (!Data)
	{
		return;
	}
	check(Data->TargetSnapshotBatchDepth > 0);
	--Data->TargetSnapshotBatchDepth;
	if (Data->TargetSnapshotBatchDepth == 0 && Data->bTargetBvhPublishPending)
	{
		Data->bTargetBvhPublishPending = false;
		Data->TargetBvh.PublishSnapshot(EElementBvhPublishMode::DeferredLargeTopology);
	}
}

bool FElementAuthorityExecution::ReplaceTargetSnapshot(const FElementTargetSnapshot& Snapshot)
{
	check(IsInGameThread());
	if (!Data || !Snapshot.IsValid()) return false;
	const FBox Bounds = Snapshot.Shape.CalculateWorldBounds();
	FTargetRecord* Existing = Data->Targets.Find(Snapshot.Target);
	if (Existing && Snapshot.Revision <= Existing->Snapshot.Revision) return false;
	FElementTargetSnapshot Stored = Snapshot;
	if (Snapshot.Target.Domain == EElementTargetDomain::Character)
	{
		Stored.SpatialHandle = {};
		if (!Existing)
		{
			Data->CharacterIndices.Add(Snapshot.Target, Data->CharacterTargets.Add(Snapshot.Target));
		}
	}
	else if (Existing)
	{
		Stored.SpatialHandle = Existing->Snapshot.SpatialHandle;
		if (!Data->TargetBvh.Update(Stored.SpatialHandle, Bounds)) return false;
	}
	else
	{
		Stored.SpatialHandle = Data->TargetBvh.Insert(Bounds);
		if (!Stored.SpatialHandle.IsSet()) return false;
		Data->TargetBySpatial.Add(Stored.SpatialHandle, Snapshot.Target);
	}
	FTargetRecord& Record = Data->Targets.FindOrAdd(Snapshot.Target);
	Record.Snapshot = MoveTemp(Stored);
	Record.Bounds = Bounds;
	if (!Data->TargetStates.Contains(Snapshot.Target))
	{
		FTargetState& State = Data->TargetStates.Add(Snapshot.Target);
		State.LastSettlementMilliseconds = Snapshot.EffectiveTimeMilliseconds;
	}
	// Snapshot 的 EffectiveTime 描述几何从何时生效，不是稳定 Gameplay State 的结算提交。
	// 已有状态只能在 Authority Commit 时推进，否则嵌入绝对时间的 State Processor 会与外层时钟失配。
	Data->PendingTargetRefresh.Add(Snapshot.Target);
	if (Snapshot.Target.Domain != EElementTargetDomain::Character)
	{
		Data->RequestTargetBvhPublish();
	}
	Data->RefreshStatCounts();
	return true;
}

bool FElementAuthorityExecution::RemoveTargetSnapshot(
	const FElementTargetKey Target,
	const uint64 ExpectedRevision,
	const EElementTargetRemovalReason Reason)
{
	check(IsInGameThread());
	if (!Data) return false;
	const FTargetRecord* Existing = Data->Targets.Find(Target);
	if (!Existing || Existing->Snapshot.Revision != ExpectedRevision) return false;
	FTargetRecord Record;
	verify(Data->Targets.RemoveAndCopyValue(Target, Record));
	if (Target.Domain == EElementTargetDomain::Character)
	{
		const int32* FoundIndex = Data->CharacterIndices.Find(Target);
		if (FoundIndex)
		{
			const int32 Index = *FoundIndex;
			const int32 Last = Data->CharacterTargets.Num() - 1;
			if (Index != Last)
			{
				Data->CharacterTargets[Index] = Data->CharacterTargets[Last];
				Data->CharacterIndices[Data->CharacterTargets[Index]] = Index;
			}
			Data->CharacterTargets.Pop(EAllowShrinking::No);
			Data->CharacterIndices.Remove(Target);
		}
	}
	else
	{
		Data->TargetBySpatial.Remove(Record.Snapshot.SpatialHandle);
		Data->TargetBvh.Remove(Record.Snapshot.SpatialHandle);
		Data->RequestTargetBvhPublish();
	}
	Data->PendingTargetRefresh.Remove(Target);
	TArray<FWakeKey, TInlineAllocator<4>> WakeKeys;
	for (const TPair<FWakeKey, FWakeRecord>& Pair : Data->ActiveWakes)
	{
		if (Pair.Key.Target == Target) WakeKeys.Add(Pair.Key);
	}
	for (const FWakeKey& Key : WakeKeys) Data->CancelWake(Key);
	// RuntimeEvict 已由 WorldStorage Adapter 在移除宿主前捕获稳定状态；运行投影本身不保留
	// 隐形副本。GameplayDestroy 同样直接丢弃，不依赖任何 Leave 回调清理。
	Data->TargetStates.Remove(Target);
	Data->RefreshStatCounts();
	return true;
}

bool FElementAuthorityExecution::SubmitMotion(const FElementMotionSubmission& Submission)
{
	if (!Data || !Submission.IsValid()) return false;
	FScopeLock QueueLock(&Data->MotionSubmissionLock);
	FQueuedMotion Queued{Submission, Data->Stats.AuthorityCycle};
	TArray<FQueuedMotion>* Queue = &Data->NormalMotions;
	if (Submission.Priority == EElementQueryPriority::Critical) Queue = &Data->CriticalMotions;
	else if (Submission.Priority == EElementQueryPriority::Background) Queue = &Data->BackgroundMotions;
	// 同一开放页内完全相同的 Target/Revision/时间窗口只保留最后一份。
	for (int32 Index = Queue->Num() - 1; Index >= 0; --Index)
	{
		const FElementMotionSubmission& Existing = (*Queue)[Index].Submission;
		if (Existing.Target == Submission.Target
			&& Existing.ExpectedTargetRevision == Submission.ExpectedTargetRevision
			&& Existing.Segments[0].StartTimeMilliseconds == Submission.Segments[0].StartTimeMilliseconds
			&& Existing.Segments.Last().EndTimeMilliseconds == Submission.Segments.Last().EndTimeMilliseconds)
		{
			(*Queue)[Index] = MoveTemp(Queued);
			++Data->Stats.MotionSubmissionDeduplicatedCount;
			Data->RefreshMotionStatCountsLocked();
			return true;
		}
	}
	const int32 PendingCount = Data->CriticalMotions.Num() + Data->NormalMotions.Num()
		+ Data->BackgroundMotions.Num();
	if (PendingCount >= Data->Config.MaximumPendingMotionCount)
	{
		// 高优先级允许挤掉尚未开始的 Background 近似工作；否则明确反压给上游，
		// 由上游继续合并路径或稍后重试，绝不让开放页无限增长。
		if (Submission.Priority != EElementQueryPriority::Background && !Data->BackgroundMotions.IsEmpty())
		{
			Data->BackgroundMotions.RemoveAt(0, 1, EAllowShrinking::No);
		}
		else
		{
			++Data->Stats.MotionSubmissionRejectedCount;
			Data->RefreshMotionStatCountsLocked();
			return false;
		}
	}
	Queue->Add(MoveTemp(Queued));
	++Data->Stats.MotionSubmissionCount;
	Data->RefreshMotionStatCountsLocked();
	return true;
}

bool FElementAuthorityExecution::PumpWorkers(
	const int64 WorldTimeMilliseconds,
	const bool bAuthorityCollectionBoundary)
{
	check(IsInGameThread());
	if (!Data || WorldTimeMilliseconds < 0 || Data->Ready.IsSet()) return false;
	++Data->Stats.WorkerPumpCount;
	if (bAuthorityCollectionBoundary)
	{
		FScopeLock QueueLock(&Data->MotionSubmissionLock);
		++Data->Stats.AuthorityCycle;
	}

	TArray<FBox> DirtySourceRegions = MoveTemp(Data->ExplicitDirtySourceRegions);
	Data->ExplicitDirtySourceRegions.Reset();
	Data->CaptureDirtySources(DirtySourceRegions);
	// 即使本轮没有新空间写入，也要给已完成的后台 BVH 重建一次原子发布机会。
	Data->InfluenceBvh.PublishSnapshot();
	Data->TargetBvh.PublishSnapshot(EElementBvhPublishMode::DeferredLargeTopology);

	TArray<FQueuedMotion> SealedMotions;
	{
		FScopeLock QueueLock(&Data->MotionSubmissionLock);
		if (bAuthorityCollectionBoundary)
		{
			for (int32 Index = Data->BackgroundMotions.Num() - 1; Index >= 0; --Index)
			{
				if (Data->Stats.AuthorityCycle - Data->BackgroundMotions[Index].QueuedAuthorityCycle
					< Data->Config.BackgroundPromotionCycles) continue;
				Data->NormalMotions.Add(MoveTemp(Data->BackgroundMotions[Index]));
				Data->BackgroundMotions.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
		SealedMotions = MoveTemp(Data->CriticalMotions);
		Data->CriticalMotions.Reset();
		if (bAuthorityCollectionBoundary || Data->NormalMotions.Num() >= Data->Config.OpenPageCapacity)
		{
			SealedMotions.Append(MoveTemp(Data->NormalMotions));
			Data->NormalMotions.Reset();
		}
		if (bAuthorityCollectionBoundary && !Data->BackgroundMotions.IsEmpty())
		{
			const int32 Count = FMath::Min(Data->Config.BackgroundWorkBudget, Data->BackgroundMotions.Num());
			for (int32 Index = 0; Index < Count; ++Index)
			{
				SealedMotions.Add(MoveTemp(Data->BackgroundMotions[Index]));
			}
			Data->BackgroundMotions.RemoveAt(0, Count, EAllowShrinking::No);
		}
		Data->RefreshMotionStatCountsLocked();
	}
	SealedMotions.RemoveAll([Data = Data.Get()](const FQueuedMotion& Motion)
	{
		const FTargetRecord* Target = Data->FindTarget(Motion.Submission.Target);
		if (Target && Target->Snapshot.Revision == Motion.Submission.ExpectedTargetRevision) return false;
		++Data->Stats.StaleResultDropCount;
		return true;
	});
	// 连续运动已经覆盖这些目标的当前位置刷新；这里在封箱后、查询前统一折叠，
	// SubmitMotion 因而可以安全地从任意生产线程追加而不读取 Authority Map。
	for (const FQueuedMotion& Motion : SealedMotions)
	{
		Data->PendingTargetRefresh.Remove(Motion.Submission.Target);
	}
	TArray<FWakeRecord> DueWakes;
	Data->PopDueWakes(WorldTimeMilliseconds, DueWakes);
		TSet<FElementTargetKey> InstantRefreshTargets = MoveTemp(Data->PendingTargetRefresh);
		Data->PendingTargetRefresh.Reset();
		Data->GatherAffectedTargets(DirtySourceRegions, InstantRefreshTargets);
		TSet<FElementTargetKey> IntervalRefreshTargets;
		for (const FWakeRecord& Wake : DueWakes) IntervalRefreshTargets.Add(Wake.Key.Target);
		for (const FElementTargetKey Target : InstantRefreshTargets) IntervalRefreshTargets.Remove(Target);

		if (SealedMotions.IsEmpty() && InstantRefreshTargets.IsEmpty() && IntervalRefreshTargets.IsEmpty())
	{
		++Data->Stats.EmptyWorkerPumpCount;
		Data->Stats.BvhCandidateCount = Data->QueryStats.CandidateCount;
		Data->RefreshStatCounts();
		return false;
	}
		++Data->Stats.SealedInputPageCount;
		TSet<FElementTargetKey> ImpactedTargets = InstantRefreshTargets;
		ImpactedTargets.Append(IntervalRefreshTargets);
		for (const FQueuedMotion& Motion : SealedMotions) ImpactedTargets.Add(Motion.Submission.Target);

		TArray<FElementTargetKey> InstantTargets;
		InstantTargets.Reserve(InstantRefreshTargets.Num());
		for (const FElementTargetKey Target : InstantRefreshTargets) InstantTargets.Add(Target);
		TArray<FElementTargetKey> IntervalTargets;
		IntervalTargets.Reserve(IntervalRefreshTargets.Num());
		for (const FElementTargetKey Target : IntervalRefreshTargets) IntervalTargets.Add(Target);

		TArray<FQueryTaskResult> MotionResults;
		MotionResults.SetNum(SealedMotions.Num());
		ParallelFor(SealedMotions.Num(), [Data = Data.Get(), &SealedMotions, &MotionResults](const int32 Index)
		{
			Data->QueryMotion(SealedMotions[Index].Submission, MotionResults[Index]);
		});
		TArray<FQueryTaskResult> InstantResults;
		InstantResults.SetNum(InstantTargets.Num());
		// Source/Target 的变化在当前时间生效：只查询边界后的新速率，旧速率由 State
		// Processor 从上次结算点积分到本边界。这样新增 Source 不会被错误地倒灌到过去。
		ParallelFor(InstantTargets.Num(),
			[Data = Data.Get(), &InstantTargets, &InstantResults, WorldTimeMilliseconds](const int32 Index)
		{
			Data->QueryCurrentTarget(
				InstantTargets[Index], WorldTimeMilliseconds, WorldTimeMilliseconds, InstantResults[Index]);
		});
		TArray<FQueryTaskResult> IntervalResults;
		IntervalResults.SetNum(IntervalTargets.Num());
		// Timer 唤醒表示输入在整个稳定区间内未变化，可以直接积分完整时间窗口。
		ParallelFor(IntervalTargets.Num(),
			[Data = Data.Get(), &IntervalTargets, &IntervalResults, WorldTimeMilliseconds](const int32 Index)
		{
			const FElementTargetKey Target = IntervalTargets[Index];
			const FTargetState* State = Data->TargetStates.Find(Target);
			const int64 Start = State ? State->LastSettlementMilliseconds : WorldTimeMilliseconds;
			Data->QueryCurrentTarget(Target, Start, WorldTimeMilliseconds, IntervalResults[Index]);
		});

		TArray<FElementQueryStatistics> Statistics;
		auto AppendQueryResults = [Data = Data.Get(), &Statistics](TArray<FQueryTaskResult>& Results)
		{
			for (FQueryTaskResult& Result : Results)
			{
				Data->QueryStats.QueryCount += Result.BvhStats.QueryCount;
				Data->QueryStats.NodeVisitCount += Result.BvhStats.NodeVisitCount;
				Data->QueryStats.CandidateCount += Result.BvhStats.CandidateCount;
				Data->Stats.NarrowPhaseCount += Result.NarrowPhaseCount;
				Statistics.Append(MoveTemp(Result.Statistics));
			}
		};
		AppendQueryResults(MotionResults);
		AppendQueryResults(InstantResults);
		AppendQueryResults(IntervalResults);

		TMap<FName, TArray<FElementQueryStatistics>> StatisticsByProcessor;
		TMap<FElementTargetKey, TMap<FElementEntityHandle, uint64>> SourceRevisionsByTarget;
		for (FElementQueryStatistics& Entry : Statistics)
		{
			SourceRevisionsByTarget.FindOrAdd(Entry.Target).Add(Entry.Source, Entry.SourceRevision);
			StatisticsByProcessor.FindOrAdd(Entry.ProcessorId).Add(MoveTemp(Entry));
	}
	TArray<TArray<FElementOffset>> ThreadLocalOffsets;
	ThreadLocalOffsets.SetNum(Data->NumericProcessors.Num());
	ParallelFor(Data->NumericProcessors.Num(), [Data = Data.Get(), &StatisticsByProcessor, &ThreadLocalOffsets](const int32 Index)
	{
		FElementNumericProcessor& Processor = *Data->NumericProcessors[Index];
		const TArray<FElementQueryStatistics>* Inputs = StatisticsByProcessor.Find(
			Processor.GetDescriptor().ProcessorId);
		if (Inputs && !Inputs->IsEmpty()) Processor.Execute(*Inputs, ThreadLocalOffsets[Index]);
	});
	TArray<FElementOffset> Offsets;
	for (TArray<FElementOffset>& Local : ThreadLocalOffsets)
	{
		if (!Local.IsEmpty()) ++Data->Stats.NumericProcessorInvocationCount;
		for (FElementOffset& Offset : Local) if (Offset.IsValid()) Offsets.Add(MoveTemp(Offset));
	}
	Data->Stats.OffsetCount += Offsets.Num();
	Offsets.Sort(OffsetLess);
	TArray<FOffsetGroup> Groups;
	for (int32 Begin = 0; Begin < Offsets.Num();)
	{
		int32 End = Begin + 1;
		while (End < Offsets.Num() && Offsets[End].Target == Offsets[Begin].Target) ++End;
		Groups.Add({Offsets[Begin].Target, Begin, End});
		Begin = End;
	}
	for (const FElementTargetKey Target : ImpactedTargets)
	{
		if (!Groups.ContainsByPredicate([Target](const FOffsetGroup& Group){ return Group.Target == Target; }))
		{
			Groups.Add({Target, 0, 0});
		}
	}
	Groups.Sort([](const FOffsetGroup& Left, const FOffsetGroup& Right){ return Left.Target < Right.Target; });

	FReadyPage Ready;
	Ready.CycleToken = Data->NextCycleToken;
	Data->NextCycleToken = AdvanceNonZero(Data->NextCycleToken);
	Ready.Targets.SetNum(Groups.Num());
		ParallelFor(Groups.Num(), [Data = Data.Get(), &Groups, &Offsets, &Ready, &SourceRevisionsByTarget, WorldTimeMilliseconds](const int32 GroupIndex)
		{
		const FOffsetGroup& Group = Groups[GroupIndex];
		FPendingTargetWrite& Write = Ready.Targets[GroupIndex];
		Write.Target = Group.Target;
		const FTargetRecord* Target = Data->Targets.Find(Group.Target);
		const FTargetState* Current = Data->TargetStates.Find(Group.Target);
		if (!Target || !Current) return;
			Write.ExpectedSnapshotRevision = Target->Snapshot.Revision;
			Write.ExpectedStateRevision = Current->Revision;
			if (const TMap<FElementEntityHandle, uint64>* Dependencies = SourceRevisionsByTarget.Find(Group.Target))
			{
				Write.SourceDependencies.Reserve(Dependencies->Num());
				for (const TPair<FElementEntityHandle, uint64>& Pair : *Dependencies)
				{
					Write.SourceDependencies.Add({Pair.Key, Pair.Value});
				}
				Write.SourceDependencies.Sort([](const FPendingTargetWrite::FSourceDependency& Left,
					const FPendingTargetWrite::FSourceDependency& Right){ return Left.Source < Right.Source; });
			}
			Write.SettlementMilliseconds = WorldTimeMilliseconds;
			Write.NumericValues = Current->NumericValues;
			for (const TUniquePtr<FElementNumericProcessor>& NumericProcessor : Data->NumericProcessors)
			{
				if (!MatchesDomain(NumericProcessor->GetDescriptor().TargetDomains, Group.Target.Domain)) continue;
				for (const FName Channel : NumericProcessor->GetDescriptor().RecomputedNumericChannels)
				{
					Write.NumericValues.Add(Channel, 0.0);
				}
			}
		for (int32 OffsetIndex = Group.Begin; OffsetIndex < Group.End;)
		{
			const FName Channel = Offsets[OffsetIndex].Channel;
			double Delta = 0.0;
			do
			{
				Delta += Offsets[OffsetIndex].Delta;
				++OffsetIndex;
			} while (OffsetIndex < Group.End && Offsets[OffsetIndex].Channel == Channel);
			Write.NumericValues.FindOrAdd(Channel) += Delta;
		}
		for (const TUniquePtr<FElementStateProcessor>& StateProcessor : Data->StateProcessors)
		{
			const FElementProcessorDescriptor& Descriptor = StateProcessor->GetDescriptor();
			if (!MatchesDomain(Descriptor.TargetDomains, Group.Target.Domain)) continue;
			FElementStateProcessorInput Input;
				Input.Target = Group.Target;
				Input.WorldTimeMilliseconds = WorldTimeMilliseconds;
				Input.PreviousSettlementTimeMilliseconds = Current->LastSettlementMilliseconds;
				Input.TargetRevision = Current->Revision;
				Input.TargetMetadata = Target->Snapshot.Metadata;
			for (const FName Channel : Descriptor.ReadNumericChannels)
			{
				Input.NumericValues.Add({Channel, Write.NumericValues.FindRef(Channel)});
			}
			if (const FElementStateValue* Existing = Current->StateValues.Find(Descriptor.OwnedStateChannel))
			{
				Input.CurrentState = *Existing;
			}
			FElementStateProcessorOutput Output;
			if (StateProcessor->Execute(Input, Output) && Output.Target == Group.Target)
			{
				Write.StateOutputs.Add(MoveTemp(Output));
			}
		}
	});
	Data->Stats.ReducedOffsetCount += Groups.Num();
	for (const FPendingTargetWrite& Write : Ready.Targets)
	{
		Data->Stats.StateProcessorInvocationCount += Write.StateOutputs.Num();
	}
	Data->Ready = MoveTemp(Ready);
	Data->Stats.BvhCandidateCount = Data->QueryStats.CandidateCount;
	Data->RefreshStatCounts();
	return true;
}

bool FElementAuthorityExecution::CommitAuthorityBarrier(const int64 WorldTimeMilliseconds)
{
	check(IsInGameThread());
	if (!Data || WorldTimeMilliseconds < 0 || !Data->Ready.IsSet()) return false;
	FReadyPage Ready = MoveTemp(Data->Ready.GetValue());
	Data->Ready.Reset();
	if (Ready.CycleToken == 0 || Ready.CycleToken == Data->LastCommittedCycleToken)
	{
		Data->Stats.StaleResultDropCount += Ready.Targets.Num();
		return false;
	}
	for (FPendingTargetWrite& Write : Ready.Targets)
	{
		const FTargetRecord* Target = Data->Targets.Find(Write.Target);
		FTargetState* State = Data->TargetStates.Find(Write.Target);
		bool bSourceStale = false;
		for (const FPendingTargetWrite::FSourceDependency& Dependency : Write.SourceDependencies)
		{
			if (!Data->Registry.IsAlive(Dependency.Source)
				|| Data->Registry.GetEntityRevision(Dependency.Source) != Dependency.ExpectedRevision)
			{
				bSourceStale = true;
				break;
			}
		}
		if (!Target || !State || bSourceStale
			|| Target->Snapshot.Revision != Write.ExpectedSnapshotRevision
			|| State->Revision != Write.ExpectedStateRevision)
		{
			++Data->Stats.StaleResultDropCount;
			continue;
		}
		State->NumericValues = MoveTemp(Write.NumericValues);
		State->LastSettlementMilliseconds = Write.SettlementMilliseconds;
		State->Revision = AdvanceNonZero(State->Revision);
		for (FElementStateProcessorOutput& Output : Write.StateOutputs)
		{
			FElementStateProcessor* const* Processor = Data->StateById.Find(
				Output.NextState.SchemaId);
			FName StateChannel = Output.NextState.SchemaId;
			if (!Processor)
			{
				for (const TPair<FName, FElementStateProcessor*>& Pair : Data->StateByChannel)
				{
					if (Output.NextState.SchemaId == Pair.Key)
					{
						Processor = &Pair.Value;
						StateChannel = Pair.Key;
						break;
					}
				}
			}
			if (!Processor || !Output.NextState.IsValid()) continue;
			State->StateValues.Add(StateChannel, Output.NextState);
			Data->ScheduleWake(
				{Write.Target, (*Processor)->GetDescriptor().ProcessorId},
				Target->Snapshot.Revision,
				Output.NextWakeTimeMilliseconds);
			for (FElementProjectionCommand& Command : Output.ProjectionCommands)
			{
				if (Command.IsValid()) Data->CommittedProjectionCommands.Add(MoveTemp(Command));
			}
			for (FElementStructuralCommand& Command : Output.StructuralCommands)
			{
				Data->CommittedStructuralCommands.Add(MoveTemp(Command));
			}
		}
	}
	Data->LastCommittedCycleToken = Ready.CycleToken;
	++Data->Stats.CommitCount;
	Data->RefreshStatCounts();
	return true;
}

bool FElementAuthorityExecution::TryGetNumericValue(
	const FElementTargetKey Target,
	const FName Channel,
	double& OutValue) const
{
	OutValue = 0.0;
	const FTargetState* State = Data ? Data->TargetStates.Find(Target) : nullptr;
	const double* Value = State ? State->NumericValues.Find(Channel) : nullptr;
	if (!Value) return false;
	OutValue = *Value;
	return true;
}

bool FElementAuthorityExecution::TryGetStateValue(
	const FElementTargetKey Target,
	const FName Channel,
	FElementStateValue& OutState) const
{
	OutState = {};
	const FTargetState* State = Data ? Data->TargetStates.Find(Target) : nullptr;
	const FElementStateValue* Value = State ? State->StateValues.Find(Channel) : nullptr;
	if (!Value) return false;
	OutState = *Value;
	return true;
}

bool FElementAuthorityExecution::CaptureTargetState(
	const FElementTargetKey Target,
	FElementAuthorityTargetStateSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	OutSnapshot = {};
	const FTargetRecord* TargetRecord = Data ? Data->Targets.Find(Target) : nullptr;
	const FTargetState* State = Data ? Data->TargetStates.Find(Target) : nullptr;
	if (!TargetRecord || !State) return false;
	OutSnapshot.Target = Target;
	OutSnapshot.StateRevision = State->Revision;
	OutSnapshot.LastSettlementMilliseconds = State->LastSettlementMilliseconds;
	OutSnapshot.NumericValues.Reserve(State->NumericValues.Num());
	for (const TPair<FName, double>& Pair : State->NumericValues)
	{
		OutSnapshot.NumericValues.Add({Pair.Key, Pair.Value});
	}
	OutSnapshot.NumericValues.Sort([](const FElementNumericValue& Left, const FElementNumericValue& Right)
	{
		return Left.Channel.LexicalLess(Right.Channel);
	});
	OutSnapshot.StateValues.Reserve(State->StateValues.Num());
	for (const TPair<FName, FElementStateValue>& Pair : State->StateValues)
	{
		OutSnapshot.StateValues.Add(Pair.Value);
	}
	OutSnapshot.StateValues.Sort([](const FElementStateValue& Left, const FElementStateValue& Right)
	{
		return Left.SchemaId.LexicalLess(Right.SchemaId);
	});
	for (const TPair<FWakeKey, FWakeRecord>& Pair : Data->ActiveWakes)
	{
		if (Pair.Key.Target == Target)
		{
			OutSnapshot.Wakes.Add({Pair.Key.ProcessorId, Pair.Value.DueTimeMilliseconds});
		}
	}
	OutSnapshot.Wakes.Sort([](const FElementPersistentWake& Left, const FElementPersistentWake& Right)
	{
		return Left.ProcessorId.LexicalLess(Right.ProcessorId);
	});
	return OutSnapshot.IsValid();
}

bool FElementAuthorityExecution::RestoreTargetState(
	const FElementAuthorityTargetStateSnapshot& Snapshot,
	FString* OutError)
{
	check(IsInGameThread());
	if (!Data || !Snapshot.IsValid())
	{
		if (OutError) *OutError = TEXT("Element Target State Snapshot 非法。");
		return false;
	}
	const FTargetRecord* Target = Data->Targets.Find(Snapshot.Target);
	if (!Target)
	{
		if (OutError) *OutError = TEXT("Element Dependent Restore 找不到已恢复的 Host Target Snapshot。");
		return false;
	}
	TMap<FName, double> NumericValues;
	for (const FElementNumericValue& Value : Snapshot.NumericValues)
	{
		if (NumericValues.Contains(Value.Channel))
		{
			if (OutError) *OutError = TEXT("Element Target State 含重复 Numeric Channel。");
			return false;
		}
		NumericValues.Add(Value.Channel, Value.Value);
	}
	TMap<FName, FElementStateValue> StateValues;
	for (const FElementStateValue& Value : Snapshot.StateValues)
	{
		if (!Data->StateByChannel.Contains(Value.SchemaId) || StateValues.Contains(Value.SchemaId))
		{
			if (OutError) *OutError = TEXT("Element Target State 含未注册或重复 State Channel。");
			return false;
		}
		StateValues.Add(Value.SchemaId, Value);
	}
	for (const FElementPersistentWake& Wake : Snapshot.Wakes)
	{
		if (!Data->StateById.Contains(Wake.ProcessorId))
		{
			if (OutError) *OutError = TEXT("Element Target State 的 Wake Processor 未注册。");
			return false;
		}
	}
	RemoveTargetState(Snapshot.Target);
	FTargetState& State = Data->TargetStates.FindOrAdd(Snapshot.Target);
	State.Revision = Snapshot.StateRevision;
	State.LastSettlementMilliseconds = Snapshot.LastSettlementMilliseconds;
	State.NumericValues = MoveTemp(NumericValues);
	State.StateValues = MoveTemp(StateValues);
	for (const FElementPersistentWake& Wake : Snapshot.Wakes)
	{
		Data->ScheduleWake(
			{Snapshot.Target, Wake.ProcessorId}, Target->Snapshot.Revision,
			TOptional<int64>(Wake.DueTimeMilliseconds));
	}
	// 当前来源可能与存盘时不同；恢复后只在下一次 Pump 重算该目标，不在 Restore 调用栈递归传播。
	Data->PendingTargetRefresh.Add(Snapshot.Target);
	Data->RefreshStatCounts();
	return true;
}

bool FElementAuthorityExecution::RemoveTargetState(const FElementTargetKey Target)
{
	check(IsInGameThread());
	if (!Data) return false;
	TArray<FWakeKey, TInlineAllocator<4>> WakeKeys;
	for (const TPair<FWakeKey, FWakeRecord>& Pair : Data->ActiveWakes)
	{
		if (Pair.Key.Target == Target) WakeKeys.Add(Pair.Key);
	}
	for (const FWakeKey& Key : WakeKeys) Data->CancelWake(Key);
	Data->PendingTargetRefresh.Remove(Target);
	const bool bRemoved = Data->TargetStates.Remove(Target) > 0;
	Data->RefreshStatCounts();
	return bRemoved;
}

uint64 FElementAuthorityExecution::GetTargetStateRevision(const FElementTargetKey Target) const
{
	const FTargetState* State = Data ? Data->TargetStates.Find(Target) : nullptr;
	return State ? State->Revision : 0;
}

void FElementAuthorityExecution::ConsumeProjectionCommands(TArray<FElementProjectionCommand>& OutCommands)
{
	check(IsInGameThread());
	OutCommands = Data ? MoveTemp(Data->CommittedProjectionCommands) : TArray<FElementProjectionCommand>();
	if (Data) Data->CommittedProjectionCommands.Reset();
}

void FElementAuthorityExecution::ConsumeStructuralCommands(TArray<FElementStructuralCommand>& OutCommands)
{
	check(IsInGameThread());
	OutCommands = Data ? MoveTemp(Data->CommittedStructuralCommands) : TArray<FElementStructuralCommand>();
	if (Data) Data->CommittedStructuralCommands.Reset();
}

const FElementAuthorityExecutionStats& FElementAuthorityExecution::GetStats() const
{
	check(Data);
	return Data->Stats;
}

SIZE_T FElementAuthorityExecution::GetAllocatedSize() const
{
	if (!Data) return 0;
	SIZE_T Size = Data->Registry.GetAllocatedSize() + Data->InfluenceBvh.GetAllocatedSize()
		+ Data->TargetBvh.GetAllocatedSize() + Data->Sources.GetAllocatedSize()
		+ Data->SourceBySpatial.GetAllocatedSize() + Data->Targets.GetAllocatedSize()
		+ Data->TargetBySpatial.GetAllocatedSize() + Data->CharacterTargets.GetAllocatedSize()
		+ Data->CharacterIndices.GetAllocatedSize() + Data->TargetStates.GetAllocatedSize()
		+ Data->CriticalMotions.GetAllocatedSize() + Data->NormalMotions.GetAllocatedSize()
		+ Data->BackgroundMotions.GetAllocatedSize() + Data->WakeHeap.GetAllocatedSize()
		+ Data->ActiveWakes.GetAllocatedSize();
	for (const TPair<FElementTargetKey, FTargetState>& Pair : Data->TargetStates)
	{
		Size += Pair.Value.NumericValues.GetAllocatedSize() + Pair.Value.StateValues.GetAllocatedSize();
	}
	return Size;
}
