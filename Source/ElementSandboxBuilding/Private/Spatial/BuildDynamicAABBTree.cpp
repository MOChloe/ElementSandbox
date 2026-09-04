#include "Spatial/BuildDynamicAABBTree.h"

#include "Spatial/BuildSpatialEntry.h"

using namespace UE::ElementSandbox::Building::Private;

FBuildDynamicAABBTree::FBuildDynamicAABBTree(const double InBoundsPadding)
	: BoundsPadding(FMath::Max(0.0, InBoundsPadding))
{
}

bool FBuildDynamicAABBTree::Insert(
	const FBuildEntityHandle Entity,
	const FBox& Bounds,
	const int32 Cost)
{
	if (!Entity.IsSet() || !IsValidSpatialBounds(Bounds) || Cost <= 0
		|| LeafByEntity.Contains(Entity))
	{
		return false;
	}

	const int32 LeafIndex = AllocateNode();
	FNode& Leaf = Nodes[LeafIndex];
	Leaf.TreeBounds = MakeTreeBounds(Bounds);
	Leaf.ExactBounds = Bounds;
	Leaf.Entity = Entity;
	Leaf.Height = 0;
	Leaf.Cost = Cost;
	LeafByEntity.Add(Entity, LeafIndex);
	InsertLeaf(LeafIndex);
	BumpRevision();
	return true;
}

bool FBuildDynamicAABBTree::Remove(const FBuildEntityHandle Entity)
{
	const int32* LeafIndex = LeafByEntity.Find(Entity);
	if (!LeafIndex)
	{
		return false;
	}

	const int32 RemovedLeaf = *LeafIndex;
	RemoveLeaf(RemovedLeaf);
	LeafByEntity.Remove(Entity);
	FreeNode(RemovedLeaf);
	BumpRevision();
	return true;
}

bool FBuildDynamicAABBTree::Update(
	const FBuildEntityHandle Entity,
	const FBox& Bounds,
	const int32 Cost)
{
	const int32* LeafIndex = LeafByEntity.Find(Entity);
	if (!LeafIndex || !IsValidSpatialBounds(Bounds) || (Cost != INDEX_NONE && Cost <= 0))
	{
		return false;
	}

	FNode& Leaf = Nodes[*LeafIndex];
	const int32 NewCost = Cost == INDEX_NONE ? Leaf.Cost : Cost;
	if (Leaf.ExactBounds == Bounds && Leaf.Cost == NewCost)
	{
		return true;
	}

	Leaf.ExactBounds = Bounds;
	const bool bCostChanged = Leaf.Cost != NewCost;
	Leaf.Cost = NewCost;
	if (Leaf.TreeBounds.IsInsideOrOn(Bounds))
	{
		if (bCostChanged)
		{
			RefitFrom(Leaf.Parent);
		}
		BumpRevision();
		return true;
	}

	const int32 UpdatedLeaf = *LeafIndex;
	RemoveLeaf(UpdatedLeaf);
	Nodes[UpdatedLeaf].TreeBounds = MakeTreeBounds(Bounds);
	InsertLeaf(UpdatedLeaf);
	BumpRevision();
	return true;
}

void FBuildDynamicAABBTree::Query(
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
		if (!Node.TreeBounds.Intersect(QueryBounds))
		{
			continue;
		}

		if (Node.IsLeaf())
		{
			// Fat Bounds 只用于剪枝，公共查询结果仍按精确 Bounds 过滤。
			if (Node.ExactBounds.Intersect(QueryBounds))
			{
				OutEntities.Add(Node.Entity);
			}
			continue;
		}

		PendingNodes.Add(Node.LeftChild);
		PendingNodes.Add(Node.RightChild);
	}
}

void FBuildDynamicAABBTree::QueryRay(
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
			Node.TreeBounds,
			Origin,
			UnitDirection,
			MaxDistance,
			NodeDistance))
		{
			continue;
		}

		if (Node.IsLeaf())
		{
			// Fat Bounds 只用于剪枝，公共查询仍按精确 Bounds 二次过滤。
			double ExactDistance = 0.0;
			if (RaycastBounds(
				Node.ExactBounds,
				Origin,
				UnitDirection,
				MaxDistance,
				ExactDistance))
			{
				OutEntities.Add(Node.Entity);
			}
			continue;
		}

		PendingNodes.Add(Node.LeftChild);
		PendingNodes.Add(Node.RightChild);
	}
}

void FBuildDynamicAABBTree::Reset()
{
	const bool bHadEntries = !LeafByEntity.IsEmpty();
	Nodes.Reset();
	LeafByEntity.Reset();
	RootNode = INDEX_NONE;
	FirstFreeNode = INDEX_NONE;
	if (bHadEntries)
	{
		BumpRevision();
	}
}

bool FBuildDynamicAABBTree::Contains(const FBuildEntityHandle Entity) const
{
	return LeafByEntity.Contains(Entity);
}

int32 FBuildDynamicAABBTree::Num() const
{
	return LeafByEntity.Num();
}

int32 FBuildDynamicAABBTree::GetTotalCost() const
{
	return RootNode != INDEX_NONE && Nodes.IsValidIndex(RootNode)
		? Nodes[RootNode].Cost
		: 0;
}

int32 FBuildDynamicAABBTree::AllocateNode()
{
	int32 NodeIndex = INDEX_NONE;
	if (FirstFreeNode != INDEX_NONE)
	{
		NodeIndex = FirstFreeNode;
		FirstFreeNode = Nodes[NodeIndex].NextFree;
		Nodes[NodeIndex] = FNode();
	}
	else
	{
		NodeIndex = Nodes.AddDefaulted();
	}

	return NodeIndex;
}

void FBuildDynamicAABBTree::FreeNode(const int32 NodeIndex)
{
	check(Nodes.IsValidIndex(NodeIndex));
	FNode& Node = Nodes[NodeIndex];
	Node = FNode();
	Node.NextFree = FirstFreeNode;
	FirstFreeNode = NodeIndex;
}

void FBuildDynamicAABBTree::InsertLeaf(const int32 LeafIndex)
{
	check(Nodes.IsValidIndex(LeafIndex));
	check(Nodes[LeafIndex].IsLeaf());

	if (RootNode == INDEX_NONE)
	{
		RootNode = LeafIndex;
		Nodes[RootNode].Parent = INDEX_NONE;
		return;
	}

	const FBox LeafBounds = Nodes[LeafIndex].TreeBounds;
	int32 SiblingIndex = RootNode;
	while (!Nodes[SiblingIndex].IsLeaf())
	{
		const FNode& SiblingCandidate = Nodes[SiblingIndex];
		const int32 LeftChild = SiblingCandidate.LeftChild;
		const int32 RightChild = SiblingCandidate.RightChild;
		const double CurrentArea = GetBoundsSurfaceArea(SiblingCandidate.TreeBounds);
		const FBox CombinedBounds = MergeBounds(SiblingCandidate.TreeBounds, LeafBounds);
		const double CombinedArea = GetBoundsSurfaceArea(CombinedBounds);
		const double ParentCost = 2.0 * CombinedArea;
		const double InheritanceCost = 2.0 * (CombinedArea - CurrentArea);

		const auto CalculateChildCost = [this, &LeafBounds, InheritanceCost](const int32 ChildIndex)
		{
			const FNode& Child = Nodes[ChildIndex];
			const double MergedArea = GetBoundsSurfaceArea(
				MergeBounds(LeafBounds, Child.TreeBounds));
			return Child.IsLeaf()
				? MergedArea + InheritanceCost
				: MergedArea - GetBoundsSurfaceArea(Child.TreeBounds) + InheritanceCost;
		};

		const double LeftCost = CalculateChildCost(LeftChild);
		const double RightCost = CalculateChildCost(RightChild);
		if (ParentCost < LeftCost && ParentCost < RightCost)
		{
			break;
		}

		SiblingIndex = LeftCost < RightCost ? LeftChild : RightChild;
	}

	const int32 OldParentIndex = Nodes[SiblingIndex].Parent;
	const int32 NewParentIndex = AllocateNode();
	FNode& NewParent = Nodes[NewParentIndex];
	NewParent.Parent = OldParentIndex;
	NewParent.TreeBounds = MergeBounds(LeafBounds, Nodes[SiblingIndex].TreeBounds);
	NewParent.Height = Nodes[SiblingIndex].Height + 1;
	NewParent.Cost = Nodes[SiblingIndex].Cost + Nodes[LeafIndex].Cost;
	NewParent.LeftChild = SiblingIndex;
	NewParent.RightChild = LeafIndex;

	Nodes[SiblingIndex].Parent = NewParentIndex;
	Nodes[LeafIndex].Parent = NewParentIndex;
	if (OldParentIndex == INDEX_NONE)
	{
		RootNode = NewParentIndex;
	}
	else
	{
		FNode& OldParent = Nodes[OldParentIndex];
		if (OldParent.LeftChild == SiblingIndex)
		{
			OldParent.LeftChild = NewParentIndex;
		}
		else
		{
			check(OldParent.RightChild == SiblingIndex);
			OldParent.RightChild = NewParentIndex;
		}
	}

	RefitFrom(NewParentIndex);
}

void FBuildDynamicAABBTree::RemoveLeaf(const int32 LeafIndex)
{
	check(Nodes.IsValidIndex(LeafIndex));
	check(Nodes[LeafIndex].IsLeaf());
	if (LeafIndex == RootNode)
	{
		RootNode = INDEX_NONE;
		Nodes[LeafIndex].Parent = INDEX_NONE;
		return;
	}

	const int32 ParentIndex = Nodes[LeafIndex].Parent;
	const int32 GrandParentIndex = Nodes[ParentIndex].Parent;
	const int32 SiblingIndex = Nodes[ParentIndex].LeftChild == LeafIndex
		? Nodes[ParentIndex].RightChild
		: Nodes[ParentIndex].LeftChild;

	if (GrandParentIndex != INDEX_NONE)
	{
		FNode& GrandParent = Nodes[GrandParentIndex];
		if (GrandParent.LeftChild == ParentIndex)
		{
			GrandParent.LeftChild = SiblingIndex;
		}
		else
		{
			check(GrandParent.RightChild == ParentIndex);
			GrandParent.RightChild = SiblingIndex;
		}
		Nodes[SiblingIndex].Parent = GrandParentIndex;
		FreeNode(ParentIndex);
		RefitFrom(GrandParentIndex);
	}
	else
	{
		RootNode = SiblingIndex;
		Nodes[SiblingIndex].Parent = INDEX_NONE;
		FreeNode(ParentIndex);
	}

	Nodes[LeafIndex].Parent = INDEX_NONE;
}

int32 FBuildDynamicAABBTree::Balance(const int32 NodeIndex)
{
	check(Nodes.IsValidIndex(NodeIndex));
	FNode& NodeA = Nodes[NodeIndex];
	if (NodeA.IsLeaf() || NodeA.Height < 2)
	{
		return NodeIndex;
	}

	const int32 NodeBIndex = NodeA.LeftChild;
	const int32 NodeCIndex = NodeA.RightChild;
	FNode& NodeB = Nodes[NodeBIndex];
	FNode& NodeC = Nodes[NodeCIndex];
	const int32 HeightBalance = NodeC.Height - NodeB.Height;

	if (HeightBalance > 1)
	{
		const int32 NodeFIndex = NodeC.LeftChild;
		const int32 NodeGIndex = NodeC.RightChild;
		FNode& NodeF = Nodes[NodeFIndex];
		FNode& NodeG = Nodes[NodeGIndex];

		NodeC.LeftChild = NodeIndex;
		NodeC.Parent = NodeA.Parent;
		NodeA.Parent = NodeCIndex;
		if (NodeC.Parent != INDEX_NONE)
		{
			FNode& OldParent = Nodes[NodeC.Parent];
			if (OldParent.LeftChild == NodeIndex)
			{
				OldParent.LeftChild = NodeCIndex;
			}
			else
			{
				check(OldParent.RightChild == NodeIndex);
				OldParent.RightChild = NodeCIndex;
			}
		}
		else
		{
			RootNode = NodeCIndex;
		}

		if (NodeF.Height > NodeG.Height)
		{
			NodeC.RightChild = NodeFIndex;
			NodeA.RightChild = NodeGIndex;
			NodeG.Parent = NodeIndex;
			NodeA.TreeBounds = MergeBounds(NodeB.TreeBounds, NodeG.TreeBounds);
			NodeC.TreeBounds = MergeBounds(NodeA.TreeBounds, NodeF.TreeBounds);
			NodeA.Height = 1 + FMath::Max(NodeB.Height, NodeG.Height);
			NodeC.Height = 1 + FMath::Max(NodeA.Height, NodeF.Height);
			NodeA.Cost = NodeB.Cost + NodeG.Cost;
			NodeC.Cost = NodeA.Cost + NodeF.Cost;
		}
		else
		{
			NodeC.RightChild = NodeGIndex;
			NodeA.RightChild = NodeFIndex;
			NodeF.Parent = NodeIndex;
			NodeA.TreeBounds = MergeBounds(NodeB.TreeBounds, NodeF.TreeBounds);
			NodeC.TreeBounds = MergeBounds(NodeA.TreeBounds, NodeG.TreeBounds);
			NodeA.Height = 1 + FMath::Max(NodeB.Height, NodeF.Height);
			NodeC.Height = 1 + FMath::Max(NodeA.Height, NodeG.Height);
			NodeA.Cost = NodeB.Cost + NodeF.Cost;
			NodeC.Cost = NodeA.Cost + NodeG.Cost;
		}
		return NodeCIndex;
	}

	if (HeightBalance < -1)
	{
		const int32 NodeDIndex = NodeB.LeftChild;
		const int32 NodeEIndex = NodeB.RightChild;
		FNode& NodeD = Nodes[NodeDIndex];
		FNode& NodeE = Nodes[NodeEIndex];

		NodeB.LeftChild = NodeIndex;
		NodeB.Parent = NodeA.Parent;
		NodeA.Parent = NodeBIndex;
		if (NodeB.Parent != INDEX_NONE)
		{
			FNode& OldParent = Nodes[NodeB.Parent];
			if (OldParent.LeftChild == NodeIndex)
			{
				OldParent.LeftChild = NodeBIndex;
			}
			else
			{
				check(OldParent.RightChild == NodeIndex);
				OldParent.RightChild = NodeBIndex;
			}
		}
		else
		{
			RootNode = NodeBIndex;
		}

		if (NodeD.Height > NodeE.Height)
		{
			NodeB.RightChild = NodeDIndex;
			NodeA.LeftChild = NodeEIndex;
			NodeE.Parent = NodeIndex;
			NodeA.TreeBounds = MergeBounds(NodeC.TreeBounds, NodeE.TreeBounds);
			NodeB.TreeBounds = MergeBounds(NodeA.TreeBounds, NodeD.TreeBounds);
			NodeA.Height = 1 + FMath::Max(NodeC.Height, NodeE.Height);
			NodeB.Height = 1 + FMath::Max(NodeA.Height, NodeD.Height);
			NodeA.Cost = NodeC.Cost + NodeE.Cost;
			NodeB.Cost = NodeA.Cost + NodeD.Cost;
		}
		else
		{
			NodeB.RightChild = NodeEIndex;
			NodeA.LeftChild = NodeDIndex;
			NodeD.Parent = NodeIndex;
			NodeA.TreeBounds = MergeBounds(NodeC.TreeBounds, NodeD.TreeBounds);
			NodeB.TreeBounds = MergeBounds(NodeA.TreeBounds, NodeE.TreeBounds);
			NodeA.Height = 1 + FMath::Max(NodeC.Height, NodeD.Height);
			NodeB.Height = 1 + FMath::Max(NodeA.Height, NodeE.Height);
			NodeA.Cost = NodeC.Cost + NodeD.Cost;
			NodeB.Cost = NodeA.Cost + NodeE.Cost;
		}
		return NodeBIndex;
	}

	return NodeIndex;
}

void FBuildDynamicAABBTree::RefitFrom(int32 NodeIndex)
{
	while (NodeIndex != INDEX_NONE)
	{
		NodeIndex = Balance(NodeIndex);
		FNode& Node = Nodes[NodeIndex];
		if (!Node.IsLeaf())
		{
			const FNode& LeftChild = Nodes[Node.LeftChild];
			const FNode& RightChild = Nodes[Node.RightChild];
			Node.Height = 1 + FMath::Max(LeftChild.Height, RightChild.Height);
			Node.TreeBounds = MergeBounds(LeftChild.TreeBounds, RightChild.TreeBounds);
			Node.Cost = LeftChild.Cost + RightChild.Cost;
		}
		NodeIndex = Node.Parent;
	}
}

void FBuildDynamicAABBTree::BumpRevision()
{
	Revision = Revision == MAX_uint64 ? 1 : Revision + 1;
}

FBox FBuildDynamicAABBTree::MakeTreeBounds(const FBox& ExactBounds) const
{
	return BoundsPadding > 0.0
		? ExactBounds.ExpandBy(BoundsPadding)
		: ExactBounds;
}

#if WITH_DEV_AUTOMATION_TESTS

bool FBuildDynamicAABBTree::Validate() const
{
	if (RootNode == INDEX_NONE)
	{
		return LeafByEntity.IsEmpty();
	}

	if (!Nodes.IsValidIndex(RootNode) || Nodes[RootNode].Parent != INDEX_NONE)
	{
		return false;
	}

	int32 LeafCount = 0;
	return ValidateNode(RootNode, INDEX_NONE, LeafCount)
		&& LeafCount == LeafByEntity.Num();
}

bool FBuildDynamicAABBTree::ValidateNode(
	const int32 NodeIndex,
	const int32 ExpectedParent,
	int32& OutLeafCount) const
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return false;
	}

	const FNode& Node = Nodes[NodeIndex];
	if (Node.Parent != ExpectedParent || Node.Height < 0 || !IsValidSpatialBounds(Node.TreeBounds))
	{
		return false;
	}

	if (Node.IsLeaf())
	{
		const int32* MappedNode = LeafByEntity.Find(Node.Entity);
		++OutLeafCount;
		return Node.LeftChild == INDEX_NONE
			&& Node.RightChild == INDEX_NONE
			&& MappedNode
			&& *MappedNode == NodeIndex
			&& Node.Cost > 0
			&& IsValidSpatialBounds(Node.ExactBounds)
			&& Node.TreeBounds.IsInsideOrOn(Node.ExactBounds);
	}

	if (Node.LeftChild == INDEX_NONE || Node.RightChild == INDEX_NONE)
	{
		return false;
	}

	const FNode& LeftChild = Nodes[Node.LeftChild];
	const FNode& RightChild = Nodes[Node.RightChild];
	const FBox ExpectedBounds = MergeBounds(LeftChild.TreeBounds, RightChild.TreeBounds);
	return Node.Height == 1 + FMath::Max(LeftChild.Height, RightChild.Height)
			&& Node.TreeBounds.Equals(ExpectedBounds)
			&& Node.Cost == LeftChild.Cost + RightChild.Cost
		&& ValidateNode(Node.LeftChild, NodeIndex, OutLeafCount)
		&& ValidateNode(Node.RightChild, NodeIndex, OutLeafCount);
}

#endif
