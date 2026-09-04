#include "MeteorPageScheduler.h"

#include "Misc/ScopeLock.h"

namespace UE::ElementSandbox::Meteor
{
bool FMeteorPageScheduler::Initialize(
	const FMeteorBurstId InBurstId,
	const FMeteorRuntimeConfig& InConfig,
	const double NowSeconds)
{
	Reset();
	if (!InBurstId.IsSet() || !InConfig.IsValid() || !FMath::IsFinite(NowSeconds))
	{
		return false;
	}
	BurstId = InBurstId;
	Config = InConfig;
	CurrentWheelTick = ToWheelTick(NowSeconds);
	Slots.Reserve(Config.MaximumWorkPages);
	FreeSlots.Reserve(Config.MaximumWorkPages);
	bInitialized = true;
	return true;
}

void FMeteorPageScheduler::Reset()
{
	FScopeLock Lock(&InboxMutex);
	CurrentInbox.Reset();
	NextInbox.Reset();
	Slots.Reset();
	FreeSlots.Reset();
	OpenPages.Reset();
	BackgroundQueue.Reset();
	UrgentQueue.Reset();
	CompletedQueue.Reset();
	DeferredUrgentSeals.Reset();
	BackgroundQueueHead = 0;
	UrgentQueueHead = 0;
	CompletedQueueHead = 0;
	for (int32 Bucket = 0; Bucket < WheelSize; ++Bucket)
	{
		WheelLevel0[Bucket].Reset();
		WheelLevel1[Bucket].Reset();
	}
	BurstId = {};
	CurrentWheelTick = 0;
	StaleTokensDiscarded = 0;
	ReservedIncomingSeeds = 0;
	NextTrajectoryPageId = 1;
	bInitialized = false;
}

bool FMeteorPageScheduler::ReserveIncoming(const int32 AdditionalSeeds)
{
	if (!bInitialized || AdditionalSeeds < 0
		|| AdditionalSeeds > Config.MaximumWorkPages * WorkPageCapacity)
	{
		return false;
	}
	FScopeLock Lock(&InboxMutex);
	int64 ResidentSeeds = static_cast<int64>(CurrentInbox.Num()) + NextInbox.Num()
		+ ReservedIncomingSeeds;
	for (const TUniquePtr<FPageSlot>& Slot : Slots)
	{
		if (Slot && Slot->State != EMeteorWorkPageState::Free)
		{
			ResidentSeeds += Slot->WorkPage.Num();
		}
	}
	const int64 Requested = ResidentSeeds + AdditionalSeeds;
	if (Requested > static_cast<int64>(Config.MaximumWorkPages) * WorkPageCapacity)
	{
		return false;
	}
	ReservedIncomingSeeds += AdditionalSeeds;
	NextInbox.Reserve(NextInbox.Num() + ReservedIncomingSeeds);
	return true;
}

void FMeteorPageScheduler::CancelIncomingReservation(const int32 ReservedSeeds)
{
	if (ReservedSeeds <= 0) return;
	FScopeLock Lock(&InboxMutex);
	ReservedIncomingSeeds = FMath::Max(0, ReservedIncomingSeeds - ReservedSeeds);
}

bool FMeteorPageScheduler::EnqueueSeed(const FMeteorDebrisSeed& Seed)
{
	if (!bInitialized || !Seed.IsValid() || Seed.Key.BurstId != BurstId)
	{
		return false;
	}
	FScopeLock Lock(&InboxMutex);
	if (ReservedIncomingSeeds > 0)
	{
		--ReservedIncomingSeeds;
	}
	else
	{
		int64 ResidentSeeds = static_cast<int64>(CurrentInbox.Num()) + NextInbox.Num();
		for (const TUniquePtr<FPageSlot>& Slot : Slots)
		{
			if (Slot && Slot->State != EMeteorWorkPageState::Free)
			{
				ResidentSeeds += Slot->WorkPage.Num();
			}
		}
		if (ResidentSeeds >= static_cast<int64>(Config.MaximumWorkPages) * WorkPageCapacity)
		{
			return false;
		}
	}
	NextInbox.Add(Seed);
	return true;
}

int32 FMeteorPageScheduler::GetPendingSeedCount() const
{
	FScopeLock Lock(&InboxMutex);
	return CurrentInbox.Num() + NextInbox.Num();
}

void FMeteorPageScheduler::Pump(const double NowSeconds)
{
	if (!bInitialized || !FMath::IsFinite(NowSeconds))
	{
		return;
	}
	{
		FScopeLock Lock(&InboxMutex);
		Swap(CurrentInbox, NextInbox);
	}
	TArray<FMeteorDebrisSeed> RetrySeeds;
	for (const FMeteorDebrisSeed& Seed : CurrentInbox)
	{
		if (!AppendSeed(Seed, NowSeconds))
		{
			RetrySeeds.Add(Seed);
		}
	}
	// 紧急 Lane 仍按 OpenKey 在本轮内聚合；只有 Inbox 全部装页之后才封半页。
	// 这保留了截止时间语义，同时避免紧急路径退化为“一条 Lane 一张网络页”。
	for (const FMeteorPageHandle Handle : DeferredUrgentSeals)
	{
		FPageSlot* Slot = Resolve(Handle);
		if (Slot && Slot->State == EMeteorWorkPageState::Open && Slot->bDeferredUrgentSeal)
		{
			SealPage(Handle, true);
		}
	}
	DeferredUrgentSeals.Reset();
	CurrentInbox.Reset();
	if (!RetrySeeds.IsEmpty())
	{
		FScopeLock Lock(&InboxMutex);
		NextInbox.Append(MoveTemp(RetrySeeds));
	}
	AdvanceWheel(NowSeconds);
}

bool FMeteorPageScheduler::TryAcquireWork(
	FMeteorPageHandle& OutHandle,
	FMeteorWorkPage& OutPage)
{
	OutHandle = {};
	OutPage = {};
	auto TryQueue = [this, &OutHandle, &OutPage](
		TArray<FMeteorPageHandle>& Queue,
		int32& QueueHead)
	{
		while (QueueHead < Queue.Num())
		{
			const FMeteorPageHandle Handle = Queue[QueueHead++];
			FPageSlot* Slot = Resolve(Handle);
			if (!Slot || (Slot->State != EMeteorWorkPageState::UrgentReady
				&& Slot->State != EMeteorWorkPageState::BackgroundReady))
			{
				continue;
			}
			Slot->State = EMeteorWorkPageState::Computing;
			OutHandle = Handle;
			OutPage = Slot->WorkPage;
			if (QueueHead == Queue.Num())
			{
				Queue.Reset();
				QueueHead = 0;
			}
			return true;
		}
		Queue.Reset();
		QueueHead = 0;
		return false;
	};
	return TryQueue(UrgentQueue, UrgentQueueHead)
		|| TryQueue(BackgroundQueue, BackgroundQueueHead);
}

bool FMeteorPageScheduler::CompleteWork(
	const FMeteorPageHandle Handle,
	FMeteorTrajectoryPage&& CompletedPage)
{
	FPageSlot* Slot = Resolve(Handle);
	if (!Slot || Slot->State != EMeteorWorkPageState::Computing
		|| !CompletedPage.IsValid() || CompletedPage.BurstId != BurstId)
	{
		return false;
	}
	Slot->CompletedPage = MoveTemp(CompletedPage);
	Slot->State = EMeteorWorkPageState::Completed;
	CompletedQueue.Add(Handle);
	return true;
}

bool FMeteorPageScheduler::FailWork(const FMeteorPageHandle Handle)
{
	FPageSlot* Slot = Resolve(Handle);
	if (!Slot || Slot->State != EMeteorWorkPageState::Computing)
	{
		return false;
	}
	Slot->State = EMeteorWorkPageState::UrgentReady;
	UrgentQueue.Add(Handle);
	return true;
}

bool FMeteorPageScheduler::ConsumeCompleted(
	FMeteorPageHandle& OutHandle,
	FMeteorTrajectoryPage& OutPage)
{
	OutHandle = {};
	OutPage = {};
	while (CompletedQueueHead < CompletedQueue.Num())
	{
		const FMeteorPageHandle Handle = CompletedQueue[CompletedQueueHead++];
		FPageSlot* Slot = Resolve(Handle);
		if (!Slot || Slot->State != EMeteorWorkPageState::Completed)
		{
			continue;
		}
		OutHandle = Handle;
		OutPage = Slot->CompletedPage;
		if (CompletedQueueHead == CompletedQueue.Num())
		{
			CompletedQueue.Reset();
			CompletedQueueHead = 0;
		}
		return true;
	}
	CompletedQueue.Reset();
	CompletedQueueHead = 0;
	return false;
}

bool FMeteorPageScheduler::ReleaseCompleted(const FMeteorPageHandle Handle)
{
	FPageSlot* Slot = Resolve(Handle);
	if (!Slot || Slot->State != EMeteorWorkPageState::Completed)
	{
		return false;
	}
	Slot->WorkPage = {};
	Slot->CompletedPage = {};
	Slot->bDeferredUrgentSeal = false;
	Slot->State = EMeteorWorkPageState::Free;
	Slot->ScheduleRevision = Slot->ScheduleRevision == MAX_uint32 ? 1 : Slot->ScheduleRevision + 1;
	Slot->Generation = Slot->Generation == MAX_uint32 ? 1 : Slot->Generation + 1;
	FreeSlots.Add(Handle.Slot);
	return true;
}

FMeteorSchedulerStats FMeteorPageScheduler::GetStats(const double NowSeconds) const
{
	FMeteorSchedulerStats Stats;
	Stats.StaleTokensDiscarded = StaleTokensDiscarded;
	for (const TUniquePtr<FPageSlot>& SlotPtr : Slots)
	{
		if (!SlotPtr || SlotPtr->State == EMeteorWorkPageState::Free) continue;
		++Stats.AllocatedPages;
		switch (SlotPtr->State)
		{
		case EMeteorWorkPageState::Open: ++Stats.OpenPages; break;
		case EMeteorWorkPageState::BackgroundReady: ++Stats.BackgroundPages; break;
		case EMeteorWorkPageState::UrgentReady: ++Stats.UrgentPages; break;
		case EMeteorWorkPageState::Computing: ++Stats.ComputingPages; break;
		case EMeteorWorkPageState::Completed: ++Stats.CompletedPages; break;
		default: break;
		}
		Stats.MinimumSlackSeconds = FMath::Min(
			Stats.MinimumSlackSeconds,
			SlotPtr->WorkPage.EarliestDeadlineSeconds - NowSeconds);
	}
	return Stats;
}

FMeteorPageHandle FMeteorPageScheduler::AllocatePage(
	const FOpenPageKey& Key,
	const FVector3d& Origin)
{
	uint32 SlotIndex = MAX_uint32;
	if (!FreeSlots.IsEmpty())
	{
		SlotIndex = FreeSlots.Pop(EAllowShrinking::No);
	}
	else if (Slots.Num() < Config.MaximumWorkPages)
	{
		SlotIndex = static_cast<uint32>(Slots.Add(MakeUnique<FPageSlot>()));
	}
	if (SlotIndex == MAX_uint32)
	{
		return {};
	}
	FPageSlot& Slot = *Slots[SlotIndex];
	const FMeteorPageHandle Handle{SlotIndex, Slot.Generation};
	Slot.State = EMeteorWorkPageState::Open;
	Slot.bDeferredUrgentSeal = false;
	Slot.OpenKey = Key;
	Slot.ScheduleRevision = Slot.ScheduleRevision == MAX_uint32 ? 1 : Slot.ScheduleRevision + 1;
	Slot.WorkPage.Reset(Handle, Key.RenderArchetypeId, Origin);
	OpenPages.Add(Key, Handle);
	return Handle;
}

FMeteorPageScheduler::FPageSlot* FMeteorPageScheduler::Resolve(const FMeteorPageHandle Handle)
{
	return Handle.IsSet() && Slots.IsValidIndex(static_cast<int32>(Handle.Slot))
		&& Slots[Handle.Slot] && Slots[Handle.Slot]->Generation == Handle.Generation
		? Slots[Handle.Slot].Get() : nullptr;
}

const FMeteorPageScheduler::FPageSlot* FMeteorPageScheduler::Resolve(const FMeteorPageHandle Handle) const
{
	return Handle.IsSet() && Slots.IsValidIndex(static_cast<int32>(Handle.Slot))
		&& Slots[Handle.Slot] && Slots[Handle.Slot]->Generation == Handle.Generation
		? Slots[Handle.Slot].Get() : nullptr;
}

bool FMeteorPageScheduler::AppendSeed(const FMeteorDebrisSeed& Seed, const double NowSeconds)
{
	const FOpenPageKey Key = MakeOpenKey(Seed);
	FMeteorPageHandle Handle = OpenPages.FindRef(Key);
	FPageSlot* Slot = Resolve(Handle);
	if (!Slot || Slot->State != EMeteorWorkPageState::Open)
	{
		Handle = AllocatePage(Key, MakePageOrigin(Seed));
		Slot = Resolve(Handle);
	}
	if (!Slot || !Slot->WorkPage.Append(Seed))
	{
		return false;
	}

	++Slot->ScheduleRevision;
	const int64 DeadlineTick = ToWheelTick(Slot->WorkPage.EarliestDeadlineSeconds);
	const bool bDueNow = Slot->WorkPage.EarliestDeadlineSeconds <= NowSeconds
		|| DeadlineTick <= CurrentWheelTick;
	if (Slot->WorkPage.IsFull())
	{
		const bool bUrgent = bDueNow || Slot->WorkPage.EarliestDeadlineSeconds
			<= NowSeconds + Config.NetworkLeadSeconds * 0.25;
		SealPage(Handle, bUrgent);
	}
	else if (bDueNow)
	{
		if (!Slot->bDeferredUrgentSeal)
		{
			Slot->bDeferredUrgentSeal = true;
			DeferredUrgentSeals.Add(Handle);
		}
	}
	else
	{
		ScheduleToken({Handle, Slot->ScheduleRevision, DeadlineTick});
	}
	return true;
}

void FMeteorPageScheduler::SealPage(const FMeteorPageHandle Handle, const bool bUrgent)
{
	FPageSlot* Slot = Resolve(Handle);
	if (!Slot || Slot->State != EMeteorWorkPageState::Open || Slot->WorkPage.Num() == 0)
	{
		return;
	}
	OpenPages.Remove(Slot->OpenKey);
	++Slot->ScheduleRevision;
	Slot->bDeferredUrgentSeal = false;
	Slot->State = bUrgent ? EMeteorWorkPageState::UrgentReady : EMeteorWorkPageState::BackgroundReady;
	(bUrgent ? UrgentQueue : BackgroundQueue).Add(Handle);
	if (!bUrgent)
	{
		ScheduleToken({Handle, Slot->ScheduleRevision,
			ToWheelTick(Slot->WorkPage.EarliestDeadlineSeconds)});
	}
}

void FMeteorPageScheduler::PromoteToken(const FDeadlineToken& Token)
{
	FPageSlot* Slot = Resolve(Token.Handle);
	if (!Slot || Slot->ScheduleRevision != Token.ScheduleRevision)
	{
		++StaleTokensDiscarded;
		return;
	}
	if (Slot->State == EMeteorWorkPageState::Open)
	{
		SealPage(Token.Handle, true);
	}
	else if (Slot->State == EMeteorWorkPageState::BackgroundReady)
	{
		Slot->State = EMeteorWorkPageState::UrgentReady;
		UrgentQueue.Add(Token.Handle);
	}
	else
	{
		++StaleTokensDiscarded;
	}
}

void FMeteorPageScheduler::ScheduleToken(const FDeadlineToken& Token)
{
	// 截止时间可能因量化落在当前 Wheel Tick。当前桶已经在本轮被消费，若仍把
	// Token 塞回该桶，它要等时间轮完整绕一圈才会再次出现，近距离冲击波会因此
	// 卡在原点。当前或过期 Token 必须立刻走同一套版本校验与提急路径。
	if (Token.DeadlineTick <= CurrentWheelTick)
	{
		PromoteToken(Token);
		return;
	}
	const int64 Delta = Token.DeadlineTick - CurrentWheelTick;
	if (Delta < WheelSize)
	{
		WheelLevel0[Token.DeadlineTick & (WheelSize - 1)].Add(Token);
	}
	else
	{
		WheelLevel1[(Token.DeadlineTick >> 8) & (WheelSize - 1)].Add(Token);
	}
}

void FMeteorPageScheduler::AdvanceWheel(const double NowSeconds)
{
	const int64 TargetTick = ToWheelTick(NowSeconds);
	while (CurrentWheelTick < TargetTick)
	{
		++CurrentWheelTick;
		if ((CurrentWheelTick & (WheelSize - 1)) == 0)
		{
			CascadeSecondLevel(CurrentWheelTick);
		}
		TArray<FDeadlineToken> Tokens = MoveTemp(
			WheelLevel0[CurrentWheelTick & (WheelSize - 1)]);
		WheelLevel0[CurrentWheelTick & (WheelSize - 1)].Reset();
		for (const FDeadlineToken& Token : Tokens)
		{
			if (Token.DeadlineTick <= CurrentWheelTick)
			{
				PromoteToken(Token);
			}
			else
			{
				ScheduleToken(Token);
			}
		}
	}
}

void FMeteorPageScheduler::CascadeSecondLevel(const int64 CurrentTick)
{
	const int32 Bucket = (CurrentTick >> 8) & (WheelSize - 1);
	TArray<FDeadlineToken> Tokens = MoveTemp(WheelLevel1[Bucket]);
	WheelLevel1[Bucket].Reset();
	for (const FDeadlineToken& Token : Tokens)
	{
		ScheduleToken(Token);
	}
}

int64 FMeteorPageScheduler::ToWheelTick(const double Seconds)
{
	return FMath::FloorToInt64(Seconds / WheelQuantumSeconds);
}

FMeteorPageScheduler::FOpenPageKey FMeteorPageScheduler::MakeOpenKey(
	const FMeteorDebrisSeed& Seed)
{
	FOpenPageKey Key;
	Key.RenderArchetypeId = Seed.RenderArchetypeId;
	// 小截止桶与小 Origin Cell 的笛卡尔积会把环形波前切成数百个空洞小页。
	// 一秒内的 Lane 共享页面，页头仍保存真实最早 Deadline，所以只是更早计算、不会晚算。
	Key.DeadlineBucket = FMath::FloorToInt64(Seed.LatestComputeStartSeconds);
	Key.OriginCell = FIntVector(
		FMath::FloorToInt(Seed.StartPosition.X / 500000.0),
		FMath::FloorToInt(Seed.StartPosition.Y / 500000.0),
		FMath::FloorToInt(Seed.StartPosition.Z / 500000.0));
	return Key;
}

FVector3d FMeteorPageScheduler::MakePageOrigin(const FMeteorDebrisSeed& Seed)
{
	const FOpenPageKey Key = MakeOpenKey(Seed);
	return FVector3d(Key.OriginCell) * 500000.0;
}
}
