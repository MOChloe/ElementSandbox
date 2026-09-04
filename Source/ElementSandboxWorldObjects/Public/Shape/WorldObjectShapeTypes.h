#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldObjectTypes.h"

#include "WorldObjectShapeTypes.generated.h"

/** WorldObject 自有的 100 m 三维宿主分片；不依赖 Element Simulation。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectShapeShardKey final
{
	static constexpr double DefaultSize = 10000.0;

	FIntVector Coordinates = FIntVector::ZeroValue;

	static FWorldObjectShapeShardKey FromWorldLocation(
		const FVector& WorldLocation,
		double ShardSize = DefaultSize);
	FBox CalculateWorldBounds(double ShardSize = DefaultSize) const;

	friend bool operator==(
		const FWorldObjectShapeShardKey& Left,
		const FWorldObjectShapeShardKey& Right)
	{
		return Left.Coordinates == Right.Coordinates;
	}

	friend bool operator!=(
		const FWorldObjectShapeShardKey& Left,
		const FWorldObjectShapeShardKey& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(
		const FWorldObjectShapeShardKey& Left,
		const FWorldObjectShapeShardKey& Right)
	{
		if (Left.Coordinates.X != Right.Coordinates.X) return Left.Coordinates.X < Right.Coordinates.X;
		if (Left.Coordinates.Y != Right.Coordinates.Y) return Left.Coordinates.Y < Right.Coordinates.Y;
		return Left.Coordinates.Z < Right.Coordinates.Z;
	}

	friend uint32 GetTypeHash(const FWorldObjectShapeShardKey& Key)
	{
		return GetTypeHash(Key.Coordinates);
	}
};

UENUM(BlueprintType)
enum class EWorldObjectShapeKind : uint8
{
	Sphere,
	Capsule,
	Obb
};

/**
 * Definition 或实例显式提供的中性局部几何。它既不是交互包络，也不表示可燃资格。
 * CapsuleSegmentHalfLength 是两个半球圆心相对 Center 的距离，不包含 Radius。
 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectShapeDefinition final
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape")
	EWorldObjectShapeKind Kind = EWorldObjectShapeKind::Obb;

	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape")
	FVector Center = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape")
	FQuat Rotation = FQuat::Identity;

	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape")
	FVector HalfExtents = FVector(25.0);

	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape", meta=(ClampMin="0.0"))
	double Radius = 0.0;

	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape")
	FVector CapsuleAxis = FVector::UpVector;

	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape", meta=(ClampMin="0.0"))
	double CapsuleSegmentHalfLength = 0.0;

	/** Definition 内容变化时显式推进；0 永远无效。 */
	UPROPERTY(EditDefaultsOnly, Category="World Object|Shape", meta=(ClampMin="1"))
	uint64 TemplateRevision = 1;

	bool IsValid() const;
	FBox CalculateBroadphaseBounds(const FTransform& WorldTransform) const;

	static FWorldObjectShapeDefinition MakeObbFromBounds(
		const FBox& LocalBounds,
		uint64 TemplateRevision = 1);
};

/** WorldObject 宿主中的稳定 Shape 身份；Handle 只用于当前 World 的 Generation 校验。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectShapeRef final
{
	FWorldEntityId WorldEntityId;
	FWorldObjectEntityHandle Entity;
	uint16 ShapeId = 0;

	bool IsSet() const
	{
		return WorldEntityId.IsSet() && Entity.IsSet();
	}

	friend bool operator==(const FWorldObjectShapeRef& Left, const FWorldObjectShapeRef& Right)
	{
		return Left.WorldEntityId == Right.WorldEntityId
			&& Left.Entity == Right.Entity
			&& Left.ShapeId == Right.ShapeId;
	}

	friend uint32 GetTypeHash(const FWorldObjectShapeRef& Ref)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(Ref.WorldEntityId), GetTypeHash(Ref.Entity)),
			GetTypeHash(Ref.ShapeId));
	}
};

/** Game Thread 冻结、Worker 可复制的 WorldObject Shape，不携带 UObject/Actor/Chaos/Render 引用。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectShapeInstanceSnapshot final
{
	FWorldObjectShapeRef ShapeRef;
	FName DefinitionId = NAME_None;
	FName SurfaceProfileId = NAME_None;
	EWorldObjectSpatialClass SpatialClass = EWorldObjectSpatialClass::Portable;
	EWorldObjectMotionState MotionState = EWorldObjectMotionState::Dormant;
	uint64 TemplateRevision = 0;
	uint64 TransformRevision = 0;
	uint64 ShapeRevision = 0;
	uint32 StateRevision = 0;
	FWorldObjectShapeDefinition LocalGeometry;
	FTransform WorldTransform = FTransform::Identity;
	FBox InteractionWorldBounds = FBox(ForceInit);
	FBox WorldBounds = FBox(ForceInit);

	bool IsValid() const;
};

/** 返回一个有限 AABB 覆盖到的全部分片；结果稳定排序且无重复。 */
ELEMENTSANDBOXWORLDOBJECTS_API bool GetWorldObjectShapeShardsForBounds(
	const FBox& WorldBounds,
	TArray<FWorldObjectShapeShardKey>& OutShards,
	double ShardSize = FWorldObjectShapeShardKey::DefaultSize);
