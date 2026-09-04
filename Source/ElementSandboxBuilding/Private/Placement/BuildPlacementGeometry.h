#pragma once

#include "CoreMinimal.h"
#include "Chaos/ImplicitFwd.h"

class UBuildingDefinition;
class UStaticMesh;
class UWorld;

/**
 * 从已 Cook 的 Simple Collision 编译出的只读几何。
 * Shape 自带缩放与元素局部 Transform，调用时只再提供无缩放的世界刚体 Transform。
 */
class FBuildPlacementGeometry final
{
public:
	bool Raycast(
		const FTransform& RigidWorldTransform,
		const FVector& Start,
		const FVector& End,
		FVector& OutLocation,
		FVector& OutNormal,
		double& OutDistance) const;

	bool Overlaps(
		const FTransform& RigidWorldTransform,
		const FBuildPlacementGeometry& Other,
		const FTransform& OtherRigidWorldTransform) const;

	bool OverlapsWorld(
		UWorld& World,
		const FTransform& RigidWorldTransform,
		const FCollisionQueryParams& QueryParams,
		const FCollisionObjectQueryParams& ObjectParams) const;

	int32 GetShapeCount() const { return Shapes.Num(); }
	bool IsValid() const { return !Shapes.IsEmpty(); }

private:
	TArray<Chaos::FImplicitObjectPtr> Shapes;

	friend class FBuildPlacementGeometryCache;
};

/**
 * Game Thread 缓存。键只包含碰撞 Mesh 与最终 Scale；位置和旋转始终作为查询参数，
 * 因而客户端预览与服务器裁决不会创建或移动任何 Physics Scene Body。
 */
class FBuildPlacementGeometryCache final
{
public:
	bool CacheDefinition(
		const UBuildingDefinition& Definition,
		double PenetrationTolerance = 0.5);

	const FBuildPlacementGeometry* FindOrAdd(
		UStaticMesh& Mesh,
		const FVector& Scale);

private:
	struct FKey final
	{
		TWeakObjectPtr<UStaticMesh> Mesh;
		FVector Scale = FVector::OneVector;

		bool operator==(const FKey& Other) const
		{
			return Mesh == Other.Mesh && Scale == Other.Scale;
		}

		friend uint32 GetTypeHash(const FKey& Key)
		{
			return HashCombine(GetTypeHash(Key.Mesh), GetTypeHash(Key.Scale));
		}
	};

	TMap<FKey, TUniquePtr<FBuildPlacementGeometry>> Entries;
};
