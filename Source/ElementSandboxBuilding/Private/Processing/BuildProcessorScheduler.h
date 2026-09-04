#pragma once

#include "CoreMinimal.h"
#include "Processing/BuildProcessor.h"

class UBuildingWorldSubsystem;

/** 每 World 一份、只按 Ready Queue 执行轻量 Processor 的私有调度器。 */
class FBuildProcessorScheduler final
{
public:
	FBuildProcessorScheduler();
	~FBuildProcessorScheduler();

	FBuildProcessorScheduler(const FBuildProcessorScheduler&) = delete;
	FBuildProcessorScheduler& operator=(const FBuildProcessorScheduler&) = delete;

	FBuildProcessorRegistrationHandle RegisterProcessor(
		TUniquePtr<FBuildProcessor> Processor);
	bool UnregisterProcessor(FBuildProcessorRegistrationHandle Registration);
	bool TryGetProcessorStats(
		FBuildProcessorRegistrationHandle Registration,
		FBuildProcessorStats& OutStats) const;

	bool RequestExecution(FBuildProcessor& Processor);
	void ExecuteReady(
		UBuildingWorldSubsystem& BuildingSubsystem,
		double WorldTimeSeconds,
		float DeltaSeconds);
	bool HasReadyProcessors() const;
	SIZE_T GetEstimatedAllocatedSize() const;
	void Reset();

private:
	struct FProcessorSlot final
	{
		TUniquePtr<FBuildProcessor> Processor;
		FBuildProcessorStats Stats;
		uint32 Generation = 1;
		int32 NextFreeSlot = INDEX_NONE;
		bool bReadyQueued = false;
		bool bFailureLogged = false;
	};

	static uint32 AllocateSchedulerId();
	static void AdvanceGeneration(uint32& Generation);
	FProcessorSlot* FindSlot(FBuildProcessorRegistrationHandle Registration);
	const FProcessorSlot* FindSlot(FBuildProcessorRegistrationHandle Registration) const;
	FBuildProcessorRegistrationHandle MakeHandle(int32 SlotIndex) const;
	bool QueueRegistration(FBuildProcessorRegistrationHandle Registration);

	uint32 SchedulerId = 0;
	TArray<FProcessorSlot> Slots;
	TArray<FBuildProcessorRegistrationHandle> ReadyQueue;
	TArray<FBuildProcessorRegistrationHandle> DrainQueue;
	int32 FirstFreeSlot = INDEX_NONE;
	bool bDraining = false;
};
