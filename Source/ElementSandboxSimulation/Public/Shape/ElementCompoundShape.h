#pragma once

#include "CoreMinimal.h"

enum class EElementPrimitiveKind : uint8
{
	Sphere,
	Box,
	Capsule
};

/** 固定标签的简单局部 Shape；没有虚函数、UObject 或实例动态分配。 */
struct ELEMENTSANDBOXSIMULATION_API FElementShape final
{
	static FElementShape MakeSphere(const FVector& Center, double Radius);
	static FElementShape MakeBox(const FVector& Center, const FQuat& Rotation, const FVector& HalfExtents);
	static FElementShape MakeCapsule(
		const FVector& Center,
		const FVector& Axis,
		double Radius,
		double SegmentHalfLength);

	bool IsValid() const;
	FBox CalculateWorldBounds(const FTransform& OwnerTransform) const;

	EElementPrimitiveKind Kind = EElementPrimitiveKind::Sphere;
	FVector Center = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector HalfExtents = FVector::ZeroVector;
	FVector CapsuleAxis = FVector::UpVector;
	double Radius = 0.0;
	double CapsuleSegmentHalfLength = 0.0;
};

/** 一个逻辑元素/目标的复合 Shape。子 Shape 只用于几何，命中结果始终按逻辑实体去重。 */
struct ELEMENTSANDBOXSIMULATION_API FElementCompoundShape final
{
	FTransform WorldTransform = FTransform::Identity;
	TArray<FElementShape, TInlineAllocator<4>> Shapes;

	bool IsValid() const;
	FBox CalculateWorldBounds() const;
};

/** 查询层内部不可变空间快照的 Generation-safe 身份；不持久化。 */
struct ELEMENTSANDBOXSIMULATION_API FElementSpatialSnapshotHandle final
{
	uint32 Index = MAX_uint32;
	uint32 Generation = 0;

	bool IsSet() const { return Index != MAX_uint32 && Generation != 0; }

	friend bool operator==(const FElementSpatialSnapshotHandle& Left, const FElementSpatialSnapshotHandle& Right)
	{
		return Left.Index == Right.Index && Left.Generation == Right.Generation;
	}

	friend uint32 GetTypeHash(const FElementSpatialSnapshotHandle& Handle)
	{
		return HashCombineFast(GetTypeHash(Handle.Index), GetTypeHash(Handle.Generation));
	}
};
