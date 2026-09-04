#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Entity/WorldEntityId.h"

#include "BuildShapeTypes.generated.h"

/** Building 自有的 100 m 三维宿主分片；不依赖 Element Simulation。 */
struct ELEMENTSANDBOXBUILDING_API FBuildShapeShardKey final
{
	static constexpr double DefaultSize = 10000.0;

	FIntVector Coordinates = FIntVector::ZeroValue;

	static FBuildShapeShardKey FromWorldLocation(
		const FVector& WorldLocation,
		double ShardSize = DefaultSize);
	FBox CalculateWorldBounds(double ShardSize = DefaultSize) const;

	friend bool operator==(const FBuildShapeShardKey& Left, const FBuildShapeShardKey& Right)
	{
		return Left.Coordinates == Right.Coordinates;
	}

	friend bool operator!=(const FBuildShapeShardKey& Left, const FBuildShapeShardKey& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FBuildShapeShardKey& Left, const FBuildShapeShardKey& Right)
	{
		if (Left.Coordinates.X != Right.Coordinates.X) return Left.Coordinates.X < Right.Coordinates.X;
		if (Left.Coordinates.Y != Right.Coordinates.Y) return Left.Coordinates.Y < Right.Coordinates.Y;
		return Left.Coordinates.Z < Right.Coordinates.Z;
	}

	friend uint32 GetTypeHash(const FBuildShapeShardKey& Key)
	{
		return GetTypeHash(Key.Coordinates);
	}
};

/** MeshBoundsObb 只存在于 Definition；发布到 Journal 前必须解析成其余三种纯值 Shape。 */
UENUM()
enum class EBuildPartShapeKind : uint8
{
	MeshBoundsObb,
	Sphere,
	Capsule,
	Obb
};

/**
 * Building Mesh Part 的中性局部几何。它不表示可燃资格，也不读取 Chaos Body。
 * CapsuleSegmentHalfLength 是两个半球圆心相对 Center 的距离，不包含 Radius。
 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildPartShapeDefinition final
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Building|Shape")
	EBuildPartShapeKind Kind = EBuildPartShapeKind::MeshBoundsObb;

	UPROPERTY(EditDefaultsOnly, Category="Building|Shape")
	FVector Center = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, Category="Building|Shape")
	FQuat Rotation = FQuat::Identity;

	UPROPERTY(EditDefaultsOnly, Category="Building|Shape")
	FVector HalfExtents = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, Category="Building|Shape", meta=(ClampMin="0.0"))
	double Radius = 0.0;

	UPROPERTY(EditDefaultsOnly, Category="Building|Shape")
	FVector CapsuleAxis = FVector::UpVector;

	UPROPERTY(EditDefaultsOnly, Category="Building|Shape", meta=(ClampMin="0.0"))
	double CapsuleSegmentHalfLength = 0.0;

	/** Definition 内容变化时显式推进；0 永远无效。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Shape", meta=(ClampMin="1"))
	uint64 TemplateRevision = 1;

	bool TryResolve(const FBox& MeshLocalBounds, FBuildPartShapeDefinition& OutResolved) const;
	bool IsResolvedValid() const;
	FBox CalculateBroadphaseBounds(const FTransform& WorldTransform) const;
};

/** Building 宿主中的稳定 Part/Shape 身份；Handle 只用于当前 World 的 Generation 校验。 */
struct ELEMENTSANDBOXBUILDING_API FBuildShapeRef final
{
	FWorldEntityId WorldEntityId;
	FBuildEntityHandle Entity;
	int32 PartId = INDEX_NONE;
	uint16 ShapeId = 0;

	bool IsSet() const
	{
		return WorldEntityId.IsSet() && Entity.IsSet() && PartId >= 0;
	}

	friend bool operator==(const FBuildShapeRef& Left, const FBuildShapeRef& Right)
	{
		return Left.WorldEntityId == Right.WorldEntityId
			&& Left.Entity == Right.Entity
			&& Left.PartId == Right.PartId
			&& Left.ShapeId == Right.ShapeId;
	}

	friend uint32 GetTypeHash(const FBuildShapeRef& Ref)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(Ref.WorldEntityId), GetTypeHash(Ref.Entity)),
			HashCombineFast(GetTypeHash(Ref.PartId), GetTypeHash(Ref.ShapeId)));
	}
};

/** Game Thread 冻结、Worker 可复制的 Building Part Shape；不携带 UObject/Chaos/Render 引用。 */
struct ELEMENTSANDBOXBUILDING_API FBuildShapeInstanceSnapshot final
{
	FBuildShapeRef ShapeRef;
	FName DefinitionId = NAME_None;
	FName SurfaceProfileId = NAME_None;
	uint64 TemplateRevision = 0;
	uint64 EntityTransformRevision = 0;
	uint64 PartTransformRevision = 0;
	uint64 TransformRevision = 0;
	uint32 StateRevision = 0;
	FBuildPartShapeDefinition LocalGeometry;
	FTransform WorldTransform = FTransform::Identity;
	FBox WorldBounds = FBox(ForceInit);

	bool IsValid() const;
};

/** 返回一个有限 AABB 覆盖到的全部分片；结果稳定排序且无重复。 */
ELEMENTSANDBOXBUILDING_API bool GetBuildShapeShardsForBounds(
	const FBox& WorldBounds,
	TArray<FBuildShapeShardKey>& OutShards,
	double ShardSize = FBuildShapeShardKey::DefaultSize);
