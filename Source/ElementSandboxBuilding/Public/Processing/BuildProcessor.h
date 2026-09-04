#pragma once

#include "CoreMinimal.h"

class FBuildProcessorScheduler;
class UBuildingWorldSubsystem;

/** 轻量 Building Processor 单次执行后的调度结果。 */
enum class EBuildProcessorRunResult : uint8
{
	Done,
	RetryNextFrame,
	Failed
};

/**
 * World Scheduler 中 Processor 注册项的稳定身份。
 * SchedulerId 隔离 World，Generation 防止注销后的 Slot 复用误命中。
 */
struct ELEMENTSANDBOXBUILDING_API FBuildProcessorRegistrationHandle final
{
public:
	FBuildProcessorRegistrationHandle() = default;

	bool IsSet() const
	{
		return SchedulerId != 0 && SlotIndex != INDEX_NONE && Generation != 0;
	}

	friend bool operator==(
		const FBuildProcessorRegistrationHandle& Left,
		const FBuildProcessorRegistrationHandle& Right)
	{
		return Left.SchedulerId == Right.SchedulerId
			&& Left.SlotIndex == Right.SlotIndex
			&& Left.Generation == Right.Generation;
	}

	friend bool operator!=(
		const FBuildProcessorRegistrationHandle& Left,
		const FBuildProcessorRegistrationHandle& Right)
	{
		return !(Left == Right);
	}

private:
	FBuildProcessorRegistrationHandle(
		uint32 InSchedulerId,
		int32 InSlotIndex,
		uint32 InGeneration)
		: SchedulerId(InSchedulerId)
		, SlotIndex(InSlotIndex)
		, Generation(InGeneration)
	{
	}

	uint32 SchedulerId = 0;
	int32 SlotIndex = INDEX_NONE;
	uint32 Generation = 0;

	friend FBuildProcessorScheduler;
};

/** Scheduler 传给轻量 Processor 的只读帧上下文。 */
struct ELEMENTSANDBOXBUILDING_API FBuildProcessorContext final
{
	UBuildingWorldSubsystem& BuildingSubsystem;
	double WorldTimeSeconds = 0.0;
	float DeltaSeconds = 0.0f;
};

/** 单个 Processor 的低成本运行观测。 */
struct ELEMENTSANDBOXBUILDING_API FBuildProcessorStats final
{
	uint64 ExecutionCount = 0;
	uint64 FailureCount = 0;
	uint32 ConsecutiveFailureCount = 0;
	EBuildProcessorRunResult LastResult = EBuildProcessorRunResult::Done;
	bool bReady = false;
};

/**
 * Building 轻量 Processor 的最小调度契约。
 *
 * Processor 不自行 Tick；领域入口把工作写入自己的专用队列后调用
 * RequestExecution。Scheduler 只调度已就绪 Processor，不规定工作项形状、
 * Fragment 访问或多线程策略。
 */
class ELEMENTSANDBOXBUILDING_API FBuildProcessor
{
public:
	explicit FBuildProcessor(FName InDebugName);
	virtual ~FBuildProcessor();

	FBuildProcessor(const FBuildProcessor&) = delete;
	FBuildProcessor& operator=(const FBuildProcessor&) = delete;
	FBuildProcessor(FBuildProcessor&&) = delete;
	FBuildProcessor& operator=(FBuildProcessor&&) = delete;

	FName GetDebugName() const
	{
		return DebugName;
	}

protected:
	/** 队列从空变为非空或跨帧工作仍未结束时请求一次执行。 */
	bool RequestExecution();
	bool IsRegistered() const;

	virtual EBuildProcessorRunResult Execute(FBuildProcessorContext& Context) = 0;

private:
	void Attach(
		FBuildProcessorScheduler& InScheduler,
		FBuildProcessorRegistrationHandle InRegistration);
	void Detach();

	FName DebugName;
	FBuildProcessorScheduler* Scheduler = nullptr;
	FBuildProcessorRegistrationHandle Registration;
	friend FBuildProcessorScheduler;
};
