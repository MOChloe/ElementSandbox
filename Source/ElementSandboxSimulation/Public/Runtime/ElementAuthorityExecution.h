#pragma once

#include "CoreMinimal.h"
#include "Entity/ElementEntityRegistry.h"
#include "Processing/ElementProcessor.h"
#include "Runtime/ElementRuntimeTypes.h"
#include "Templates/UniquePtr.h"

enum class EElementTargetRemovalReason : uint8
{
	RuntimeEvict,
	GameplayDestroy
};

struct ELEMENTSANDBOXSIMULATION_API FElementAuthorityExecutionConfig final
{
	int32 OpenPageCapacity = 1024;
	int32 BackgroundWorkBudget = 4096;
	uint32 BackgroundPromotionCycles = 8;
	int32 MaximumPendingMotionCount = 65536;

	bool IsValid() const
	{
		return OpenPageCapacity > 0 && BackgroundWorkBudget > 0 && BackgroundPromotionCycles > 0
			&& MaximumPendingMotionCount >= OpenPageCapacity;
	}
};

struct ELEMENTSANDBOXSIMULATION_API FElementAuthorityExecutionStats final
{
	uint64 AuthorityCycle = 0;
	uint64 WorkerPumpCount = 0;
	uint64 EmptyWorkerPumpCount = 0;
	uint64 SealedInputPageCount = 0;
	uint64 MotionSubmissionCount = 0;
	uint64 MotionSubmissionDeduplicatedCount = 0;
	uint64 MotionSubmissionRejectedCount = 0;
	uint64 CharacterSnapshotVisits = 0;
	uint64 BvhCandidateCount = 0;
	uint64 NarrowPhaseCount = 0;
	uint64 NumericProcessorInvocationCount = 0;
	uint64 OffsetCount = 0;
	uint64 ReducedOffsetCount = 0;
	uint64 StateProcessorInvocationCount = 0;
	uint64 CommitCount = 0;
	uint64 StaleResultDropCount = 0;
	uint64 WakeScheduledCount = 0;
	uint64 WakeReplacedCount = 0;
	uint64 WakeCancelledCount = 0;
	int32 PendingCriticalCount = 0;
	int32 PendingNormalCount = 0;
	int32 PendingBackgroundCount = 0;
	int32 PendingWakeCount = 0;
	int32 InfluenceCount = 0;
	int32 TargetCount = 0;
};

class FElementAuthorityExecutionData;

/**
 * Element Authority 的集中执行核。所有可变对象只在 Game Thread 登记和 Barrier 发布；
 * Worker 只读封闭页/BVH Snapshot，并把纯值结果写到私有输出页。
 */
class ELEMENTSANDBOXSIMULATION_API FElementAuthorityExecution final
{
public:
	explicit FElementAuthorityExecution(const FElementAuthorityExecutionConfig& Config = {});
	~FElementAuthorityExecution();

	FElementAuthorityExecution(const FElementAuthorityExecution&) = delete;
	FElementAuthorityExecution& operator=(const FElementAuthorityExecution&) = delete;

	bool RegisterNumericProcessor(TUniquePtr<FElementNumericProcessor> Processor);
	bool RegisterStateProcessor(TUniquePtr<FElementStateProcessor> Processor);
	bool ValidateProcessorRegistry(FString* OutError = nullptr) const;

	FElementEntityHandle CreateElement(FWorldEntityId PersistentId = {});
	bool DestroyElement(FElementEntityHandle Element);

	template <typename FragmentType>
	bool AddFragment(const FElementEntityHandle Element, const FragmentType& Fragment)
	{
		return Registry().AddFragment<FragmentType>(Element, Fragment);
	}

	template <typename FragmentType, typename EditFunction>
	bool EditFragment(const FElementEntityHandle Element, EditFunction&& Edit)
	{
		return Registry().EditFragment<FragmentType>(Element, Forward<EditFunction>(Edit));
	}

	template <typename FragmentType>
	bool RemoveFragment(const FElementEntityHandle Element)
	{
		return Registry().RemoveFragment<FragmentType>(Element);
	}

	const FElementEntityRegistry& ReadRegistry() const;

	/**
	 * 初始化、加载或非连续位置变化都走替换；不会检查旧位置到新位置的中间路径。
	 * EffectiveTime 只初始化新目标的稳定状态时钟；已有目标的结算时间只能由 Authority Commit 推进。
	 */
	bool ReplaceTargetSnapshot(const FElementTargetSnapshot& Snapshot);
	bool RemoveTargetSnapshot(
		FElementTargetKey Target,
		uint64 ExpectedRevision,
		EElementTargetRemovalReason Reason);
	/**
	 * 宿主 Query Snapshot 的一次批量广播可包含大量 Upsert/Remove；批次内仍逐条校验和修改，
	 * 但 Target BVH 的不可变读快照只在最外层 End 时发布一次。
	 */
	void BeginTargetSnapshotBatch();
	void EndTargetSnapshotBatch();

	/** 可由任意上游频率提交；本函数只追加开放页，不在调用者线程开始查询。 */
	bool SubmitMotion(const FElementMotionSubmission& Submission);

	/**
	 * 封箱并执行一批工作。Critical 可在任意 Pump 进入；Normal 只在 Authority 边界或页满时进入；
	 * Background 受预算限制。存在 Ready 结果等待 Barrier 时不会覆盖它。
	 */
	bool PumpWorkers(int64 WorldTimeMilliseconds, bool bAuthorityCollectionBoundary);

	/** 校验 Generation/Revision/Cycle 后发布；本轮结构命令产生的 Dirty 只会进入下一页。 */
	bool CommitAuthorityBarrier(int64 WorldTimeMilliseconds);

	bool TryGetNumericValue(FElementTargetKey Target, FName Channel, double& OutValue) const;
	bool TryGetStateValue(FElementTargetKey Target, FName Channel, FElementStateValue& OutState) const;
	uint64 GetTargetStateRevision(FElementTargetKey Target) const;
	/** 只捕获稳定数值、状态和绝对唤醒时间；不暴露任何查询缓存或队列实现。 */
	bool CaptureTargetState(
		FElementTargetKey Target,
		FElementAuthorityTargetStateSnapshot& OutSnapshot) const;
	/** Host Primary 已恢复并登记 Target Snapshot 后，Dependent Element 才能调用。 */
	bool RestoreTargetState(const FElementAuthorityTargetStateSnapshot& Snapshot, FString* OutError = nullptr);
	/** RuntimeEvict/回滚释放内存状态，不产生 Enter/Leave 或 Gameplay 清理回调。 */
	bool RemoveTargetState(FElementTargetKey Target);

	void ConsumeProjectionCommands(TArray<FElementProjectionCommand>& OutCommands);
	void ConsumeStructuralCommands(TArray<FElementStructuralCommand>& OutCommands);

	const FElementAuthorityExecutionStats& GetStats() const;
	SIZE_T GetAllocatedSize() const;

private:
	FElementEntityRegistry& Registry();
	TUniquePtr<FElementAuthorityExecutionData> Data;
};
