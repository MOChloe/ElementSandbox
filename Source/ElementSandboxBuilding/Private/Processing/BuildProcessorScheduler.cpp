#include "Processing/BuildProcessorScheduler.h"

#include "BuildingWorldSubsystem.h"
#include "ElementSandboxBuilding.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Templates/Atomic.h"

namespace
{
	uint32 GNextBuildProcessorSchedulerId = 0;
}

FBuildProcessorScheduler::FBuildProcessorScheduler()
	: SchedulerId(AllocateSchedulerId())
{
	check(IsInGameThread());
}

FBuildProcessorScheduler::~FBuildProcessorScheduler()
{
	Reset();
}

FBuildProcessorRegistrationHandle FBuildProcessorScheduler::RegisterProcessor(
	TUniquePtr<FBuildProcessor> Processor)
{
	check(IsInGameThread());
	if (bDraining || !Processor || Processor->Scheduler)
	{
		return {};
	}

	int32 SlotIndex = INDEX_NONE;
	if (FirstFreeSlot != INDEX_NONE)
	{
		SlotIndex = FirstFreeSlot;
		FProcessorSlot& Slot = Slots[SlotIndex];
		FirstFreeSlot = Slot.NextFreeSlot;
		Slot.NextFreeSlot = INDEX_NONE;
	}
	else
	{
		SlotIndex = Slots.AddDefaulted();
	}

	FProcessorSlot& Slot = Slots[SlotIndex];
	check(!Slot.Processor && Slot.Generation != 0 && !Slot.bReadyQueued);
	Slot.Stats = {};
	Slot.bFailureLogged = false;
	Slot.Processor = MoveTemp(Processor);
	const FBuildProcessorRegistrationHandle Registration = MakeHandle(SlotIndex);
	Slot.Processor->Attach(*this, Registration);
	return Registration;
}

bool FBuildProcessorScheduler::UnregisterProcessor(
	const FBuildProcessorRegistrationHandle Registration)
{
	check(IsInGameThread());
	if (bDraining)
	{
		return false;
	}

	FProcessorSlot* Slot = FindSlot(Registration);
	if (!Slot)
	{
		return false;
	}

	Slot->Processor->Detach();
	Slot->Processor.Reset();
	Slot->Stats = {};
	Slot->bReadyQueued = false;
	Slot->bFailureLogged = false;
	AdvanceGeneration(Slot->Generation);
	Slot->NextFreeSlot = FirstFreeSlot;
	FirstFreeSlot = Registration.SlotIndex;
	return true;
}

bool FBuildProcessorScheduler::TryGetProcessorStats(
	const FBuildProcessorRegistrationHandle Registration,
	FBuildProcessorStats& OutStats) const
{
	check(IsInGameThread());
	const FProcessorSlot* Slot = FindSlot(Registration);
	if (!Slot)
	{
		OutStats = {};
		return false;
	}

	OutStats = Slot->Stats;
	OutStats.bReady = Slot->bReadyQueued;
	return true;
}

bool FBuildProcessorScheduler::RequestExecution(FBuildProcessor& Processor)
{
	check(IsInGameThread());
	if (Processor.Scheduler != this)
	{
		return false;
	}
	FProcessorSlot* Slot = FindSlot(Processor.Registration);
	return Slot && Slot->Processor.Get() == &Processor
		&& QueueRegistration(Processor.Registration);
}

void FBuildProcessorScheduler::ExecuteReady(
	UBuildingWorldSubsystem& BuildingSubsystem,
	const double WorldTimeSeconds,
	const float DeltaSeconds)
{
	check(IsInGameThread());
	check(!bDraining);
	if (ReadyQueue.IsEmpty())
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Processor_Scheduler);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, ProcessorScheduler);
	Swap(ReadyQueue, DrainQueue);
	ReadyQueue.Reset();
	bDraining = true;

	// 先解除整批 Ready 位。Drain 期间任何唤醒都会进入下一帧队列，
	// 不会因目标尚未轮到执行而被本批次吞掉。
	for (const FBuildProcessorRegistrationHandle Registration : DrainQueue)
	{
		if (FProcessorSlot* Slot = FindSlot(Registration))
		{
			Slot->bReadyQueued = false;
		}
	}

	FBuildProcessorContext Context{
		BuildingSubsystem,
		WorldTimeSeconds,
		DeltaSeconds};
	for (const FBuildProcessorRegistrationHandle Registration : DrainQueue)
	{
		FProcessorSlot* Slot = FindSlot(Registration);
		if (!Slot)
		{
			continue;
		}

		const EBuildProcessorRunResult Result = Slot->Processor->Execute(Context);
		++Slot->Stats.ExecutionCount;
		Slot->Stats.LastResult = Result;
		if (Result == EBuildProcessorRunResult::Failed)
		{
			++Slot->Stats.FailureCount;
			++Slot->Stats.ConsecutiveFailureCount;
			if (!Slot->bFailureLogged)
			{
				UE_LOG(
					LogElementSandboxBuilding,
					Error,
					TEXT("Building Processor %s failed in World %s; retaining work for next frame."),
					*Slot->Processor->GetDebugName().ToString(),
					*BuildingSubsystem.GetWorldRef().GetName());
				Slot->bFailureLogged = true;
			}
		}
		else
		{
			Slot->Stats.ConsecutiveFailureCount = 0;
			Slot->bFailureLogged = false;
		}

		if (Result != EBuildProcessorRunResult::Done)
		{
			QueueRegistration(Registration);
		}
	}

	bDraining = false;
	DrainQueue.Reset();
}

bool FBuildProcessorScheduler::HasReadyProcessors() const
{
	check(IsInGameThread());
	return !ReadyQueue.IsEmpty();
}

SIZE_T FBuildProcessorScheduler::GetEstimatedAllocatedSize() const
{
	check(IsInGameThread());
	SIZE_T AllocatedSize = Slots.GetAllocatedSize()
		+ ReadyQueue.GetAllocatedSize()
		+ DrainQueue.GetAllocatedSize();
	return AllocatedSize;
}

void FBuildProcessorScheduler::Reset()
{
	check(IsInGameThread());
	check(!bDraining);
	for (FProcessorSlot& Slot : Slots)
	{
		if (Slot.Processor)
		{
			Slot.Processor->Detach();
			Slot.Processor.Reset();
		}
	}
	Slots.Reset();
	ReadyQueue.Reset();
	DrainQueue.Reset();
	FirstFreeSlot = INDEX_NONE;
	SchedulerId = AllocateSchedulerId();
}

uint32 FBuildProcessorScheduler::AllocateSchedulerId()
{
	check(IsInGameThread());
	++GNextBuildProcessorSchedulerId;
	if (GNextBuildProcessorSchedulerId == 0)
	{
		++GNextBuildProcessorSchedulerId;
	}
	return GNextBuildProcessorSchedulerId;
}

void FBuildProcessorScheduler::AdvanceGeneration(uint32& Generation)
{
	++Generation;
	if (Generation == 0)
	{
		++Generation;
	}
}

FBuildProcessorScheduler::FProcessorSlot* FBuildProcessorScheduler::FindSlot(
	const FBuildProcessorRegistrationHandle Registration)
{
	return Registration.IsSet()
		&& Registration.SchedulerId == SchedulerId
		&& Slots.IsValidIndex(Registration.SlotIndex)
		&& Slots[Registration.SlotIndex].Processor
		&& Slots[Registration.SlotIndex].Generation == Registration.Generation
		? &Slots[Registration.SlotIndex]
		: nullptr;
}

const FBuildProcessorScheduler::FProcessorSlot* FBuildProcessorScheduler::FindSlot(
	const FBuildProcessorRegistrationHandle Registration) const
{
	return Registration.IsSet()
		&& Registration.SchedulerId == SchedulerId
		&& Slots.IsValidIndex(Registration.SlotIndex)
		&& Slots[Registration.SlotIndex].Processor
		&& Slots[Registration.SlotIndex].Generation == Registration.Generation
		? &Slots[Registration.SlotIndex]
		: nullptr;
}

FBuildProcessorRegistrationHandle FBuildProcessorScheduler::MakeHandle(
	const int32 SlotIndex) const
{
	check(Slots.IsValidIndex(SlotIndex));
	return FBuildProcessorRegistrationHandle(
		SchedulerId,
		SlotIndex,
		Slots[SlotIndex].Generation);
}

bool FBuildProcessorScheduler::QueueRegistration(
	const FBuildProcessorRegistrationHandle Registration)
{
	FProcessorSlot* Slot = FindSlot(Registration);
	if (!Slot)
	{
		return false;
	}
	if (!Slot->bReadyQueued)
	{
		Slot->bReadyQueued = true;
		ReadyQueue.Add(Registration);
	}
	return true;
}
