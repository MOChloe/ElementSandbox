#pragma once

#include "CoreMinimal.h"
#include "Spatial/BuildSpatialEntry.h"

/**
 * Chunk Snapshot 使用的只读 AABB Tree。
 *
 * Build 完成后只包含连续 Node/Entry 数组；Query 不分配长期对象，也不修改树。
 * 构建输入会被复制，因此未来可以在工作线程构建候选 Snapshot，再在所有者线程换代。
 */
class FBuildAABBTree final
{
public:
	FBuildAABBTree() = default;
	FBuildAABBTree(const FBuildAABBTree&) = delete;
	FBuildAABBTree& operator=(const FBuildAABBTree&) = delete;
	FBuildAABBTree(FBuildAABBTree&&) = default;
	FBuildAABBTree& operator=(FBuildAABBTree&&) = default;

	void Build(TConstArrayView<FBuildSpatialEntry> InEntries);
	void Query(const FBox& QueryBounds, TArray<FBuildEntityHandle>& OutEntities) const;
	void QueryRay(
		const FVector& Origin,
		const FVector& UnitDirection,
		double MaxDistance,
		TArray<FBuildEntityHandle>& OutEntities) const;
	void Reset();

	int32 Num() const
	{
		return Entries.Num();
	}

	int32 GetNodeCount() const
	{
		return Nodes.Num();
	}

	SIZE_T GetAllocatedSize() const
	{
		return Nodes.GetAllocatedSize() + Entries.GetAllocatedSize();
	}

private:
	struct FNode
	{
		FBox Bounds = FBox(ForceInit);
		int32 LeftChild = INDEX_NONE;
		int32 RightChild = INDEX_NONE;
		int32 FirstEntry = INDEX_NONE;
		int32 EntryCount = 0;

		bool IsLeaf() const
		{
			return EntryCount > 0;
		}
	};

	int32 BuildNode(int32 FirstEntry, int32 EntryCount);
	FBox CalculateEntryBounds(int32 FirstEntry, int32 EntryCount) const;

	static constexpr int32 MaxEntriesPerLeaf = 4;

	TArray<FNode> Nodes;
	TArray<FBuildSpatialEntry> Entries;
	int32 RootNode = INDEX_NONE;
};
