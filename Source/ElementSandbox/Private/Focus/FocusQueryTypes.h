#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"

#include "FocusQueryTypes.generated.h"

/** 本地玩家本帧的视点；具体 Query 自行决定如何解释它。 */
struct FFocusQueryContext final
{
	FVector ViewOrigin = FVector::ZeroVector;
	FVector ViewDirection = FVector::ForwardVector;

	bool IsValid() const
	{
		return !ViewOrigin.ContainsNaN()
			&& !ViewDirection.ContainsNaN()
			&& !ViewDirection.IsNearlyZero();
	}
};

/**
 * 一个 Query 已经完成自身筛选与遮挡判定后提交的最终命中。
 * Target 是该 Query 与配套 Handler 之间的类型化私有载荷，Focus Host 不解释它。
 */
USTRUCT()
struct FFocusQueryHit final
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	double HitDistance = 0.0;

	UPROPERTY(Transient)
	FVector HitLocation = FVector::ZeroVector;

	UPROPERTY(Transient)
	FInstancedStruct Target;

	/** 直接瞄准优先于附近辅助；辅助评分不冒充射线命中距离。 */
	UPROPERTY(Transient)
	bool bDirectAim = true;

	UPROPERTY(Transient)
	double SelectionScore = 0.0;

	/** 连续交互必须显式声明；门等离散交互保持 false。 */
	UPROPERTY(Transient)
	bool bRepeatableInteract = false;

	bool IsValid() const
	{
		return FMath::IsFinite(HitDistance)
			&& HitDistance >= 0.0
			&& FMath::IsFinite(SelectionScore) && SelectionScore >= 0.0
			&& !HitLocation.ContainsNaN()
			&& Target.IsValid();
	}
};

/** 单个 Query + Handler 注册项的进程内身份；不跨 Focus Host 或存档使用。 */
struct FFocusQueryRegistrationHandle final
{
public:
	FFocusQueryRegistrationHandle() = default;

	bool IsSet() const { return Value != 0; }

	friend bool operator==(
		const FFocusQueryRegistrationHandle& Left,
		const FFocusQueryRegistrationHandle& Right)
	{
		return Left.Value == Right.Value;
	}

	friend bool operator!=(
		const FFocusQueryRegistrationHandle& Left,
		const FFocusQueryRegistrationHandle& Right)
	{
		return !(Left == Right);
	}

private:
	explicit FFocusQueryRegistrationHandle(const uint64 InValue)
		: Value(InValue)
	{
	}

	uint64 Value = 0;

	friend class UFocusHostComponent;
};

DECLARE_DELEGATE_TwoParams(
	FFocusQueryDelegate,
	const FFocusQueryContext&,
	TArray<FFocusQueryHit>&);
