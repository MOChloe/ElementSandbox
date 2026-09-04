#include "ElementPresentationWorldSubsystem.h"

#include "Async/Async.h"
#include "Async/ElementVisualBuild.h"
#include "Backends/ElementVisualInstancePool.h"
#include "Observation/ElementViewCoverage.h"
#include "PresentationWorldSubsystem.h"

#include "Containers/Queue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Subsystems/SubsystemCollection.h"
#include "Async/TaskGraphInterfaces.h"
#include "TimerManager.h"
#include "UObject/GCObject.h"

namespace
{
	uint64 AdvanceNonZero(uint64 Value)
	{
		if (++Value == 0)
		{
			++Value;
		}
		return Value;
	}

	uint32 AdvanceNonZero(uint32 Value)
	{
		if (++Value == 0)
		{
			++Value;
		}
		return Value;
	}

	using FDescriptorMap = TMap<FElementVisualKey, FElementVisualDescriptor>;

	TSharedRef<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> MakeSortedDescriptors(
		const FDescriptorMap& Descriptors)
	{
		TSharedRef<FElementVisualDescriptorArray, ESPMode::ThreadSafe> Values =
			MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>();
		Values->Reserve(Descriptors.Num());
		for (const TPair<FElementVisualKey, FElementVisualDescriptor>& Pair : Descriptors)
		{
			Values->Add(Pair.Value);
		}
		Values->Sort([](const FElementVisualDescriptor& Left, const FElementVisualDescriptor& Right)
		{
			return Left.Key < Right.Key;
		});
		return Values;
	}

	TSharedRef<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> MakeTargetDescriptors(
		const FDescriptorMap& Resident,
		const TMap<FName, FElementVisualDefinition>& Definitions)
	{
		TSharedRef<FElementVisualDescriptorArray, ESPMode::ThreadSafe> Values =
			MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>();
		Values->Reserve(Resident.Num());
		for (const TPair<FElementVisualKey, FElementVisualDescriptor>& Pair : Resident)
		{
			if (Definitions.Contains(Pair.Value.VisualDefinitionId))
			{
				Values->Add(Pair.Value);
			}
		}
		Values->Sort([](const FElementVisualDescriptor& Left, const FElementVisualDescriptor& Right)
		{
			return Left.Key < Right.Key;
		});
		return Values;
	}

	bool AreDescriptorArraysEquivalent(
		const FElementVisualDescriptorArray& Left,
		const FElementVisualDescriptorArray& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (!Left[Index].IsEquivalent(Right[Index]))
			{
				return false;
			}
		}
		return true;
	}
}

struct FElementPresentationShardState final
{
	int32 CoverageRefCount = 0;
	uint64 CoverageToken = 0;
	bool bGracePending = false;
	double GraceDeadlineSeconds = 0.0;
	bool bResidentInitialized = false;
	FElementVisualShardSnapshot ResidentSnapshot;
	FDescriptorMap Resident;
	TSharedPtr<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> Target;
	FDescriptorMap Applied;
	uint64 TargetRevision = 0;
	bool bInFlight = false;
	bool bPendingDelta = false;
};

struct FElementVisualBuildCompletionSink final
{
	TQueue<FElementVisualBuildResult, EQueueMode::Mpsc> Completed;
	TAtomic<bool> bAccept{true};
};

class FElementPresentationWorldData final : public FGCObject
{
public:
	explicit FElementPresentationWorldData(UWorld& InWorld)
		: World(&InWorld)
		, CompletionSink(MakeShared<FElementVisualBuildCompletionSink, ESPMode::ThreadSafe>())
	{
	}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		for (TPair<FName, FElementVisualDefinition>& Pair : Definitions)
		{
			Collector.AddReferencedObject(Pair.Value.StaticMesh);
		}
	}

	virtual FString GetReferencerName() const override
	{
		return TEXT("FElementPresentationWorldData");
	}

	UWorld* World = nullptr;
	FElementPresentationConfig Config;
	bool bConfigured = false;
	TWeakObjectPtr<UPresentationWorldSubsystem> ViewSourceSubsystem;
	FDelegateHandle ViewUpdatedDelegateHandle;
	FDelegateHandle ViewRemovedDelegateHandle;
	TMap<FPresentationSourceHandle, FElementPresentationViewState> ViewStates;

	TSharedPtr<IElementVisualSource, ESPMode::ThreadSafe> VisualSource;
	FElementVisualSourceHandle VisualSourceHandle;
	FDelegateHandle VisualChangesDelegateHandle;
	uint32 NextVisualSourceId = 0;
	uint32 VisualSourceGeneration = 1;

	TMap<FElementVisualShardKey, FElementPresentationShardState> Shards;
	TMap<FName, FElementVisualDefinition> Definitions;
	uint64 CatalogGeneration = 1;
	TUniquePtr<FElementVisualInstancePool> Pool;
	TMap<FElementVisualKey, FElementVisualShardKey> AppliedLocations;

	TSharedRef<FElementVisualBuildCompletionSink, ESPMode::ThreadSafe> CompletionSink;
	TArray<FElementVisualBuildRequest> HeldBuilds;
	TArray<FElementVisualApplyCommand> PendingApply;
	int32 PendingApplyHead = 0;
	int32 InFlightBuildCount = 0;
	FTimerHandle GraceTimerHandle;
	TSet<FElementVisualShardKey> DeferredChangedShards;

	bool bSynchronousBuildsForTesting = false;
	bool bHoldBuildsForTesting = false;
	bool bPauseJournalForTesting = false;
	int32 ApplyFailuresRemainingForTesting = 0;
	FElementPresentationStats Stats;
};

namespace
{
	bool NeedsBuild(const FElementPresentationShardState& State)
	{
		static const FElementVisualDescriptorArray Empty;
		const FElementVisualDescriptorArray& Target = State.Target.IsValid() ? *State.Target : Empty;
		const TSharedRef<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> Applied =
			MakeSortedDescriptors(State.Applied);
		return !AreDescriptorArraysEquivalent(Target, *Applied);
	}

	void DispatchBuild(FElementPresentationWorldData& Data, const FElementVisualShardKey Shard)
	{
		FElementPresentationShardState* State = Data.Shards.Find(Shard);
		if (!State || State->bInFlight || !NeedsBuild(*State))
		{
			return;
		}
		if (!State->Target.IsValid())
		{
			State->Target = MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>();
		}
		FElementVisualBuildRequest Request;
		Request.Shard = Shard;
		Request.TargetRevision = State->TargetRevision;
		Request.CoverageToken = State->CoverageToken;
		Request.CatalogGeneration = Data.CatalogGeneration;
		Request.Target = State->Target;
		Request.Applied = MakeSortedDescriptors(State->Applied);
		State->bInFlight = true;
		++Data.InFlightBuildCount;
		++Data.Stats.BuildDispatchCount;

		if (Data.bHoldBuildsForTesting)
		{
			Data.HeldBuilds.Add(MoveTemp(Request));
			return;
		}
		if (Data.bSynchronousBuildsForTesting)
		{
			Data.CompletionSink->Completed.Enqueue(BuildElementVisualDelta(Request));
			return;
		}
		const TSharedRef<FElementVisualBuildCompletionSink, ESPMode::ThreadSafe> Sink = Data.CompletionSink;
		Async(EAsyncExecution::ThreadPool, [Sink, Request = MoveTemp(Request)]() mutable
		{
			FElementVisualBuildResult Result = BuildElementVisualDelta(Request);
			if (Sink->bAccept.Load())
			{
				Sink->Completed.Enqueue(MoveTemp(Result));
			}
		});
	}

	bool SetTarget(
		FElementPresentationWorldData& Data,
		const FElementVisualShardKey Shard,
		FElementPresentationShardState& State,
		TSharedRef<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> NewTarget)
	{
		static const FElementVisualDescriptorArray Empty;
		const FElementVisualDescriptorArray& Existing = State.Target.IsValid() ? *State.Target : Empty;
		if (AreDescriptorArraysEquivalent(Existing, *NewTarget))
		{
			if (!State.Target.IsValid())
			{
				State.Target = NewTarget;
			}
			return false;
		}
		State.Target = NewTarget;
		State.TargetRevision = AdvanceNonZero(State.TargetRevision);
		if (State.bInFlight)
		{
			State.bPendingDelta = true;
			++Data.Stats.BuildCoalescedCount;
		}
		else
		{
			DispatchBuild(Data, Shard);
		}
		return true;
	}

	void RefreshTargetFromResident(
		FElementPresentationWorldData& Data,
		const FElementVisualShardKey Shard,
		FElementPresentationShardState& State)
	{
		SetTarget(Data, Shard, State, MakeTargetDescriptors(State.Resident, Data.Definitions));
	}

	void RefreshResidentSnapshot(FElementPresentationShardState& State, const FElementVisualShardKey Shard)
	{
		State.ResidentSnapshot.Shard = Shard;
		State.ResidentSnapshot.Descriptors = MakeSortedDescriptors(State.Resident);
	}

	bool AcquireShardSnapshot(
		FElementPresentationWorldData& Data,
		const FElementVisualShardKey Shard,
		FElementPresentationShardState& State,
		const bool bLocalRebuild)
	{
		if (!Data.VisualSource.IsValid())
		{
			return false;
		}
		FElementVisualShardSnapshot Snapshot;
		if (!Data.VisualSource->CopyShardSnapshot(Shard, Snapshot))
		{
			++Data.Stats.BuildFailedCount;
			return false;
		}
		++Data.Stats.SnapshotReadCount;
		if (bLocalRebuild)
		{
			++Data.Stats.LocalRebuildCount;
		}
		State.bResidentInitialized = true;
		State.ResidentSnapshot = Snapshot;
		State.Resident.Reset();
		for (const FElementVisualDescriptor& Descriptor : Snapshot.GetDescriptors())
		{
			State.Resident.Add(Descriptor.Key, Descriptor);
		}
		RefreshTargetFromResident(Data, Shard, State);
		return true;
	}

	void ScheduleGrace(
		FElementPresentationWorldData& Data,
		const FElementVisualShardKey Shard,
		FElementPresentationShardState& State)
	{
		(void)Shard;
		if (State.bGracePending)
		{
			return;
		}
		State.bGracePending = true;
		State.GraceDeadlineSeconds = FPlatformTime::Seconds() + Data.Config.GraceSeconds;
		++Data.Stats.GraceScheduledCount;
	}

	void CancelGrace(FElementPresentationShardState& State)
	{
		State.bGracePending = false;
		State.GraceDeadlineSeconds = 0.0;
	}

	void EnterCoverage(FElementPresentationWorldData& Data, const FElementVisualShardKey Shard)
	{
		FElementPresentationShardState& State = Data.Shards.FindOrAdd(Shard);
		const int32 PreviousRefCount = State.CoverageRefCount++;
		if (PreviousRefCount > 0)
		{
			++Data.Stats.CoverageRefCountHitCount;
			return;
		}
		if (State.bGracePending)
		{
			CancelGrace(State);
			return;
		}
		State.CoverageToken = AdvanceNonZero(State.CoverageToken);
		AcquireShardSnapshot(Data, Shard, State, false);
	}

	void LeaveCoverage(FElementPresentationWorldData& Data, const FElementVisualShardKey Shard)
	{
		FElementPresentationShardState* State = Data.Shards.Find(Shard);
		if (!State || State->CoverageRefCount <= 0)
		{
			++Data.Stats.InvalidVisibleRemovalCount;
			return;
		}
		if (--State->CoverageRefCount == 0)
		{
			ScheduleGrace(Data, Shard, *State);
		}
	}

	void ApplyCoverageUpdate(
		FElementPresentationWorldData& Data,
		FElementPresentationViewState& ViewState,
		const FPresentationViewSource& View,
		TSet<FElementVisualShardKey>&& NewCoverage)
	{
		for (const FElementVisualShardKey& Shard : NewCoverage)
		{
			if (ViewState.Coverage.Contains(Shard))
			{
				++Data.Stats.CoverageUnchangedCount;
			}
			else
			{
				++Data.Stats.CoverageAddCount;
				EnterCoverage(Data, Shard);
			}
		}
		for (const FElementVisualShardKey& Shard : ViewState.Coverage)
		{
			if (!NewCoverage.Contains(Shard))
			{
				++Data.Stats.CoverageRemoveCount;
				LeaveCoverage(Data, Shard);
			}
		}
		ViewState.Coverage = MoveTemp(NewCoverage);
		ViewState.Anchor = View;
		ViewState.bHasAnchor = true;
	}

	void ExpireGraceShard(
		FElementPresentationWorldData& Data,
		const FElementVisualShardKey Shard,
		FElementPresentationShardState& State)
	{
		if (!State.bGracePending || State.CoverageRefCount > 0)
		{
			return;
		}
		CancelGrace(State);
		++Data.Stats.GraceExpiredCount;
		State.CoverageToken = AdvanceNonZero(State.CoverageToken);
		SetTarget(Data, Shard, State, MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>());
		if (!State.bInFlight && NeedsBuild(State))
		{
			DispatchBuild(Data, Shard);
		}
	}

	void ChaseLatestIfNeeded(
		FElementPresentationWorldData& Data,
		const FElementVisualShardKey Shard,
		FElementPresentationShardState& State)
	{
		if (State.bPendingDelta)
		{
			State.bPendingDelta = false;
			++Data.Stats.PendingDeltaChaseCount;
		}
		DispatchBuild(Data, Shard);
	}

	void DrainBuildCompletions(FElementPresentationWorldData& Data)
	{
		FElementVisualBuildResult Result;
		while (Data.CompletionSink->Completed.Dequeue(Result))
		{
			++Data.Stats.BuildCompleteCount;
			FElementPresentationShardState* State = Data.Shards.Find(Result.Shard);
			if (Data.InFlightBuildCount > 0)
			{
				--Data.InFlightBuildCount;
			}
			if (!State)
			{
				++Data.Stats.BuildStaleDiscardCount;
				continue;
			}
			State->bInFlight = false;
			if (!Result.bSucceeded)
			{
				++Data.Stats.BuildFailedCount;
				ChaseLatestIfNeeded(Data, Result.Shard, *State);
				continue;
			}
			if (Result.TargetRevision != State->TargetRevision
				|| Result.CoverageToken != State->CoverageToken
				|| Result.CatalogGeneration != Data.CatalogGeneration)
			{
				++Data.Stats.BuildStaleDiscardCount;
				ChaseLatestIfNeeded(Data, Result.Shard, *State);
				continue;
			}
			State->bPendingDelta = false;
			Data.PendingApply.Append(MoveTemp(Result.Commands));
		}
	}

	bool IsCommandCurrent(
		const FElementPresentationWorldData& Data,
		const FElementVisualApplyCommand& Command)
	{
		const FElementPresentationShardState* State = Data.Shards.Find(Command.Shard);
		return State && State->TargetRevision == Command.TargetRevision
			&& State->CoverageToken == Command.CoverageToken
			&& Data.CatalogGeneration == Command.CatalogGeneration;
	}

	void CompactApplyQueue(FElementPresentationWorldData& Data)
	{
		if (Data.PendingApplyHead >= Data.PendingApply.Num())
		{
			Data.PendingApply.Reset();
			Data.PendingApplyHead = 0;
		}
		else if (Data.PendingApplyHead >= 1024)
		{
			Data.PendingApply.RemoveAt(0, Data.PendingApplyHead, EAllowShrinking::No);
			Data.PendingApplyHead = 0;
		}
	}

	void ApplyPendingCommands(FElementPresentationWorldData& Data)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		int32 AppliedThisTick = 0;
		bool bBudgetExhausted = false;
		while (Data.PendingApplyHead < Data.PendingApply.Num())
		{
			if (AppliedThisTick >= Data.Config.MaxApplyCommandsPerTick
				|| (AppliedThisTick > 0
					&& (FPlatformTime::Seconds() - StartSeconds) * 1000.0 >= Data.Config.MaxApplyMilliseconds))
			{
				bBudgetExhausted = true;
				break;
			}
			FElementVisualApplyCommand& Command = Data.PendingApply[Data.PendingApplyHead];
			if (!IsCommandCurrent(Data, Command))
			{
				++Data.PendingApplyHead;
				FElementPresentationShardState* State = Data.Shards.Find(Command.Shard);
				if (State && !State->bInFlight)
				{
					DispatchBuild(Data, Command.Shard);
				}
				continue;
			}
			FElementPresentationShardState& State = Data.Shards.FindChecked(Command.Shard);
			bool bSucceeded = false;
			if (Data.ApplyFailuresRemainingForTesting > 0)
			{
				--Data.ApplyFailuresRemainingForTesting;
			}
			else if (Command.Kind == EElementVisualApplyCommandKind::Upsert)
			{
				const FElementVisualDefinition* Definition = Data.Definitions.Find(
					Command.Descriptor.VisualDefinitionId);
				bSucceeded = Definition && Data.Pool && Data.Pool->Upsert(Command.Descriptor, *Definition);
				if (bSucceeded)
				{
					State.Applied.Add(Command.Key, Command.Descriptor);
					Data.AppliedLocations.Add(Command.Key, Command.Shard);
				}
			}
			else
			{
				bSucceeded = Data.Pool && Data.Pool->Remove(Command.Key);
				if (bSucceeded)
				{
					State.Applied.Remove(Command.Key);
					Data.AppliedLocations.Remove(Command.Key);
				}
				else if (!State.Applied.Contains(Command.Key))
				{
					++Data.Stats.InvalidVisibleRemovalCount;
				}
			}
			if (!bSucceeded)
			{
				++Data.Stats.ApplyFailedCount;
				if (++Command.RetryCount <= 3)
				{
					break;
				}
				++Data.Stats.BuildFailedCount;
				++Data.PendingApplyHead;
				continue;
			}
			++Data.PendingApplyHead;
			++AppliedThisTick;
			++Data.Stats.ApplyCommandCount;
		}
		if (bBudgetExhausted)
		{
			++Data.Stats.ApplyBudgetExhaustedCount;
		}
		CompactApplyQueue(Data);
	}

	void ReleaseUnusedResidentData(FElementPresentationWorldData& Data)
	{
		for (TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data.Shards)
		{
			FElementPresentationShardState& State = Pair.Value;
			if (State.CoverageRefCount == 0 && !State.bGracePending && !State.bInFlight
				&& State.Applied.IsEmpty() && (!State.Target.IsValid() || State.Target->IsEmpty()))
			{
				State.Resident.Reset();
				State.ResidentSnapshot = {};
				State.bResidentInitialized = false;
			}
		}
	}
}

UElementPresentationWorldSubsystem::UElementPresentationWorldSubsystem() = default;
UElementPresentationWorldSubsystem::~UElementPresentationWorldSubsystem() = default;

bool UElementPresentationWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World && World->GetNetMode() != NM_DedicatedServer;
}

void UElementPresentationWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UPresentationWorldSubsystem>();
	UWorld* World = GetWorld();
	if (!World || World->IsNetMode(NM_DedicatedServer))
	{
		return;
	}
	Data = MakePimpl<FElementPresentationWorldData>(*World);
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!Presentation)
	{
		Data.Reset();
		return;
	}
	Data->ViewSourceSubsystem = Presentation;
	Data->ViewUpdatedDelegateHandle = Presentation->OnViewSourceUpdated().AddUObject(
		this, &UElementPresentationWorldSubsystem::HandleViewSourceUpdated);
	Data->ViewRemovedDelegateHandle = Presentation->OnViewSourceRemoved().AddUObject(
		this, &UElementPresentationWorldSubsystem::HandleViewSourceRemoved);
	FPresentationViewSnapshot InitialSnapshot;
	if (Presentation->CopyCurrentViewSnapshot(InitialSnapshot))
	{
		for (const FPresentationViewSource& Source : InitialSnapshot.Sources)
		{
			HandleViewSourceUpdated(Source);
		}
	}
}

void UElementPresentationWorldSubsystem::Deinitialize()
{
	if (Data)
	{
		Data->CompletionSink->bAccept.Store(false);
		if (UPresentationWorldSubsystem* Presentation = Data->ViewSourceSubsystem.Get())
		{
			Presentation->OnViewSourceUpdated().Remove(Data->ViewUpdatedDelegateHandle);
			Presentation->OnViewSourceRemoved().Remove(Data->ViewRemovedDelegateHandle);
		}
		if (Data->VisualSource.IsValid())
		{
			Data->VisualSource->OnChangesAvailable().Remove(Data->VisualChangesDelegateHandle);
		}
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(Data->GraceTimerHandle);
		}
		if (Data->Pool)
		{
			Data->Pool->Reset();
		}
		Data.Reset();
	}
	Super::Deinitialize();
}

bool UElementPresentationWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UElementPresentationWorldSubsystem::Configure(const FElementPresentationConfig& Config)
{
	check(IsInGameThread());
	if (!Data || Data->bConfigured || !Config.IsValid() || Data->VisualSource.IsValid()
		|| !Data->Definitions.IsEmpty() || !Data->Shards.IsEmpty() || Data->InFlightBuildCount != 0
		|| Data->PendingApplyHead < Data->PendingApply.Num())
	{
		return false;
	}
	Data->Config = Config;
	Data->bConfigured = true;
	Data->Pool = MakeUnique<FElementVisualInstancePool>(*Data->World, Config);
	TArray<FPresentationViewSource> PendingViews;
	PendingViews.Reserve(Data->ViewStates.Num());
	for (const TPair<FPresentationSourceHandle, FElementPresentationViewState>& Pair : Data->ViewStates)
	{
		PendingViews.Add(Pair.Value.Latest);
	}
	for (const FPresentationViewSource& PendingView : PendingViews)
	{
		HandleViewSourceUpdated(PendingView);
	}
	return true;
}

bool UElementPresentationWorldSubsystem::IsConfigured() const
{
	return Data && Data->bConfigured;
}

FElementVisualSourceHandle UElementPresentationWorldSubsystem::RegisterVisualSource(
	TSharedRef<IElementVisualSource, ESPMode::ThreadSafe> Source)
{
	check(IsInGameThread());
	if (!Data || !Data->bConfigured || Data->VisualSource.IsValid())
	{
		return {};
	}
	Data->VisualSource = Source;
	Data->NextVisualSourceId = AdvanceNonZero(Data->NextVisualSourceId);
	Data->VisualSourceHandle = FElementVisualSourceHandle(
		Data->NextVisualSourceId, Data->VisualSourceGeneration);
	Data->VisualChangesDelegateHandle = Source->OnChangesAvailable().AddUObject(
		this, &UElementPresentationWorldSubsystem::HandleVisualChangesAvailable);
	for (TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		if (Pair.Value.CoverageRefCount > 0)
		{
			AcquireShardSnapshot(*Data, Pair.Key, Pair.Value, false);
		}
	}
	return Data->VisualSourceHandle;
}

bool UElementPresentationWorldSubsystem::UnregisterVisualSource(const FElementVisualSourceHandle Handle)
{
	check(IsInGameThread());
	if (!Data || !Data->VisualSource.IsValid() || !(Handle == Data->VisualSourceHandle))
	{
		return false;
	}
	Data->VisualSource->OnChangesAvailable().Remove(Data->VisualChangesDelegateHandle);
	Data->VisualSource.Reset();
	Data->VisualSourceHandle = {};
	Data->VisualSourceGeneration = AdvanceNonZero(Data->VisualSourceGeneration);
	for (TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		FElementPresentationShardState& State = Pair.Value;
		State.Resident.Reset();
		State.ResidentSnapshot = {};
		State.bResidentInitialized = false;
		State.CoverageToken = AdvanceNonZero(State.CoverageToken);
		SetTarget(*Data, Pair.Key, State, MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>());
	}
	return true;
}

bool UElementPresentationWorldSubsystem::RegisterVisualDefinition(
	const FElementVisualDefinition& Definition)
{
	check(IsInGameThread());
	if (!Data || !Data->bConfigured || !Definition.IsValid())
	{
		return false;
	}
	if (const FElementVisualDefinition* Existing = Data->Definitions.Find(Definition.DefinitionId))
	{
		if (Existing->StaticMesh == Definition.StaticMesh
			&& Existing->MaterialOverride == Definition.MaterialOverride
			&& Existing->Backend == Definition.Backend
			&& Existing->CustomDataFloatCount == Definition.CustomDataFloatCount)
		{
			return true;
		}
		return false;
	}
	Data->Definitions.Add(Definition.DefinitionId, Definition);
	Data->CatalogGeneration = AdvanceNonZero(Data->CatalogGeneration);
	for (TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		if (Pair.Value.bResidentInitialized)
		{
			RefreshTargetFromResident(*Data, Pair.Key, Pair.Value);
		}
	}
	return true;
}

bool UElementPresentationWorldSubsystem::UnregisterVisualDefinition(const FName DefinitionId)
{
	check(IsInGameThread());
	if (!Data || !Data->bConfigured || DefinitionId.IsNone()
		|| Data->Definitions.Remove(DefinitionId) == 0)
	{
		return false;
	}
	Data->CatalogGeneration = AdvanceNonZero(Data->CatalogGeneration);
	for (TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		if (Pair.Value.bResidentInitialized)
		{
			RefreshTargetFromResident(*Data, Pair.Key, Pair.Value);
		}
	}
	return true;
}

void UElementPresentationWorldSubsystem::HandleViewSourceUpdated(const FPresentationViewSource& View)
{
	check(IsInGameThread());
	if (!Data || !View.SourceHandle.IsSet())
	{
		return;
	}
	++Data->Stats.ViewUpdateCount;
	FElementPresentationViewState* Existing = Data->ViewStates.Find(View.SourceHandle);
	if (!Data->bConfigured)
	{
		if (!Existing)
		{
			Existing = &Data->ViewStates.Add(View.SourceHandle);
		}
		Existing->Latest = View;
		return;
	}
	if (Existing)
	{
		Existing->Latest = View;
		if (Existing->bHasAnchor
			&& !ElementViewCoverage::CrossesInvalidationThreshold(Existing->Anchor, View, Data->Config))
		{
			return;
		}
		++Data->Stats.ThresholdInvalidatedCount;
	}
	else
	{
		Existing = &Data->ViewStates.Add(View.SourceHandle);
		Existing->Latest = View;
	}
	TSet<FElementVisualShardKey> NewCoverage;
	if (!ElementViewCoverage::BuildCoverage(View, Data->Config, NewCoverage))
	{
		++Data->Stats.BuildFailedCount;
		return;
	}
	++Data->Stats.CoverageRecomputeCount;
	ApplyCoverageUpdate(*Data, *Existing, View, MoveTemp(NewCoverage));
	ScheduleNextGraceTimer();
}

void UElementPresentationWorldSubsystem::HandleViewSourceRemoved(const FPresentationSourceHandle Source)
{
	check(IsInGameThread());
	if (!Data)
	{
		return;
	}
	FElementPresentationViewState Removed;
	if (!Data->ViewStates.RemoveAndCopyValue(Source, Removed))
	{
		return;
	}
	for (const FElementVisualShardKey& Shard : Removed.Coverage)
	{
		++Data->Stats.CoverageRemoveCount;
		LeaveCoverage(*Data, Shard);
	}
	ScheduleNextGraceTimer();
}

void UElementPresentationWorldSubsystem::HandleVisualChangesAvailable(
	const TArray<FElementVisualShardKey>& Shards)
{
	check(IsInGameThread());
	if (!Data || !Data->VisualSource.IsValid())
	{
		return;
	}
	if (Data->bPauseJournalForTesting)
	{
		Data->DeferredChangedShards.Append(Shards);
		return;
	}
	for (const FElementVisualShardKey& Shard : Shards)
	{
		FElementPresentationShardState* State = Data->Shards.Find(Shard);
		if (!State || (State->CoverageRefCount == 0 && !State->bGracePending))
		{
			continue;
		}
		if (!State->bResidentInitialized)
		{
			AcquireShardSnapshot(*Data, Shard, *State, false);
			continue;
		}
		TArray<FElementVisualChangeBatch> Batches;
		const EElementVisualJournalReadResult ReadResult = Data->VisualSource->ReadChangesAfter(
			Shard, State->ResidentSnapshot.Cursor, Batches);
		if (ReadResult == EElementVisualJournalReadResult::Gap)
		{
			++Data->Stats.JournalGapCount;
			++Data->Stats.JournalOverflowCount;
			AcquireShardSnapshot(*Data, Shard, *State, true);
			continue;
		}
		if (ReadResult == EElementVisualJournalReadResult::UpToDate)
		{
			++Data->Stats.JournalDuplicateCount;
			continue;
		}
		for (const FElementVisualChangeBatch& Batch : Batches)
		{
			for (const FElementVisualChange& Change : Batch.Changes)
			{
				if (Change.Kind == EElementVisualChangeKind::Upsert)
				{
					State->Resident.Add(Change.Key, Change.Descriptor);
				}
				else
				{
					State->Resident.Remove(Change.Key);
				}
				++Data->Stats.JournalDeltaCount;
			}
			State->ResidentSnapshot.Cursor = Batch.Sequence;
		}
		RefreshResidentSnapshot(*State, Shard);
		RefreshTargetFromResident(*Data, Shard, *State);
	}
}

void UElementPresentationWorldSubsystem::ScheduleNextGraceTimer()
{
	if (!Data || !GetWorld())
	{
		return;
	}
	GetWorld()->GetTimerManager().ClearTimer(Data->GraceTimerHandle);
	double Earliest = TNumericLimits<double>::Max();
	for (const TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		if (Pair.Value.bGracePending)
		{
			Earliest = FMath::Min(Earliest, Pair.Value.GraceDeadlineSeconds);
		}
	}
	if (Earliest == TNumericLimits<double>::Max())
	{
		return;
	}
	const float Delay = static_cast<float>(FMath::Max(0.001, Earliest - FPlatformTime::Seconds()));
	GetWorld()->GetTimerManager().SetTimer(
		Data->GraceTimerHandle,
		FTimerDelegate::CreateUObject(this, &UElementPresentationWorldSubsystem::HandleGraceTimer),
		Delay,
		false);
}

void UElementPresentationWorldSubsystem::HandleGraceTimer()
{
	if (!Data)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	for (TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		if (Pair.Value.bGracePending && Pair.Value.GraceDeadlineSeconds <= Now)
		{
			ExpireGraceShard(*Data, Pair.Key, Pair.Value);
		}
	}
	ScheduleNextGraceTimer();
}

void UElementPresentationWorldSubsystem::Tick(const float DeltaTime)
{
	(void)DeltaTime;
	if (!Data)
	{
		return;
	}
	const bool bHadWork = Data->InFlightBuildCount > 0
		|| !Data->CompletionSink->Completed.IsEmpty()
		|| Data->PendingApplyHead < Data->PendingApply.Num();
	if (!bHadWork)
	{
		++Data->Stats.IdleTickCount;
		return;
	}
	DrainBuildCompletions(*Data);
	ApplyPendingCommands(*Data);
	ReleaseUnusedResidentData(*Data);
}

bool UElementPresentationWorldSubsystem::IsTickable() const
{
	return Data && (Data->InFlightBuildCount > 0
		|| !Data->CompletionSink->Completed.IsEmpty()
		|| Data->PendingApplyHead < Data->PendingApply.Num());
}

TStatId UElementPresentationWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UElementPresentationWorldSubsystem, STATGROUP_Tickables);
}

FElementPresentationStats UElementPresentationWorldSubsystem::GetStats() const
{
	FElementPresentationStats Result;
	if (!Data)
	{
		return Result;
	}
	Result = Data->Stats;
	Result.ViewSourceCount = Data->ViewStates.Num();
	Result.PendingApplyCount = Data->PendingApply.Num() - Data->PendingApplyHead;
	Result.InFlightBuildCount = Data->InFlightBuildCount;
	for (const TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		const FElementPresentationShardState& State = Pair.Value;
		Result.CoveredShardCount += State.CoverageRefCount > 0 ? 1 : 0;
		Result.ResidentVisualCount += State.Resident.Num();
		Result.TargetVisualCount += State.Target.IsValid() ? State.Target->Num() : 0;
		Result.AppliedVisualCount += State.Applied.Num();
		Result.GracePendingShardCount += State.bGracePending ? 1 : 0;
	}
	if (Data->Pool)
	{
		Data->Pool->AppendStats(Result);
	}
	return Result;
}

FElementPresentationDebugSnapshot UElementPresentationWorldSubsystem::CopyDebugSnapshot() const
{
	FElementPresentationDebugSnapshot Result;
	if (!Data)
	{
		return Result;
	}
	Result.PendingApplyCount = Data->PendingApply.Num() - Data->PendingApplyHead;
	Result.bTickable = IsTickable();
	for (const TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		FElementPresentationShardDebug& Debug = Result.Shards.AddDefaulted_GetRef();
		Debug.Shard = Pair.Key;
		Debug.CoverageRefCount = Pair.Value.CoverageRefCount;
		Debug.ResidentCount = Pair.Value.Resident.Num();
		Debug.TargetCount = Pair.Value.Target.IsValid() ? Pair.Value.Target->Num() : 0;
		Debug.AppliedCount = Pair.Value.Applied.Num();
		Debug.ResidentCursor = Pair.Value.ResidentSnapshot.Cursor;
		Debug.TargetRevision = Pair.Value.TargetRevision;
		Debug.bInFlight = Pair.Value.bInFlight;
		Debug.bGracePending = Pair.Value.bGracePending;
	}
	Result.Shards.Sort([](const FElementPresentationShardDebug& Left, const FElementPresentationShardDebug& Right)
	{
		return Left.Shard < Right.Shard;
	});
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
void UElementPresentationWorldSubsystem::SetSynchronousBuildsForTesting(const bool bSynchronous)
{
	if (Data)
	{
		Data->bSynchronousBuildsForTesting = bSynchronous;
	}
}

void UElementPresentationWorldSubsystem::SetBuildDispatchHeldForTesting(const bool bHeld)
{
	if (Data)
	{
		Data->bHoldBuildsForTesting = bHeld;
	}
}

void UElementPresentationWorldSubsystem::ReleaseHeldBuildsForTesting()
{
	if (!Data)
	{
		return;
	}
	Data->bHoldBuildsForTesting = false;
	TArray<FElementVisualBuildRequest> Requests = MoveTemp(Data->HeldBuilds);
	Data->HeldBuilds.Reset();
	for (const FElementVisualBuildRequest& Request : Requests)
	{
		Data->CompletionSink->Completed.Enqueue(BuildElementVisualDelta(Request));
	}
}

void UElementPresentationWorldSubsystem::SetJournalConsumptionPausedForTesting(const bool bPaused)
{
	if (!Data || Data->bPauseJournalForTesting == bPaused)
	{
		return;
	}
	Data->bPauseJournalForTesting = bPaused;
	if (!bPaused && !Data->DeferredChangedShards.IsEmpty())
	{
		TArray<FElementVisualShardKey> Deferred = Data->DeferredChangedShards.Array();
		Deferred.Sort();
		Data->DeferredChangedShards.Reset();
		HandleVisualChangesAvailable(Deferred);
	}
}

void UElementPresentationWorldSubsystem::SetApplyFailureCountForTesting(const int32 FailureCount)
{
	if (Data)
	{
		Data->ApplyFailuresRemainingForTesting = FMath::Max(0, FailureCount);
	}
}

bool UElementPresentationWorldSubsystem::PumpUntilIdleForTesting(const int32 MaxIterations)
{
	for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (!IsTickable())
		{
			return true;
		}
		Tick(0.0f);
		if (Data && Data->InFlightBuildCount > 0 && Data->CompletionSink->Completed.IsEmpty())
		{
			FPlatformProcess::SleepNoStats(0.0005f);
		}
	}
	return !IsTickable();
}

void UElementPresentationWorldSubsystem::ExpireAllGraceForTesting()
{
	if (!Data)
	{
		return;
	}
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(Data->GraceTimerHandle);
	}
	for (TPair<FElementVisualShardKey, FElementPresentationShardState>& Pair : Data->Shards)
	{
		if (Pair.Value.bGracePending)
		{
			ExpireGraceShard(*Data, Pair.Key, Pair.Value);
		}
	}
}

bool UElementPresentationWorldSubsystem::IsVisualAppliedForTesting(const FElementVisualKey& Key) const
{
	return Data && Data->AppliedLocations.Contains(Key) && Data->Pool && Data->Pool->Contains(Key);
}

uint64 UElementPresentationWorldSubsystem::GetAppliedVisualRevisionForTesting(
	const FElementVisualKey& Key) const
{
	if (!Data)
	{
		return 0;
	}
	const FElementVisualShardKey* Shard = Data->AppliedLocations.Find(Key);
	const FElementPresentationShardState* State = Shard ? Data->Shards.Find(*Shard) : nullptr;
	const FElementVisualDescriptor* Descriptor = State ? State->Applied.Find(Key) : nullptr;
	return Descriptor ? Descriptor->Revision : 0;
}

int32 UElementPresentationWorldSubsystem::GetCoverageRefCountForTesting(const FElementVisualShardKey Shard) const
{
	const FElementPresentationShardState* State = Data ? Data->Shards.Find(Shard) : nullptr;
	return State ? State->CoverageRefCount : 0;
}
#endif
