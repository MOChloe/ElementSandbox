#pragma once

#include "CharacterSnapshotHandle.h"
#include "CoreMinimal.h"

struct ELEMENTSANDBOXCHARACTERS_API FCharacterCapsuleSnapshot final
{
	FVector Center = FVector::ZeroVector;
	FVector Axis = FVector::UpVector;
	double Radius = 0.0;
	double HalfHeight = 0.0;

	double GetSegmentHalfLength() const { return FMath::Max(0.0, HalfHeight - Radius); }
	FBox CalculateBounds() const;
	bool IsValid() const;
	bool Equals(const FCharacterCapsuleSnapshot& Other, double Tolerance = 0.01) const;
};

/** Post-Actor 阶段冻结的连续 POD；不含 Actor、ASC、Component 或 ECS Fragment。 */
struct ELEMENTSANDBOXCHARACTERS_API FCharacterQuerySnapshot final
{
	FCharacterSnapshotHandle Handle;
	uint64 Revision = 0;
	int64 EffectiveTimeMilliseconds = 0;
	FTransform WorldTransform = FTransform::Identity;
	FCharacterCapsuleSnapshot Capsule;

	bool IsValid() const
	{
		return Handle.IsSet() && Revision != 0 && EffectiveTimeMilliseconds >= 0
			&& !WorldTransform.ContainsNaN() && Capsule.IsValid();
	}
};

enum class ECharacterQuerySnapshotChangeKind : uint8
{
	Upsert,
	Motion,
	Remove
};

/** 事务批次中的中性变化；Motion 同时携带前后 Snapshot，供元素查询生成路径。 */
struct ELEMENTSANDBOXCHARACTERS_API FCharacterQuerySnapshotChange final
{
	ECharacterQuerySnapshotChangeKind Kind = ECharacterQuerySnapshotChangeKind::Upsert;
	FCharacterSnapshotHandle Handle;
	TOptional<FCharacterQuerySnapshot> Previous;
	TOptional<FCharacterQuerySnapshot> Current;

	bool IsValid() const;
};

struct ELEMENTSANDBOXCHARACTERS_API FCharacterQuerySnapshotBatch final
{
	uint64 Sequence = 0;
	TArray<FCharacterQuerySnapshotChange> Changes;

	bool IsValid() const;
};

struct ELEMENTSANDBOXCHARACTERS_API FCharacterQuerySnapshotStats final
{
	int32 SnapshotCount = 0;
	int32 LastPostActorSampleCount = 0;
	int32 LastPublishedChangeCount = 0;
	uint64 PublishedBatchCount = 0;
	uint64 TotalCapsuleSampleCount = 0;
	uint64 TotalPostActorPassCount = 0;
	SIZE_T SnapshotAllocatedBytes = 0;
};

