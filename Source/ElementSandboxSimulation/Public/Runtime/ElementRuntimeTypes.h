#pragma once

#include "CoreMinimal.h"
#include "Entity/ElementEntityHandle.h"
#include "Entity/WorldEntityId.h"
#include "Shape/ElementCompoundShape.h"

enum class EElementTargetDomain : uint8
{
	None = 0,
	Character = 1 << 0,
	Building = 1 << 1,
	WorldObject = 1 << 2
};
ENUM_CLASS_FLAGS(EElementTargetDomain);

/** Simulation 可比较的中性目标身份；Character 没有永久 ID，仍由 RegistryId/Slot/Generation 拒绝复用。 */
struct ELEMENTSANDBOXSIMULATION_API FElementTargetKey final
{
	EElementTargetDomain Domain = EElementTargetDomain::None;
	FWorldEntityId WorldEntityId;
	uint32 RegistryId = 0;
	int32 Slot = INDEX_NONE;
	uint32 Generation = 0;
	int32 PartId = 0;

	bool IsValid() const
	{
		const bool bKnownDomain = Domain == EElementTargetDomain::Character
			|| Domain == EElementTargetDomain::Building
			|| Domain == EElementTargetDomain::WorldObject;
		return bKnownDomain && RegistryId != 0 && Slot != INDEX_NONE && Generation != 0
			&& (Domain == EElementTargetDomain::Character || WorldEntityId.IsSet());
	}

	friend bool operator==(const FElementTargetKey& Left, const FElementTargetKey& Right)
	{
		return Left.Domain == Right.Domain && Left.WorldEntityId == Right.WorldEntityId
			&& Left.RegistryId == Right.RegistryId && Left.Slot == Right.Slot
			&& Left.Generation == Right.Generation && Left.PartId == Right.PartId;
	}

	friend bool operator!=(const FElementTargetKey& Left, const FElementTargetKey& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FElementTargetKey& Left, const FElementTargetKey& Right)
	{
		if (Left.Domain != Right.Domain) return static_cast<uint8>(Left.Domain) < static_cast<uint8>(Right.Domain);
		if (Left.WorldEntityId != Right.WorldEntityId) return Left.WorldEntityId < Right.WorldEntityId;
		if (Left.RegistryId != Right.RegistryId) return Left.RegistryId < Right.RegistryId;
		if (Left.Slot != Right.Slot) return Left.Slot < Right.Slot;
		if (Left.Generation != Right.Generation) return Left.Generation < Right.Generation;
		return Left.PartId < Right.PartId;
	}

	friend uint32 GetTypeHash(const FElementTargetKey& Key)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(static_cast<uint8>(Key.Domain)), GetTypeHash(Key.WorldEntityId)),
			HashCombineFast(
				HashCombineFast(GetTypeHash(Key.RegistryId), GetTypeHash(Key.Slot)),
				HashCombineFast(GetTypeHash(Key.Generation), GetTypeHash(Key.PartId))));
	}
};

enum class EElementQueryPriority : uint8
{
	Critical,
	Normal,
	Background
};

enum class EElementSpatialWeightMode : uint8
{
	Uniform,
	LinearFalloff,
	/** 按独立源几何到 Target 实际表面的距离，在指定距离内做 SmoothStep 衰减。 */
	SurfaceDistanceFalloff
};

/** Processor 之间交换的紧凑纯值；含义只由对应 Fragment/Processor 解释。 */
struct ELEMENTSANDBOXSIMULATION_API FElementValuePayload final
{
	static constexpr int32 MaximumValues = 8;
	TStaticArray<double, MaximumValues> Values = {};
	uint8 Count = 0;

	bool IsValid() const
	{
		if (Count > MaximumValues) return false;
		for (uint8 Index = 0; Index < Count; ++Index)
		{
			if (!FMath::IsFinite(Values[Index])) return false;
		}
		return true;
	}
};

struct ELEMENTSANDBOXSIMULATION_API FElementInfluenceSnapshot final
{
	FElementEntityHandle Source;
	/** 依附宿主可选身份；查询层只用它排除 Source 影响自身，不解释领域语义。 */
	FElementTargetKey HostTarget;
	FName ProcessorId = NAME_None;
	uint64 FragmentRevision = 0;
	/** Broadphase/接触支持体；SurfaceDistanceFalloff 时通常是源几何按作用距离扩张后的 Shape。 */
	FElementCompoundShape Shape;
	/** SurfaceDistanceFalloff 的未扩张源几何；其他权重模式保持为空。 */
	FElementCompoundShape FalloffOriginShape;
	double FalloffDistanceCentimeters = 0.0;
	FElementSpatialSnapshotHandle SpatialHandle;
	FElementValuePayload Payload;

	bool IsValid() const
	{
		const bool bHasNoDistanceFalloff = FalloffOriginShape.Shapes.IsEmpty()
			&& FalloffDistanceCentimeters == 0.0;
		const bool bHasValidDistanceFalloff = FalloffOriginShape.IsValid()
			&& FMath::IsFinite(FalloffDistanceCentimeters)
			&& FalloffDistanceCentimeters > 0.0;
		return Source.IsSet() && !ProcessorId.IsNone() && FragmentRevision != 0
					&& Shape.IsValid() && Payload.IsValid()
					&& (bHasNoDistanceFalloff || bHasValidDistanceFalloff);
	}
};

struct ELEMENTSANDBOXSIMULATION_API FElementTargetSnapshot final
{
	FElementTargetKey Target;
	uint64 Revision = 0;
	int64 EffectiveTimeMilliseconds = 0;
	FElementCompoundShape Shape;
	FElementSpatialSnapshotHandle SpatialHandle;
	/** 宿主 Adapter 冻结的中性分类纯值；Simulation 只透传，不解释具体元素语义。 */
	FElementValuePayload Metadata;

	bool IsValid() const
	{
		return Target.IsValid() && Revision != 0 && EffectiveTimeMilliseconds >= 0
			&& Shape.IsValid() && Metadata.IsValid();
	}
};

struct ELEMENTSANDBOXSIMULATION_API FElementMotionSegment final
{
	FTransform PreviousTransform = FTransform::Identity;
	FTransform CurrentTransform = FTransform::Identity;
	int64 StartTimeMilliseconds = 0;
	int64 EndTimeMilliseconds = 0;

	bool IsValid() const
	{
		return !PreviousTransform.ContainsNaN() && !CurrentTransform.ContainsNaN()
			&& StartTimeMilliseconds >= 0 && EndTimeMilliseconds > StartTimeMilliseconds;
	}
};

/** 上游可低频提交一段或多段权威近似路径；非连续变化改用 ReplaceTargetSnapshot。 */
struct ELEMENTSANDBOXSIMULATION_API FElementMotionSubmission final
{
	FElementTargetKey Target;
	uint64 ExpectedTargetRevision = 0;
	EElementQueryPriority Priority = EElementQueryPriority::Normal;
	TArray<FElementMotionSegment, TInlineAllocator<4>> Segments;

	bool IsValid() const
	{
		if (!Target.IsValid() || ExpectedTargetRevision == 0 || Segments.IsEmpty()) return false;
		int64 ExpectedStart = Segments[0].StartTimeMilliseconds;
		for (const FElementMotionSegment& Segment : Segments)
		{
			if (!Segment.IsValid() || Segment.StartTimeMilliseconds != ExpectedStart) return false;
			ExpectedStart = Segment.EndTimeMilliseconds;
		}
		return true;
	}
};

/** 原始运动段在查询层内已消费并压缩；Processor 不接收距离或全部原始片段。 */
struct ELEMENTSANDBOXSIMULATION_API FElementQueryStatistics final
{
	FElementEntityHandle Source;
	FElementTargetKey Target;
	FName ProcessorId = NAME_None;
	uint64 SourceRevision = 0;
	uint64 TargetRevision = 0;
	int64 WindowStartMilliseconds = 0;
	int64 WindowEndMilliseconds = 0;
	double ContactDurationSeconds = 0.0;
	double IntegratedWeightSeconds = 0.0;
	double MaximumWeight = 0.0;
	double EndWeight = 0.0;
	bool bContinuousMotion = false;
	FElementValuePayload SourcePayload;
	FElementValuePayload TargetMetadata;

	bool IsValid() const
	{
		return Source.IsSet() && Target.IsValid() && !ProcessorId.IsNone()
			&& SourceRevision != 0 && TargetRevision != 0
			&& WindowStartMilliseconds >= 0 && WindowEndMilliseconds >= WindowStartMilliseconds
			&& FMath::IsFinite(ContactDurationSeconds) && ContactDurationSeconds >= 0.0
			&& FMath::IsFinite(IntegratedWeightSeconds) && IntegratedWeightSeconds >= 0.0
			&& FMath::IsFinite(MaximumWeight) && MaximumWeight >= 0.0 && MaximumWeight <= 1.0
			&& FMath::IsFinite(EndWeight) && EndWeight >= 0.0 && EndWeight <= 1.0
				&& SourcePayload.IsValid() && TargetMetadata.IsValid();
	}
};

struct ELEMENTSANDBOXSIMULATION_API FElementOffset final
{
	FElementTargetKey Target;
	FName Channel = NAME_None;
	double Delta = 0.0;

	bool IsValid() const
	{
		return Target.IsValid() && !Channel.IsNone() && FMath::IsFinite(Delta);
	}
};

struct ELEMENTSANDBOXSIMULATION_API FElementNumericValue final
{
	FName Channel = NAME_None;
	double Value = 0.0;
};

struct ELEMENTSANDBOXSIMULATION_API FElementStateValue final
{
	FName SchemaId = NAME_None;
	uint64 Revision = 0;
	FElementValuePayload Payload;

	bool IsValid() const { return !SchemaId.IsNone() && Revision != 0 && Payload.IsValid(); }
};

enum class EElementStructuralCommandKind : uint8
{
	AddInfluenceFragment,
	RemoveInfluenceFragment,
	DestroyElement
};

struct ELEMENTSANDBOXSIMULATION_API FElementStructuralCommand final
{
	EElementStructuralCommandKind Kind = EElementStructuralCommandKind::AddInfluenceFragment;
	FElementTargetKey Target;
	FElementEntityHandle Element;
	FName FragmentType = NAME_None;
	FElementValuePayload Payload;
};

struct ELEMENTSANDBOXSIMULATION_API FElementProjectionCommand final
{
	FElementTargetKey Target;
	FName Channel = NAME_None;
	uint64 Revision = 0;
	FElementValuePayload Payload;

	bool IsValid() const
	{
		return Target.IsValid() && !Channel.IsNone() && Revision != 0 && Payload.IsValid();
	}
};

struct ELEMENTSANDBOXSIMULATION_API FElementProcessorDescriptor final
{
	FName ProcessorId = NAME_None;
	FName FragmentType = NAME_None;
	EElementTargetDomain TargetDomains = EElementTargetDomain::None;
	EElementSpatialWeightMode WeightMode = EElementSpatialWeightMode::Uniform;
	TArray<FName, TInlineAllocator<4>> ReadNumericChannels;
	TArray<FName, TInlineAllocator<4>> WriteNumericChannels;
	/** 每次目标进入批次时先清零，再归并当前来源贡献；用于聚合速率而非持久累计值。 */
	TArray<FName, TInlineAllocator<4>> RecomputedNumericChannels;
	FName OwnedStateChannel = NAME_None;

	bool IsNumericValid() const;
	bool IsStateValid() const;
};

struct ELEMENTSANDBOXSIMULATION_API FElementStateProcessorInput final
{
	FElementTargetKey Target;
	int64 WorldTimeMilliseconds = 0;
	int64 PreviousSettlementTimeMilliseconds = 0;
	uint64 TargetRevision = 0;
	TArray<FElementNumericValue, TInlineAllocator<8>> NumericValues;
	TOptional<FElementStateValue> CurrentState;
	FElementValuePayload TargetMetadata;
};

struct ELEMENTSANDBOXSIMULATION_API FElementStateProcessorOutput final
{
	FElementTargetKey Target;
	FElementStateValue NextState;
	TOptional<int64> NextWakeTimeMilliseconds;
	TArray<FElementStructuralCommand, TInlineAllocator<2>> StructuralCommands;
	TArray<FElementProjectionCommand, TInlineAllocator<2>> ProjectionCommands;
};

/**
 * 可持久化的集中唤醒事实。Token、Heap 位置和 Cycle 等运行期细节不属于稳定状态，
 * 恢复时由 Authority Runtime 重新生成。
 */
struct ELEMENTSANDBOXSIMULATION_API FElementPersistentWake final
{
	FName ProcessorId = NAME_None;
	int64 DueTimeMilliseconds = 0;

	bool IsValid() const { return !ProcessorId.IsNone() && DueTimeMilliseconds >= 0; }
};

/**
 * 单个宿主目标在 Element Authority 中的稳定纯值。TargetKey 只用于当前运行期定位，
 * WorldStorage Payload 另存 Host Key，恢复后用宿主的新 Generation 重新绑定。
 */
struct ELEMENTSANDBOXSIMULATION_API FElementAuthorityTargetStateSnapshot final
{
	FElementTargetKey Target;
	uint64 StateRevision = 0;
	int64 LastSettlementMilliseconds = 0;
	TArray<FElementNumericValue> NumericValues;
	TArray<FElementStateValue> StateValues;
	TArray<FElementPersistentWake> Wakes;

	bool IsValid() const;
};
