#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "MeteorRuntimeTypes.h"

namespace UE::ElementSandbox::Meteor
{
	enum class EMeteorWorkPageState : uint8
	{
		Free,
		Open,
		BackgroundReady,
		UrgentReady,
		Computing,
		Completed
	};

	struct ELEMENTSANDBOXMETEOR_API FMeteorSchedulerStats final
	{
		int32 OpenPages = 0;
		int32 BackgroundPages = 0;
		int32 UrgentPages = 0;
		int32 ComputingPages = 0;
		int32 CompletedPages = 0;
		int32 AllocatedPages = 0;
		int32 StaleTokensDiscarded = 0;
		double MinimumSlackSeconds = TNumericLimits<double>::Max();
	};

	/**
	 * 单 Burst 的中央页面调度器。只有 Pump 线程写页面池、开放页和时间轮；生产者只进 Next Inbox。
	 * 时间轮 Token 通过 Slot Generation + ScheduleRevision 自失效，不扫描后台队列。
	 */
	class ELEMENTSANDBOXMETEOR_API FMeteorPageScheduler final
	{
	public:
		bool Initialize(FMeteorBurstId InBurstId, const FMeteorRuntimeConfig& InConfig, double NowSeconds);
		void Reset();
		bool ReserveIncoming(int32 AdditionalSeeds);
		void CancelIncomingReservation(int32 ReservedSeeds);
		bool EnqueueSeed(const FMeteorDebrisSeed& Seed);
		void Pump(double NowSeconds);

		bool TryAcquireWork(FMeteorPageHandle& OutHandle, FMeteorWorkPage& OutPage);
		bool CompleteWork(FMeteorPageHandle Handle, FMeteorTrajectoryPage&& CompletedPage);
		bool FailWork(FMeteorPageHandle Handle);
		bool ConsumeCompleted(FMeteorPageHandle& OutHandle, FMeteorTrajectoryPage& OutPage);
		bool ReleaseCompleted(FMeteorPageHandle Handle);

		FMeteorSchedulerStats GetStats(double NowSeconds) const;
		int32 GetAllocatedPageCount() const { return Slots.Num() - FreeSlots.Num(); }
		int32 GetPendingSeedCount() const;
		uint64 GetNextTrajectoryPageId() { return NextTrajectoryPageId++; }

	private:
		static constexpr int32 WheelSize = 256;
		static constexpr double WheelQuantumSeconds = 0.01;

		struct FOpenPageKey final
		{
			EMeteorTrajectoryKernel Kernel = EMeteorTrajectoryKernel::BallisticGroundPlane;
			FName RenderArchetypeId = NAME_None;
			int64 DeadlineBucket = 0;
			FIntVector OriginCell = FIntVector::ZeroValue;
			friend bool operator==(const FOpenPageKey&, const FOpenPageKey&) = default;
			friend uint32 GetTypeHash(const FOpenPageKey& Key)
			{
				return HashCombineFast(
					HashCombineFast(static_cast<uint32>(Key.Kernel), GetTypeHash(Key.RenderArchetypeId)),
					HashCombineFast(GetTypeHash(Key.DeadlineBucket), GetTypeHash(Key.OriginCell)));
			}
		};

		struct FDeadlineToken final
		{
			FMeteorPageHandle Handle;
			uint32 ScheduleRevision = 0;
			int64 DeadlineTick = 0;
		};

		struct FPageSlot final
		{
			uint32 Generation = 1;
			uint32 ScheduleRevision = 1;
			EMeteorWorkPageState State = EMeteorWorkPageState::Free;
			bool bDeferredUrgentSeal = false;
			FOpenPageKey OpenKey;
			FMeteorWorkPage WorkPage;
			FMeteorTrajectoryPage CompletedPage;
		};

		FMeteorPageHandle AllocatePage(const FOpenPageKey& Key, const FVector3d& Origin);
		FPageSlot* Resolve(FMeteorPageHandle Handle);
		const FPageSlot* Resolve(FMeteorPageHandle Handle) const;
		bool AppendSeed(const FMeteorDebrisSeed& Seed, double NowSeconds);
		void SealPage(FMeteorPageHandle Handle, bool bUrgent);
		void PromoteToken(const FDeadlineToken& Token);
		void ScheduleToken(const FDeadlineToken& Token);
		void AdvanceWheel(double NowSeconds);
		void CascadeSecondLevel(int64 CurrentTick);
		static int64 ToWheelTick(double Seconds);
		static FOpenPageKey MakeOpenKey(const FMeteorDebrisSeed& Seed);
		static FVector3d MakePageOrigin(const FMeteorDebrisSeed& Seed);

		FMeteorBurstId BurstId;
		FMeteorRuntimeConfig Config;
		mutable FCriticalSection InboxMutex;
		TArray<FMeteorDebrisSeed> CurrentInbox;
		TArray<FMeteorDebrisSeed> NextInbox;
		TArray<TUniquePtr<FPageSlot>> Slots;
		TArray<uint32> FreeSlots;
		TMap<FOpenPageKey, FMeteorPageHandle> OpenPages;
		TArray<FMeteorPageHandle> BackgroundQueue;
		TArray<FMeteorPageHandle> UrgentQueue;
		TArray<FMeteorPageHandle> CompletedQueue;
		// 已到截止时间的开放页要先吃完本轮 Inbox，再作为紧急半页封箱。
		// 否则同一 Pump 中的每条 Lane 都会各自生成一张单 Lane 页面。
		TArray<FMeteorPageHandle> DeferredUrgentSeals;
		int32 BackgroundQueueHead = 0;
		int32 UrgentQueueHead = 0;
		int32 CompletedQueueHead = 0;
		TArray<FDeadlineToken> WheelLevel0[WheelSize];
		TArray<FDeadlineToken> WheelLevel1[WheelSize];
		int64 CurrentWheelTick = 0;
		int32 StaleTokensDiscarded = 0;
		int32 ReservedIncomingSeeds = 0;
		uint64 NextTrajectoryPageId = 1;
		bool bInitialized = false;
	};
}
