#pragma once

#include "CoreMinimal.h"

#include "FireCombustionTypes.generated.h"

/** 火源传播资格；它是玩法白名单，不是碰撞通道。 */
UENUM()
enum class EFirePropagationPolicy : uint8
{
	All,
	CharacterOnly
};

UENUM()
enum class EFireCombustionPhase : uint8
{
	Cold,
	Heating,
	Burning,
	BurnedOut
};

/** 燃烧截止时的目标状态。 */
UENUM()
enum class EFireBurnCompletion : uint8
{
	BurnedOut,
	ReturnToCold
};

/**
 * 可直接持久化的火焰玩法真值。时间均是 WorldStorage 的绝对世界模拟时间。
 * Burning 的起止时间保留亚毫秒精度，避免 Authority Barrier 分片改变点燃时刻。
 */
USTRUCT()
struct ELEMENTSANDBOXCOMBUSTION_API FFireCombustionState final
{
	GENERATED_BODY()

	UPROPERTY()
	EFireCombustionPhase Phase = EFireCombustionPhase::Cold;

	UPROPERTY()
	double HeatDose = 0.0;

	UPROPERTY()
	double BurnStartTimeMilliseconds = 0.0;

	UPROPERTY()
	double BurnEndTimeMilliseconds = 0.0;

	UPROPERTY()
	int64 LastResolvedWorldTimeMilliseconds = 0;

	UPROPERTY()
	uint32 Revision = 1;

	bool IsBurning() const { return Phase == EFireCombustionPhase::Burning; }
};

/** 冻结到 Worker 的单类燃烧规则；数值是无单位 Gameplay 参数。 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXCOMBUSTION_API FFireCombustionProfile final
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.000001"))
	double IgnitionDose = 1.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.0"))
	double HeatDecayPerSecond = 0.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.0"))
	double BurnDurationSeconds = 1.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.000001"))
	double EmittedFireIntensity = 0.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.000001"))
	double EmissionRangeCentimeters = 0.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire")
	EFirePropagationPolicy EmissionPolicy = EFirePropagationPolicy::All;

	UPROPERTY(EditDefaultsOnly, Category="Fire")
	EFireBurnCompletion BurnCompletion = EFireBurnCompletion::BurnedOut;

	/** ReturnToCold 类型在输入达到此强度时，把截止时间刷新到受支持区间末尾之后。 */
	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.0"))
	double BurningSupportIntensity = 0.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.0"))
	double BurningTailSeconds = 0.0;
};

USTRUCT()
struct ELEMENTSANDBOXCOMBUSTION_API FFireIntensitySegment final
{
	GENERATED_BODY()

	UPROPERTY()
	int64 StartTimeMilliseconds = 0;

	UPROPERTY()
	int64 EndTimeMilliseconds = 0;

	UPROPERTY()
	double IncomingFireIntensity = 0.0;
};

USTRUCT()
struct ELEMENTSANDBOXCOMBUSTION_API FFireIntervalInput final
{
	GENERATED_BODY()

	UPROPERTY()
	int64 StartTimeMilliseconds = 0;

	UPROPERTY()
	int64 EndTimeMilliseconds = 0;

	UPROPERTY()
	TArray<FFireIntensitySegment> Segments;
};

UENUM()
enum class EFireCombustionEventType : uint8
{
	StartedHeating,
	Cooled,
	Ignited,
	BurnEnded
};

USTRUCT()
struct ELEMENTSANDBOXCOMBUSTION_API FFireCombustionEvent final
{
	GENERATED_BODY()

	UPROPERTY()
	EFireCombustionEventType Type = EFireCombustionEventType::StartedHeating;

	UPROPERTY()
	double EffectiveTimeMilliseconds = 0.0;
};

USTRUCT()
struct ELEMENTSANDBOXCOMBUSTION_API FFireIntervalResult final
{
	GENERATED_BODY()

	UPROPERTY()
	FFireCombustionState FinalState;

	UPROPERTY()
	TArray<FFireCombustionEvent> Events;

	UPROPERTY()
	bool bSucceeded = false;

	UPROPERTY()
	bool bHasNextInternalDeadline = false;

	UPROPERTY()
	double NextInternalDeadlineMilliseconds = 0.0;

	UPROPERTY()
	int32 AnalyticSegmentCount = 0;
};
