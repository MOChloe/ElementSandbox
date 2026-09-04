#include "Spatial/BuildAABBTree.h"

#include "Algo/Sort.h"

using namespace UE::ElementSandbox::Building::Private;

void FBuildAABBTree::Build(const TConstArrayView<FBuildSpatialEntry> InEntries)
{
	Reset();
	if (InEntries.IsEmpty())
	{
		return;
	}

	Entries.Append(InEntries.GetData(), InEntries.Num());
	Nodes.Reserve(Entries.Num() * 2 - 1);
	RootNode = BuildNode(0, Entries.Num());
}

void FBuildAABBTree::Query(
	const FBox& QueryBounds,
	TArray<FBuildEntityHandle>& OutEntities) const
{
	if (RootNode == INDEX_NONE || !IsValidSpatialBounds(QueryBounds))
	{
		return;
	}

	TArray<int32, TInlineAllocator<64>> PendingNodes;
	PendingNodes.Add(RootNode);

	while (!PendingNodes.IsEmpty())
	{
		const int32 NodeIndex = PendingNodes.Pop(EAllowShrinking::No);
		const FNode& Node = Nodes[NodeIndex];
		if (!Node.Bounds.Intersect(QueryBounds))
		{
			continue;
		}

		if (Node.IsLeaf())
		{
			const int32 EndEntry = Node.FirstEntry + Node.EntryCount;
			for (int32 EntryIndex = Node.FirstEntry; EntryIndex < EndEntry; ++EntryIndex)
			{
				const FBuildSpatialEntry& Entry = Entries[EntryIndex];
				if (Entry.Bounds.Intersect(QueryBounds))
				{
					OutEntities.Add(Entry.Entity);
				}
			}
			continue;
		}

		check(Node.LeftChild != INDEX_NONE && Node.RightChild != INDEX_NONE);
		PendingNodes.Add(Node.LeftChild);
		PendingNodes.Add(Node.RightChild);
	}
}

void FBuildAABBTree::QueryRay(
	const FVector& Origin,
	const FVector& UnitDirection,
	const double MaxDistance,
	TArray<FBuildEntityHandle>& OutEntities) const
{
	if (RootNode == INDEX_NONE)
	{
		return;
	}

	TArray<int32, TInlineAllocator<64>> PendingNodes;
	PendingNodes.Add(RootNode);
	while (!PendingNodes.IsEmpty())
	{
		const int32 NodeIndex = PendingNodes.Pop(EAllowShrinking::No);
		const FNode& Node = Nodes[NodeIndex];
		double NodeDistance = 0.0;
		if (!RaycastBounds(
			Node.Bounds,
			Origin,
			UnitDirection,
			MaxDistance,
			NodeDistance))
		{
			continue;
		}

		if (Node.IsLeaf())
		{
			const int32 EndEntry = Node.FirstEntry + Node.EntryCount;
			for (int32 EntryIndex = Node.FirstEntry; EntryIndex < EndEntry; ++EntryIndex)
			{
				const FBuildSpatialEntry& Entry = Entries[EntryIndex];
				double EntryDistance = 0.0;
				if (RaycastBounds(
					Entry.Bounds,
					Origin,
					UnitDirection,
					MaxDistance,
					EntryDistance))
				{
					OutEntities.Add(Entry.Entity);
				}
			}
			continue;
		}

		check(Node.LeftChild != INDEX_NONE && Node.RightChild != INDEX_NONE);
		PendingNodes.Add(Node.LeftChild);
		PendingNodes.Add(Node.RightChild);
	}
}

void FBuildAABBTree::Reset()
{
	Nodes.Reset();
	Entries.Reset();
	RootNode = INDEX_NONE;
}

int32 FBuildAABBTree::BuildNode(const int32 FirstEntry, const int32 EntryCount)
{
	check(EntryCount > 0);

	const int32 NodeIndex = Nodes.AddDefaulted();
	const FBox NodeBounds = CalculateEntryBounds(FirstEntry, EntryCount);
	if (EntryCount <= MaxEntriesPerLeaf)
	{
		FNode& Leaf = Nodes[NodeIndex];
		Leaf.Bounds = NodeBounds;
		Leaf.FirstEntry = FirstEntry;
		Leaf.EntryCount = EntryCount;
		return NodeIndex;
	}

	FBox CentroidBounds(ForceInit);
	const int32 EndEntry = FirstEntry + EntryCount;
	for (int32 EntryIndex = FirstEntry; EntryIndex < EndEntry; ++EntryIndex)
	{
		CentroidBounds += Entries[EntryIndex].Bounds.GetCenter();
	}

	const FVector CentroidSize = CentroidBounds.GetSize();
	int32 SplitAxis = 0;
	if (CentroidSize.Y > CentroidSize.X)
	{
		SplitAxis = 1;
	}
	if (CentroidSize.Z > CentroidSize[SplitAxis])
	{
		SplitAxis = 2;
	}

	TArrayView<FBuildSpatialEntry> EntryRange(
		Entries.GetData() + FirstEntry,
		EntryCount);
	Algo::Sort(EntryRange, [SplitAxis](const FBuildSpatialEntry& Left, const FBuildSpatialEntry& Right)
	{
		return Left.Bounds.GetCenter()[SplitAxis] < Right.Bounds.GetCenter()[SplitAxis];
	});

	const int32 LeftEntryCount = EntryCount / 2;
	const int32 LeftChild = BuildNode(FirstEntry, LeftEntryCount);
	const int32 RightChild = BuildNode(
		FirstEntry + LeftEntryCount,
		EntryCount - LeftEntryCount);

	FNode& Node = Nodes[NodeIndex];
	Node.Bounds = NodeBounds;
	Node.LeftChild = LeftChild;
	Node.RightChild = RightChild;
	return NodeIndex;
}

FBox FBuildAABBTree::CalculateEntryBounds(
	const int32 FirstEntry,
	const int32 EntryCount) const
{
	check(EntryCount > 0);
	FBox Bounds(ForceInit);
	const int32 EndEntry = FirstEntry + EntryCount;
	for (int32 EntryIndex = FirstEntry; EntryIndex < EndEntry; ++EntryIndex)
	{
		check(IsValidSpatialBounds(Entries[EntryIndex].Bounds));
		Bounds += Entries[EntryIndex].Bounds;
	}
	return Bounds;
}
