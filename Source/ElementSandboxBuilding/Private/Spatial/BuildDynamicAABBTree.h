#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"

/**
 * Chunk 增量区使用的 Dynamic AABB Tree。
 *
 * 叶节点保存精确 Bounds，树节点使用带 Padding 的 Fat Bounds。小范围 Update 若仍被
 * Fat Bounds 包含，只更新叶数据而不重新插入；越界后才按 surface-area heuristic 重插。
 */
class FBuildDynamicAABBTree final
{
public:
	explicit FBuildDynamicAABBTree(double InBoundsPadding = 0.0);
	FBuildDynamicAABBTree(const FBuildDynamicAABBTree&) = delete;
	FBuildDynamicAABBTree& operator=(const FBuildDynamicAABBTree&) = delete;
	FBuildDynamicAABBTree(FBuildDynamicAABBTree&&) = default;
	FBuildDynamicAABBTree& operator=(FBuildDynamicAABBTree&&) = default;

	bool Insert(FBuildEntityHandle Entity, const FBox& Bounds, int32 Cost = 1);
	bool Remove(FBuildEntityHandle Entity);
	/** Cost=INDEX_NONE 保留原叶成本；正值会与 Bounds 一起沿父链增量维护。 */
	bool Update(FBuildEntityHandle Entity, const FBox& Bounds, int32 Cost = INDEX_NONE);
	void Query(const FBox& QueryBounds, TArray<FBuildEntityHandle>& OutEntities) const;
	void QueryRay(
		const FVector& Origin,
		const FVector& UnitDirection,
		double MaxDistance,
		TArray<FBuildEntityHandle>& OutEntities) const;
	void Reset();

	bool Contains(FBuildEntityHandle Entity) const;
	int32 Num() const;
	int32 GetTotalCost() const;
	uint64 GetRevision() const { return Revision; }
	SIZE_T GetAllocatedSize() const
	{
		return Nodes.GetAllocatedSize() + LeafByEntity.GetAllocatedSize();
	}

#if WITH_DEV_AUTOMATION_TESTS
	bool Validate() const;
#endif

private:
	struct FNode
	{
		FBox TreeBounds = FBox(ForceInit);
		FBox ExactBounds = FBox(ForceInit);
		FBuildEntityHandle Entity;
		int32 Parent = INDEX_NONE;
		int32 LeftChild = INDEX_NONE;
		int32 RightChild = INDEX_NONE;
		int32 Height = -1;
		int32 Cost = 0;
		int32 NextFree = INDEX_NONE;

		bool IsLeaf() const
		{
			return Height == 0;
		}
	};

	int32 AllocateNode();
	void FreeNode(int32 NodeIndex);
	void InsertLeaf(int32 LeafIndex);
	void RemoveLeaf(int32 LeafIndex);
	int32 Balance(int32 NodeIndex);
	void RefitFrom(int32 NodeIndex);
	void BumpRevision();
	FBox MakeTreeBounds(const FBox& ExactBounds) const;

#if WITH_DEV_AUTOMATION_TESTS
	bool ValidateNode(int32 NodeIndex, int32 ExpectedParent, int32& OutLeafCount) const;
#endif

	TArray<FNode> Nodes;
	TMap<FBuildEntityHandle, int32> LeafByEntity;
	int32 RootNode = INDEX_NONE;
	int32 FirstFreeNode = INDEX_NONE;
	double BoundsPadding = 0.0;
	uint64 Revision = 1;
};
