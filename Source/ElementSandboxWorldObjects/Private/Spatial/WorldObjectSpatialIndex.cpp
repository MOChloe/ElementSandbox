#include "Spatial/WorldObjectSpatialIndex.h"

#include "Algo/Sort.h"
#include "ElementSandboxWorldObjects.h"

namespace
{
	constexpr int32 StaticBinCount = 8;
	constexpr int32 StaticLinearScanThreshold = 32;
	constexpr double RayParallelTolerance = 1.0e-12;

	bool IsValidBounds(const FBox& Bounds)
	{
		return Bounds.IsValid != 0 && !Bounds.ContainsNaN();
	}

	double SurfaceArea(const FBox& Bounds)
	{
		if (!IsValidBounds(Bounds))
		{
			return 0.0;
		}
		const FVector Size = Bounds.GetSize();
		return 2.0 * (Size.X * Size.Y + Size.X * Size.Z + Size.Y * Size.Z);
	}

	bool ContainsBounds(const FBox& Outer, const FBox& Inner)
	{
		return Outer.Min.X <= Inner.Min.X && Outer.Min.Y <= Inner.Min.Y
			&& Outer.Min.Z <= Inner.Min.Z && Outer.Max.X >= Inner.Max.X
			&& Outer.Max.Y >= Inner.Max.Y && Outer.Max.Z >= Inner.Max.Z;
	}

	FBox ExpandBounds(const FBox& Bounds, const double Padding)
	{
		return Bounds.ExpandBy(FMath::Max(0.0, Padding));
	}

	bool RayBoxEntry(
		const FBox& Bounds,
		const FVector& Origin,
		const FVector& UnitDirection,
		const double MaxDistance,
		double& OutDistance)
	{
		if (!IsValidBounds(Bounds)
			|| Origin.ContainsNaN()
			|| UnitDirection.ContainsNaN()
			|| UnitDirection.IsNearlyZero()
			|| !FMath::IsFinite(MaxDistance)
			|| MaxDistance < 0.0)
		{
			return false;
		}

		double Entry = 0.0;
		double Exit = MaxDistance;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Direction = UnitDirection[Axis];
			if (FMath::Abs(Direction) <= RayParallelTolerance)
			{
				if (Origin[Axis] < Bounds.Min[Axis] || Origin[Axis] > Bounds.Max[Axis])
				{
					return false;
				}
				continue;
			}

			double Near = (Bounds.Min[Axis] - Origin[Axis]) / Direction;
			double Far = (Bounds.Max[Axis] - Origin[Axis]) / Direction;
			if (Near > Far)
			{
				Swap(Near, Far);
			}
			Entry = FMath::Max(Entry, Near);
			Exit = FMath::Min(Exit, Far);
			if (Entry > Exit)
			{
				return false;
			}
		}

		OutDistance = Entry;
		return Entry <= MaxDistance && Exit >= 0.0;
	}

	struct FSpatialEntry final
	{
		FWorldObjectEntityHandle Entity;
		FBox Bounds = FBox(ForceInit);
	};

	class FStaticWorldObjectBVH final
	{
	public:
		void Build(const TArray<FSpatialEntry>& InEntries)
		{
			Entries = InEntries;
			Algo::Sort(Entries, [](const FSpatialEntry& Left, const FSpatialEntry& Right)
			{
				return Left.Entity < Right.Entity;
			});
			Nodes.Reset();
			Root = INDEX_NONE;
			if (Entries.IsEmpty())
			{
				return;
			}

			WorkingIndices.SetNumUninitialized(Entries.Num());
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				WorkingIndices[Index] = Index;
			}
			Root = BuildRange(0, WorkingIndices.Num());
			WorkingIndices.Reset();
		}

		void Query(
			const FBox& Bounds,
			TArray<int32>& Stack,
			int32& OutVisitedNodes,
			TArray<FWorldObjectEntityHandle>& OutEntities) const
		{
			if (Root == INDEX_NONE || !IsValidBounds(Bounds))
			{
				return;
			}
			Stack.Reset();
			Stack.Add(Root);
			while (!Stack.IsEmpty())
			{
				++OutVisitedNodes;
				const FNode& Node = Nodes[Stack.Pop(EAllowShrinking::No)];
				if (!Node.Bounds.Intersect(Bounds))
				{
					continue;
				}
				if (Node.IsLeaf())
				{
					const FSpatialEntry& Entry = Entries[Node.EntryIndex];
					if (Entry.Bounds.Intersect(Bounds))
					{
						OutEntities.Add(Entry.Entity);
					}
				}
				else
				{
					Stack.Add(Node.Left);
					Stack.Add(Node.Right);
				}
			}
		}

		void QueryRay(
			const FVector& Origin,
			const FVector& UnitDirection,
			const double MaxDistance,
			TArray<int32>& Stack,
			int32& OutVisitedNodes,
			TArray<FWorldObjectSpatialRayHit>& OutHits) const
		{
			if (Root == INDEX_NONE)
			{
				return;
			}
			Stack.Reset();
			Stack.Add(Root);
			while (!Stack.IsEmpty())
			{
				++OutVisitedNodes;
				const FNode& Node = Nodes[Stack.Pop(EAllowShrinking::No)];
				double IgnoredDistance = 0.0;
				if (!RayBoxEntry(Node.Bounds, Origin, UnitDirection, MaxDistance, IgnoredDistance))
				{
					continue;
				}
				if (!Node.IsLeaf())
				{
					Stack.Add(Node.Left);
					Stack.Add(Node.Right);
					continue;
				}

				const FSpatialEntry& Entry = Entries[Node.EntryIndex];
				double Distance = 0.0;
				if (RayBoxEntry(Entry.Bounds, Origin, UnitDirection, MaxDistance, Distance))
				{
					OutHits.Add({Entry.Entity, Distance});
				}
			}
		}

			SIZE_T GetAllocatedSize() const
			{
				return Entries.GetAllocatedSize()
					+ Nodes.GetAllocatedSize()
					+ WorkingIndices.GetAllocatedSize();
			}

			bool IsEmpty() const { return Root == INDEX_NONE; }
			FBox GetBounds() const
			{
				return Root != INDEX_NONE ? Nodes[Root].Bounds : FBox(ForceInit);
			}

	private:
		struct FNode final
		{
			FBox Bounds = FBox(ForceInit);
			int32 Left = INDEX_NONE;
			int32 Right = INDEX_NONE;
			int32 EntryIndex = INDEX_NONE;

			bool IsLeaf() const { return EntryIndex != INDEX_NONE; }
		};

		struct FBin final
		{
			FBox Bounds = FBox(ForceInit);
			int32 Count = 0;
		};

		int32 BuildRange(const int32 Begin, const int32 End)
		{
			const int32 NodeIndex = Nodes.AddDefaulted();
			FBox Bounds(ForceInit);
			FBox CentroidBounds(ForceInit);
			for (int32 Cursor = Begin; Cursor < End; ++Cursor)
			{
				const FSpatialEntry& Entry = Entries[WorkingIndices[Cursor]];
				Bounds += Entry.Bounds;
				CentroidBounds += Entry.Bounds.GetCenter();
			}

			const int32 Count = End - Begin;
			Nodes[NodeIndex].Bounds = Bounds;
			if (Count == 1)
			{
				Nodes[NodeIndex].EntryIndex = WorkingIndices[Begin];
				return NodeIndex;
			}

			int32 BestAxis = INDEX_NONE;
			int32 BestSplit = INDEX_NONE;
			double BestCost = TNumericLimits<double>::Max();
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const double Extent = CentroidBounds.Max[Axis] - CentroidBounds.Min[Axis];
				if (Extent <= UE_DOUBLE_SMALL_NUMBER)
				{
					continue;
				}

				FBin Bins[StaticBinCount];
				for (int32 Cursor = Begin; Cursor < End; ++Cursor)
				{
					const FSpatialEntry& Entry = Entries[WorkingIndices[Cursor]];
					const double Normalized =
						(Entry.Bounds.GetCenter()[Axis] - CentroidBounds.Min[Axis]) / Extent;
					const int32 BinIndex = FMath::Clamp(
						FMath::FloorToInt(Normalized * StaticBinCount),
						0,
						StaticBinCount - 1);
					Bins[BinIndex].Bounds += Entry.Bounds;
					++Bins[BinIndex].Count;
				}

				FBox LeftBounds[StaticBinCount - 1];
				FBox RightBounds[StaticBinCount - 1];
				int32 LeftCounts[StaticBinCount - 1] = {};
				int32 RightCounts[StaticBinCount - 1] = {};
				FBox RunningBounds(ForceInit);
				int32 RunningCount = 0;
				for (int32 Split = 0; Split < StaticBinCount - 1; ++Split)
				{
					RunningBounds += Bins[Split].Bounds;
					RunningCount += Bins[Split].Count;
					LeftBounds[Split] = RunningBounds;
					LeftCounts[Split] = RunningCount;
				}
				RunningBounds = FBox(ForceInit);
				RunningCount = 0;
				for (int32 Split = StaticBinCount - 1; Split > 0; --Split)
				{
					RunningBounds += Bins[Split].Bounds;
					RunningCount += Bins[Split].Count;
					RightBounds[Split - 1] = RunningBounds;
					RightCounts[Split - 1] = RunningCount;
				}

				for (int32 Split = 0; Split < StaticBinCount - 1; ++Split)
				{
					if (LeftCounts[Split] == 0 || RightCounts[Split] == 0)
					{
						continue;
					}
					const double Cost = SurfaceArea(LeftBounds[Split]) * LeftCounts[Split]
						+ SurfaceArea(RightBounds[Split]) * RightCounts[Split];
					if (Cost < BestCost)
					{
						BestCost = Cost;
						BestAxis = Axis;
						BestSplit = Split;
					}
				}
			}

			int32 Middle = INDEX_NONE;
			if (BestAxis != INDEX_NONE)
			{
				const double MinCentroid = CentroidBounds.Min[BestAxis];
				const double Extent = CentroidBounds.Max[BestAxis] - MinCentroid;
				TArray<int32, TInlineAllocator<64>> LeftIndices;
				TArray<int32, TInlineAllocator<64>> RightIndices;
				for (int32 Cursor = Begin; Cursor < End; ++Cursor)
				{
					const int32 EntryIndex = WorkingIndices[Cursor];
					const double Normalized =
						(Entries[EntryIndex].Bounds.GetCenter()[BestAxis] - MinCentroid) / Extent;
					const int32 BinIndex = FMath::Clamp(
						FMath::FloorToInt(Normalized * StaticBinCount),
						0,
						StaticBinCount - 1);
					(BinIndex <= BestSplit ? LeftIndices : RightIndices).Add(EntryIndex);
				}
				if (!LeftIndices.IsEmpty() && !RightIndices.IsEmpty())
				{
					int32 Cursor = Begin;
					for (const int32 EntryIndex : LeftIndices)
					{
						WorkingIndices[Cursor++] = EntryIndex;
					}
					Middle = Cursor;
					for (const int32 EntryIndex : RightIndices)
					{
						WorkingIndices[Cursor++] = EntryIndex;
					}
				}
			}

			if (Middle == INDEX_NONE)
			{
				const FVector CentroidSize = CentroidBounds.GetSize();
				const int32 Axis = CentroidSize.X >= CentroidSize.Y
					? (CentroidSize.X >= CentroidSize.Z ? 0 : 2)
					: (CentroidSize.Y >= CentroidSize.Z ? 1 : 2);
				Algo::Sort(
					MakeArrayView(WorkingIndices).Slice(Begin, Count),
					[this, Axis](const int32 Left, const int32 Right)
					{
						const double LeftCenter = Entries[Left].Bounds.GetCenter()[Axis];
						const double RightCenter = Entries[Right].Bounds.GetCenter()[Axis];
						return LeftCenter != RightCenter
							? LeftCenter < RightCenter
							: Entries[Left].Entity < Entries[Right].Entity;
					});
				Middle = Begin + Count / 2;
			}

			Nodes[NodeIndex].Left = BuildRange(Begin, Middle);
			Nodes[NodeIndex].Right = BuildRange(Middle, End);
			return NodeIndex;
		}

		TArray<FSpatialEntry> Entries;
		TArray<FNode> Nodes;
		TArray<int32> WorkingIndices;
			int32 Root = INDEX_NONE;
		};

		class FPortableDynamicAABBTree final
	{
	public:
		explicit FPortableDynamicAABBTree(const double InPadding)
			: Padding(FMath::Max(0.0, InPadding))
		{
		}

		bool Insert(const FWorldObjectEntityHandle Entity, const FBox& ExactBounds)
		{
			if (!Entity.IsSet() || !IsValidBounds(ExactBounds) || EntityToNode.Contains(Entity))
			{
				return false;
			}
			const int32 Leaf = AllocateNode();
			FNode& Node = Nodes[Leaf];
			Node.Entity = Entity;
			Node.ExactBounds = ExactBounds;
			Node.Bounds = ExpandBounds(ExactBounds, Padding);
			Node.Height = 0;
			EntityToNode.Add(Entity, Leaf);
			InsertLeaf(Leaf);
			return true;
		}

		bool Remove(const FWorldObjectEntityHandle Entity)
		{
			const int32* Leaf = EntityToNode.Find(Entity);
			if (!Leaf)
			{
				return false;
			}
			const int32 LeafIndex = *Leaf;
			RemoveLeaf(LeafIndex);
			FreeNode(LeafIndex);
			EntityToNode.Remove(Entity);
			return true;
		}

		bool Update(const FWorldObjectEntityHandle Entity, const FBox& ExactBounds)
		{
			const int32* Leaf = EntityToNode.Find(Entity);
			if (!Leaf || !IsValidBounds(ExactBounds))
			{
				return false;
			}
			FNode& Node = Nodes[*Leaf];
			Node.ExactBounds = ExactBounds;
			if (ContainsBounds(Node.Bounds, ExactBounds))
			{
				return true;
			}

			RemoveLeaf(*Leaf);
			Node.Bounds = ExpandBounds(ExactBounds, Padding);
			InsertLeaf(*Leaf);
			++ReinsertCount;
			return true;
		}

		bool TryGetBounds(const FWorldObjectEntityHandle Entity, FBox& OutBounds) const
		{
			const int32* Leaf = EntityToNode.Find(Entity);
			if (!Leaf)
			{
				return false;
			}
			OutBounds = Nodes[*Leaf].ExactBounds;
			return true;
		}

		bool Contains(const FWorldObjectEntityHandle Entity) const
		{
			return EntityToNode.Contains(Entity);
		}

		void Query(
			const FBox& Bounds,
			TArray<int32>& Stack,
			int32& OutVisitedNodes,
			TArray<FWorldObjectEntityHandle>& OutEntities) const
		{
			if (Root == INDEX_NONE || !IsValidBounds(Bounds))
			{
				return;
			}
			Stack.Reset();
			Stack.Add(Root);
			while (!Stack.IsEmpty())
			{
				++OutVisitedNodes;
				const FNode& Node = Nodes[Stack.Pop(EAllowShrinking::No)];
				if (!Node.Bounds.Intersect(Bounds))
				{
					continue;
				}
				if (Node.IsLeaf())
				{
					if (Node.ExactBounds.Intersect(Bounds))
					{
						OutEntities.Add(Node.Entity);
					}
				}
				else
				{
					Stack.Add(Node.Left);
					Stack.Add(Node.Right);
				}
			}
		}

		void QueryRay(
			const FVector& Origin,
			const FVector& UnitDirection,
			const double MaxDistance,
			TArray<int32>& Stack,
			int32& OutVisitedNodes,
			TArray<FWorldObjectSpatialRayHit>& OutHits) const
		{
			if (Root == INDEX_NONE)
			{
				return;
			}
			Stack.Reset();
			Stack.Add(Root);
			while (!Stack.IsEmpty())
			{
				++OutVisitedNodes;
				const FNode& Node = Nodes[Stack.Pop(EAllowShrinking::No)];
				double IgnoredDistance = 0.0;
				if (!RayBoxEntry(Node.Bounds, Origin, UnitDirection, MaxDistance, IgnoredDistance))
				{
					continue;
				}
				if (!Node.IsLeaf())
				{
					Stack.Add(Node.Left);
					Stack.Add(Node.Right);
					continue;
				}
				double Distance = 0.0;
				if (RayBoxEntry(Node.ExactBounds, Origin, UnitDirection, MaxDistance, Distance))
				{
					OutHits.Add({Node.Entity, Distance});
				}
			}
		}

		int32 Num() const { return EntityToNode.Num(); }
		uint64 GetReinsertCount() const { return ReinsertCount; }
		SIZE_T GetAllocatedSize() const
		{
			return Nodes.GetAllocatedSize() + EntityToNode.GetAllocatedSize();
		}

		bool Validate() const
		{
			if (Root == INDEX_NONE)
			{
				return EntityToNode.IsEmpty();
			}
			int32 LeafCount = 0;
			return Nodes.IsValidIndex(Root)
				&& Nodes[Root].Parent == INDEX_NONE
				&& ValidateNode(Root, LeafCount)
				&& LeafCount == EntityToNode.Num();
		}

	private:
		struct FNode final
		{
			FBox Bounds = FBox(ForceInit);
			FBox ExactBounds = FBox(ForceInit);
			FWorldObjectEntityHandle Entity;
			int32 Parent = INDEX_NONE;
			int32 Left = INDEX_NONE;
			int32 Right = INDEX_NONE;
			int32 Height = -1;
			int32 NextFree = INDEX_NONE;

			bool IsLeaf() const { return Left == INDEX_NONE; }
		};

		int32 AllocateNode()
		{
			if (FirstFree != INDEX_NONE)
			{
				const int32 Index = FirstFree;
				FirstFree = Nodes[Index].NextFree;
				Nodes[Index] = FNode();
				return Index;
			}
			return Nodes.AddDefaulted();
		}

		void FreeNode(const int32 Index)
		{
			Nodes[Index] = FNode();
			Nodes[Index].NextFree = FirstFree;
			FirstFree = Index;
		}

		void InsertLeaf(const int32 Leaf)
		{
			if (Root == INDEX_NONE)
			{
				Root = Leaf;
				Nodes[Leaf].Parent = INDEX_NONE;
				return;
			}

			const FBox LeafBounds = Nodes[Leaf].Bounds;
			int32 Sibling = Root;
			while (!Nodes[Sibling].IsLeaf())
			{
				const int32 Left = Nodes[Sibling].Left;
				const int32 Right = Nodes[Sibling].Right;
				const double CurrentArea = SurfaceArea(Nodes[Sibling].Bounds);
				const FBox Combined = Nodes[Sibling].Bounds + LeafBounds;
				const double CombinedArea = SurfaceArea(Combined);
				const double ParentCost = 2.0 * CombinedArea;
				const double InheritanceCost = 2.0 * (CombinedArea - CurrentArea);
				const double LeftCost = Nodes[Left].IsLeaf()
					? SurfaceArea(Nodes[Left].Bounds + LeafBounds) + InheritanceCost
					: SurfaceArea(Nodes[Left].Bounds + LeafBounds)
						- SurfaceArea(Nodes[Left].Bounds) + InheritanceCost;
				const double RightCost = Nodes[Right].IsLeaf()
					? SurfaceArea(Nodes[Right].Bounds + LeafBounds) + InheritanceCost
					: SurfaceArea(Nodes[Right].Bounds + LeafBounds)
						- SurfaceArea(Nodes[Right].Bounds) + InheritanceCost;
				if (ParentCost < LeftCost && ParentCost < RightCost)
				{
					break;
				}
				Sibling = LeftCost < RightCost ? Left : Right;
			}

			const int32 OldParent = Nodes[Sibling].Parent;
			const int32 NewParent = AllocateNode();
			Nodes[NewParent].Parent = OldParent;
			Nodes[NewParent].Bounds = LeafBounds + Nodes[Sibling].Bounds;
			Nodes[NewParent].Height = Nodes[Sibling].Height + 1;
			Nodes[NewParent].Left = Sibling;
			Nodes[NewParent].Right = Leaf;
			Nodes[Sibling].Parent = NewParent;
			Nodes[Leaf].Parent = NewParent;
			if (OldParent == INDEX_NONE)
			{
				Root = NewParent;
			}
			else if (Nodes[OldParent].Left == Sibling)
			{
				Nodes[OldParent].Left = NewParent;
			}
			else
			{
				Nodes[OldParent].Right = NewParent;
			}

			RefitToRoot(NewParent);
		}

		void RemoveLeaf(const int32 Leaf)
		{
			if (Leaf == Root)
			{
				Root = INDEX_NONE;
				Nodes[Leaf].Parent = INDEX_NONE;
				return;
			}
			const int32 Parent = Nodes[Leaf].Parent;
			const int32 GrandParent = Nodes[Parent].Parent;
			const int32 Sibling = Nodes[Parent].Left == Leaf
				? Nodes[Parent].Right
				: Nodes[Parent].Left;
			if (GrandParent == INDEX_NONE)
			{
				Root = Sibling;
				Nodes[Sibling].Parent = INDEX_NONE;
			}
			else
			{
				if (Nodes[GrandParent].Left == Parent)
				{
					Nodes[GrandParent].Left = Sibling;
				}
				else
				{
					Nodes[GrandParent].Right = Sibling;
				}
				Nodes[Sibling].Parent = GrandParent;
				RefitToRoot(GrandParent);
			}
			Nodes[Leaf].Parent = INDEX_NONE;
			FreeNode(Parent);
		}

		void RefitToRoot(int32 Index)
		{
			while (Index != INDEX_NONE)
			{
				Index = Balance(Index);
				FNode& Node = Nodes[Index];
				if (!Node.IsLeaf())
				{
					Node.Height = 1 + FMath::Max(Nodes[Node.Left].Height, Nodes[Node.Right].Height);
					Node.Bounds = Nodes[Node.Left].Bounds + Nodes[Node.Right].Bounds;
				}
				Index = Node.Parent;
			}
		}

		int32 Balance(const int32 IndexA)
		{
			FNode& A = Nodes[IndexA];
			if (A.IsLeaf() || A.Height < 2)
			{
				return IndexA;
			}
			const int32 IndexB = A.Left;
			const int32 IndexC = A.Right;
			FNode& B = Nodes[IndexB];
			FNode& C = Nodes[IndexC];
			const int32 BalanceValue = C.Height - B.Height;
			if (BalanceValue > 1)
			{
				const int32 IndexF = C.Left;
				const int32 IndexG = C.Right;
				FNode& F = Nodes[IndexF];
				FNode& G = Nodes[IndexG];
				C.Left = IndexA;
				C.Parent = A.Parent;
				A.Parent = IndexC;
				ReplaceParentChild(C.Parent, IndexA, IndexC);
				if (F.Height > G.Height)
				{
					C.Right = IndexF;
					A.Right = IndexG;
					G.Parent = IndexA;
					A.Bounds = B.Bounds + G.Bounds;
					C.Bounds = A.Bounds + F.Bounds;
					A.Height = 1 + FMath::Max(B.Height, G.Height);
					C.Height = 1 + FMath::Max(A.Height, F.Height);
				}
				else
				{
					C.Right = IndexG;
					A.Right = IndexF;
					F.Parent = IndexA;
					A.Bounds = B.Bounds + F.Bounds;
					C.Bounds = A.Bounds + G.Bounds;
					A.Height = 1 + FMath::Max(B.Height, F.Height);
					C.Height = 1 + FMath::Max(A.Height, G.Height);
				}
				return IndexC;
			}
			if (BalanceValue < -1)
			{
				const int32 IndexD = B.Left;
				const int32 IndexE = B.Right;
				FNode& D = Nodes[IndexD];
				FNode& E = Nodes[IndexE];
				B.Left = IndexA;
				B.Parent = A.Parent;
				A.Parent = IndexB;
				ReplaceParentChild(B.Parent, IndexA, IndexB);
				if (D.Height > E.Height)
				{
					B.Right = IndexD;
					A.Left = IndexE;
					E.Parent = IndexA;
					A.Bounds = C.Bounds + E.Bounds;
					B.Bounds = A.Bounds + D.Bounds;
					A.Height = 1 + FMath::Max(C.Height, E.Height);
					B.Height = 1 + FMath::Max(A.Height, D.Height);
				}
				else
				{
					B.Right = IndexE;
					A.Left = IndexD;
					D.Parent = IndexA;
					A.Bounds = C.Bounds + D.Bounds;
					B.Bounds = A.Bounds + E.Bounds;
					A.Height = 1 + FMath::Max(C.Height, D.Height);
					B.Height = 1 + FMath::Max(A.Height, E.Height);
				}
				return IndexB;
			}
			return IndexA;
		}

		void ReplaceParentChild(
			const int32 Parent,
			const int32 OldChild,
			const int32 NewChild)
		{
			if (Parent == INDEX_NONE)
			{
				Root = NewChild;
			}
			else if (Nodes[Parent].Left == OldChild)
			{
				Nodes[Parent].Left = NewChild;
			}
			else
			{
				Nodes[Parent].Right = NewChild;
			}
		}

		bool ValidateNode(const int32 Index, int32& InOutLeafCount) const
		{
			if (!Nodes.IsValidIndex(Index) || Nodes[Index].Height < 0)
			{
				return false;
			}
			const FNode& Node = Nodes[Index];
			if (Node.IsLeaf())
			{
				++InOutLeafCount;
				const int32* Mapped = EntityToNode.Find(Node.Entity);
				return Node.Height == 0 && Mapped && *Mapped == Index
					&& ContainsBounds(Node.Bounds, Node.ExactBounds);
			}
			if (!Nodes.IsValidIndex(Node.Left) || !Nodes.IsValidIndex(Node.Right)
				|| Nodes[Node.Left].Parent != Index || Nodes[Node.Right].Parent != Index)
			{
				return false;
			}
			const int32 ExpectedHeight = 1 + FMath::Max(
				Nodes[Node.Left].Height,
				Nodes[Node.Right].Height);
			const FBox ExpectedBounds = Nodes[Node.Left].Bounds + Nodes[Node.Right].Bounds;
			return Node.Height == ExpectedHeight
				&& Node.Bounds.Min.Equals(ExpectedBounds.Min)
				&& Node.Bounds.Max.Equals(ExpectedBounds.Max)
				&& ValidateNode(Node.Left, InOutLeafCount)
				&& ValidateNode(Node.Right, InOutLeafCount);
		}

		double Padding = 0.0;
		TArray<FNode> Nodes;
		TMap<FWorldObjectEntityHandle, int32> EntityToNode;
		int32 Root = INDEX_NONE;
		int32 FirstFree = INDEX_NONE;
		uint64 ReinsertCount = 0;
	};
}

class FWorldObjectSpatialIndexImpl final
{
public:
	struct FStaticChunk final
	{
		TArray<FSpatialEntry> Entries;
		TMap<FWorldObjectEntityHandle, int32> EntryIndexByEntity;
		FBox AggregateBounds = FBox(ForceInit);
		mutable FStaticWorldObjectBVH Tree;
		mutable bool bTreeDirty = false;

		void RefreshAggregateBounds()
		{
			AggregateBounds = FBox(ForceInit);
			for (const FSpatialEntry& Entry : Entries)
			{
				AggregateBounds += Entry.Bounds;
			}
		}

		bool Add(const FWorldObjectEntityHandle Entity, const FBox& Bounds)
		{
			if (EntryIndexByEntity.Contains(Entity))
			{
				return false;
			}
			EntryIndexByEntity.Add(Entity, Entries.Add({Entity, Bounds}));
			AggregateBounds += Bounds;
			bTreeDirty = true;
			return true;
		}

		bool Remove(const FWorldObjectEntityHandle Entity)
		{
			const int32* FoundIndex = EntryIndexByEntity.Find(Entity);
			if (!FoundIndex)
			{
				return false;
			}
			const int32 Index = *FoundIndex;
			const int32 LastIndex = Entries.Num() - 1;
			if (Index != LastIndex)
			{
				Entries[Index] = Entries[LastIndex];
				EntryIndexByEntity.FindChecked(Entries[Index].Entity) = Index;
			}
			Entries.RemoveAt(LastIndex, EAllowShrinking::No);
			EntryIndexByEntity.Remove(Entity);
			RefreshAggregateBounds();
			bTreeDirty = true;
			return true;
		}

		const FBox* FindBounds(const FWorldObjectEntityHandle Entity) const
		{
			const int32* Index = EntryIndexByEntity.Find(Entity);
			return Index && Entries.IsValidIndex(*Index) ? &Entries[*Index].Bounds : nullptr;
		}

		SIZE_T GetAllocatedSize() const
		{
			return Entries.GetAllocatedSize() + EntryIndexByEntity.GetAllocatedSize() + Tree.GetAllocatedSize();
		}
	};

	explicit FWorldObjectSpatialIndexImpl(const FWorldObjectSpatialConfig& InConfig)
			: Config(InConfig)
			, DynamicTree(InConfig.DynamicBoundsPadding)
		{
		}

	bool TryGetStaticChunkCoordinate(const FVector& AnchorLocation, FIntVector& OutCoordinate) const
	{
		if (AnchorLocation.ContainsNaN() || !FMath::IsFinite(Config.StaticChunkSize)
			|| Config.StaticChunkSize <= UE_SMALL_NUMBER)
		{
			return false;
		}
		const FVector Coordinate(
			FMath::FloorToDouble(AnchorLocation.X / Config.StaticChunkSize),
			FMath::FloorToDouble(AnchorLocation.Y / Config.StaticChunkSize),
			FMath::FloorToDouble(AnchorLocation.Z / Config.StaticChunkSize));
		if (Coordinate.ContainsNaN()
			|| Coordinate.X < MIN_int32 || Coordinate.X > MAX_int32
			|| Coordinate.Y < MIN_int32 || Coordinate.Y > MAX_int32
			|| Coordinate.Z < MIN_int32 || Coordinate.Z > MAX_int32)
		{
			return false;
		}
		OutCoordinate = FIntVector(
			static_cast<int32>(Coordinate.X),
			static_cast<int32>(Coordinate.Y),
			static_cast<int32>(Coordinate.Z));
		return true;
	}

		bool RebuildDirtyStaticChunks()
		{
			if (DirtyStaticChunks.IsEmpty())
			{
				return false;
			}
			TArray<FIntVector> DirtyCoordinates = DirtyStaticChunks.Array();
			DirtyCoordinates.Sort([](const FIntVector& A, const FIntVector& B)
			{
				return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z);
			});
			for (const FIntVector& Coordinate : DirtyCoordinates)
			{
				FStaticChunk* Chunk = StaticChunks.Find(Coordinate);
				if (!Chunk || !Chunk->bTreeDirty)
				{
					continue;
				}
				if (Chunk->Entries.Num() > StaticLinearScanThreshold)
				{
					Chunk->Tree.Build(Chunk->Entries);
					++StaticBuildCount;
				}
				else
				{
					Chunk->Tree.Build({});
				}
				Chunk->bTreeDirty = false;
			}
			DirtyStaticChunks.Reset();
			return true;
		}

		SIZE_T GetAllocatedSize() const
		{
			SIZE_T Size = StaticChunks.GetAllocatedSize()
				+ StaticChunkByEntity.GetAllocatedSize()
				+ DirtyStaticChunks.GetAllocatedSize()
				+ DynamicTree.GetAllocatedSize();
			for (const TPair<FIntVector, FStaticChunk>& Pair : StaticChunks)
			{
				Size += Pair.Value.GetAllocatedSize();
			}
			return Size;
		}

		FWorldObjectSpatialConfig Config;
		TMap<FIntVector, FStaticChunk> StaticChunks;
		TMap<FWorldObjectEntityHandle, FIntVector> StaticChunkByEntity;
		TSet<FIntVector> DirtyStaticChunks;
		FPortableDynamicAABBTree DynamicTree;
		double MaximumStaticAnchorOffset = 0.0;
		uint64 StaticBuildCount = 0;
};

FWorldObjectSpatialIndex::FWorldObjectSpatialIndex(
	const FWorldObjectSpatialConfig& InConfig)
	: Impl(MakeUnique<FWorldObjectSpatialIndexImpl>(InConfig))
{
}

FWorldObjectSpatialIndex::~FWorldObjectSpatialIndex() = default;

bool FWorldObjectSpatialIndex::Insert(
	const FWorldObjectEntityHandle Entity,
	const FBox& WorldBounds,
	const EWorldObjectSpatialClass SpatialClass)

{
	return Insert(Entity, WorldBounds, SpatialClass, WorldBounds.GetCenter());
}

bool FWorldObjectSpatialIndex::Insert(
	const FWorldObjectEntityHandle Entity,
	const FBox& WorldBounds,
	const EWorldObjectSpatialClass SpatialClass,
	const FVector& HomeAnchorLocation)
{
	if (!Impl || !Entity.IsSet() || !IsValidBounds(WorldBounds) || Contains(Entity))
	{
		return false;
	}
	if (SpatialClass == EWorldObjectSpatialClass::PermanentStatic)
	{
		FIntVector ChunkCoordinate = FIntVector::ZeroValue;
		if (!Impl->TryGetStaticChunkCoordinate(HomeAnchorLocation, ChunkCoordinate))
		{
			return false;
		}
		FWorldObjectSpatialIndexImpl::FStaticChunk& Chunk = Impl->StaticChunks.FindOrAdd(ChunkCoordinate);
		if (!Chunk.Add(Entity, WorldBounds))
		{
			return false;
		}
		Impl->StaticChunkByEntity.Add(Entity, ChunkCoordinate);
		Impl->DirtyStaticChunks.Add(ChunkCoordinate);
		const FVector AnchorDeltaMin = (WorldBounds.Min - HomeAnchorLocation).GetAbs();
		const FVector AnchorDeltaMax = (WorldBounds.Max - HomeAnchorLocation).GetAbs();
		Impl->MaximumStaticAnchorOffset = FMath::Max(
			Impl->MaximumStaticAnchorOffset,
			FMath::Max(AnchorDeltaMin.GetMax(), AnchorDeltaMax.GetMax()));
		return true;
	}
	return Impl->DynamicTree.Insert(Entity, WorldBounds);
}

bool FWorldObjectSpatialIndex::Remove(const FWorldObjectEntityHandle Entity)
{
	if (!Impl)
	{
		return false;
	}
	if (const FIntVector* Coordinate = Impl->StaticChunkByEntity.Find(Entity))
	{
		const FIntVector ChunkCoordinate = *Coordinate;
		FWorldObjectSpatialIndexImpl::FStaticChunk* Chunk = Impl->StaticChunks.Find(ChunkCoordinate);
		if (!Chunk || !Chunk->Remove(Entity))
		{
			return false;
		}
		Impl->StaticChunkByEntity.Remove(Entity);
		if (Chunk->Entries.IsEmpty())
		{
			Impl->StaticChunks.Remove(ChunkCoordinate);
		}
		Impl->DirtyStaticChunks.Add(ChunkCoordinate);
		return true;
	}
	return Impl->DynamicTree.Remove(Entity);
}

bool FWorldObjectSpatialIndex::UpdatePortable(
	const FWorldObjectEntityHandle Entity,
	const FBox& WorldBounds)
{
	return Impl
		&& !Impl->StaticChunkByEntity.Contains(Entity)
		&& Impl->DynamicTree.Update(Entity, WorldBounds);
}

bool FWorldObjectSpatialIndex::Contains(const FWorldObjectEntityHandle Entity) const
{
	return Impl
		&& (Impl->StaticChunkByEntity.Contains(Entity) || Impl->DynamicTree.Contains(Entity));
}

bool FWorldObjectSpatialIndex::TryGetBounds(
	const FWorldObjectEntityHandle Entity,
	FBox& OutBounds) const
{
	if (!Impl)
	{
		return false;
	}
	if (const FIntVector* Coordinate = Impl->StaticChunkByEntity.Find(Entity))
	{
		const FWorldObjectSpatialIndexImpl::FStaticChunk* Chunk = Impl->StaticChunks.Find(*Coordinate);
		if (Chunk)
		{
				if (const FBox* StaticBounds = Chunk->FindBounds(Entity))
			{
				OutBounds = *StaticBounds;
				return true;
			}
		}
		return false;
	}
	return Impl->DynamicTree.TryGetBounds(Entity, OutBounds);
}

bool FWorldObjectSpatialIndex::TryGetSpatialClass(
	const FWorldObjectEntityHandle Entity,
	EWorldObjectSpatialClass& OutClass) const
{
	if (!Impl)
	{
		return false;
	}
	if (Impl->StaticChunkByEntity.Contains(Entity))
	{
		OutClass = EWorldObjectSpatialClass::PermanentStatic;
		return true;
	}
	if (Impl->DynamicTree.Contains(Entity))
	{
		OutClass = EWorldObjectSpatialClass::Portable;
		return true;
	}
	return false;
}

void FWorldObjectSpatialIndex::QueryOverlaps(
	const FBox& QueryBounds,
	FWorldObjectSpatialQueryScratch& Scratch,
	TArray<FWorldObjectEntityHandle>& OutEntities) const
{
	OutEntities.Reset();
	Scratch.LastVisitedStaticNodes = 0;
	Scratch.LastVisitedDynamicNodes = 0;
	if (!Impl || !IsValidBounds(QueryBounds))
	{
		return;
	}
	Scratch.StaticChunkCandidates.Reset();
	const FVector Padding(Impl->MaximumStaticAnchorOffset);
	FIntVector MinCoordinate = FIntVector::ZeroValue;
	FIntVector MaxCoordinate = FIntVector::ZeroValue;
	if (Impl->TryGetStaticChunkCoordinate(QueryBounds.Min - Padding, MinCoordinate)
		&& Impl->TryGetStaticChunkCoordinate(QueryBounds.Max + Padding, MaxCoordinate))
	{
		for (int32 X = MinCoordinate.X; X <= MaxCoordinate.X; ++X)
		{
			for (int32 Y = MinCoordinate.Y; Y <= MaxCoordinate.Y; ++Y)
			{
				for (int32 Z = MinCoordinate.Z; Z <= MaxCoordinate.Z; ++Z)
				{
					const FIntVector Coordinate(X, Y, Z);
					const FWorldObjectSpatialIndexImpl::FStaticChunk* Chunk = Impl->StaticChunks.Find(Coordinate);
					if (!Chunk || !Chunk->AggregateBounds.Intersect(QueryBounds))
					{
						continue;
					}
					Scratch.StaticChunkCandidates.Add(Coordinate);
					if (!Chunk->bTreeDirty && Chunk->Entries.Num() > StaticLinearScanThreshold)
					{
						Chunk->Tree.Query(
							QueryBounds,
							Scratch.NodeStack,
							Scratch.LastVisitedStaticNodes,
							OutEntities);
					}
					else
					{
						for (const FSpatialEntry& Entry : Chunk->Entries)
						{
							++Scratch.LastVisitedStaticNodes;
							if (Entry.Bounds.Intersect(QueryBounds))
							{
								OutEntities.Add(Entry.Entity);
							}
						}
					}
				}
			}
		}
	}
	Impl->DynamicTree.Query(
		QueryBounds,
		Scratch.NodeStack,
		Scratch.LastVisitedDynamicNodes,
		OutEntities);
	Algo::Sort(OutEntities);
}

void FWorldObjectSpatialIndex::QueryPortableOverlaps(
	const FBox& QueryBounds,
	FWorldObjectSpatialQueryScratch& Scratch,
	TArray<FWorldObjectEntityHandle>& OutEntities) const
{
	OutEntities.Reset();
	Scratch.LastVisitedStaticNodes = 0;
	Scratch.LastVisitedDynamicNodes = 0;
	if (!Impl || !IsValidBounds(QueryBounds))
	{
		return;
	}
	Impl->DynamicTree.Query(
		QueryBounds,
		Scratch.NodeStack,
		Scratch.LastVisitedDynamicNodes,
		OutEntities);
	Algo::Sort(OutEntities);
}

void FWorldObjectSpatialIndex::QueryRay(
	const FVector& Origin,
	const FVector& UnitDirection,
	const double MaxDistance,
	FWorldObjectSpatialQueryScratch& Scratch,
	TArray<FWorldObjectSpatialRayHit>& OutHits) const
{
	OutHits.Reset();
	Scratch.LastVisitedStaticNodes = 0;
	Scratch.LastVisitedDynamicNodes = 0;
	if (!Impl || UnitDirection.IsNearlyZero() || MaxDistance < 0.0 || !FMath::IsFinite(MaxDistance))
	{
		return;
	}
	const FVector Direction = UnitDirection.GetSafeNormal();
	Scratch.StaticChunkCandidates.Reset();
	Scratch.VisitedStaticChunks.Reset();
	FIntVector Current = FIntVector::ZeroValue;
	if (Impl->TryGetStaticChunkCoordinate(Origin, Current))
	{
		const int32 NeighborRadius = FMath::CeilToInt(
			Impl->MaximumStaticAnchorOffset / Impl->Config.StaticChunkSize);
		auto VisitCoordinate = [&](const FIntVector& Coordinate)
		{
			if (Scratch.VisitedStaticChunks.Contains(Coordinate))
			{
				return;
			}
			Scratch.VisitedStaticChunks.Add(Coordinate);
			const FWorldObjectSpatialIndexImpl::FStaticChunk* Chunk = Impl->StaticChunks.Find(Coordinate);
			if (!Chunk)
			{
				return;
			}
			Scratch.StaticChunkCandidates.Add(Coordinate);
			if (!Chunk->bTreeDirty && Chunk->Entries.Num() > StaticLinearScanThreshold)
			{
				Chunk->Tree.QueryRay(
					Origin, Direction, MaxDistance, Scratch.NodeStack,
					Scratch.LastVisitedStaticNodes, OutHits);
				return;
			}
			for (const FSpatialEntry& Entry : Chunk->Entries)
			{
				++Scratch.LastVisitedStaticNodes;
				double Distance = 0.0;
				if (RayBoxEntry(Entry.Bounds, Origin, Direction, MaxDistance, Distance))
				{
					OutHits.Add({Entry.Entity, Distance});
				}
			}
		};

		const FIntVector Step(
			Direction.X > 0.0 ? 1 : (Direction.X < 0.0 ? -1 : 0),
			Direction.Y > 0.0 ? 1 : (Direction.Y < 0.0 ? -1 : 0),
			Direction.Z > 0.0 ? 1 : (Direction.Z < 0.0 ? -1 : 0));
		FVector TDelta(
			Step.X == 0 ? TNumericLimits<double>::Max() : Impl->Config.StaticChunkSize / FMath::Abs(Direction.X),
			Step.Y == 0 ? TNumericLimits<double>::Max() : Impl->Config.StaticChunkSize / FMath::Abs(Direction.Y),
			Step.Z == 0 ? TNumericLimits<double>::Max() : Impl->Config.StaticChunkSize / FMath::Abs(Direction.Z));
		auto NextBoundaryDistance = [&](const int32 Axis, const int32 Coordinate, const int32 AxisStep)
		{
			if (AxisStep == 0)
			{
				return TNumericLimits<double>::Max();
			}
			const double Boundary = (Coordinate + (AxisStep > 0 ? 1 : 0)) * Impl->Config.StaticChunkSize;
			return FMath::Max(0.0, (Boundary - Origin[Axis]) / Direction[Axis]);
		};
		FVector TMax(
			NextBoundaryDistance(0, Current.X, Step.X),
			NextBoundaryDistance(1, Current.Y, Step.Y),
			NextBoundaryDistance(2, Current.Z, Step.Z));
		double CurrentDistance = 0.0;
		while (CurrentDistance <= MaxDistance)
		{
			for (int32 X = Current.X - NeighborRadius; X <= Current.X + NeighborRadius; ++X)
			{
				for (int32 Y = Current.Y - NeighborRadius; Y <= Current.Y + NeighborRadius; ++Y)
				{
					for (int32 Z = Current.Z - NeighborRadius; Z <= Current.Z + NeighborRadius; ++Z)
					{
						VisitCoordinate(FIntVector(X, Y, Z));
					}
				}
			}
			if (TMax.X <= TMax.Y && TMax.X <= TMax.Z)
			{
				CurrentDistance = TMax.X;
				TMax.X += TDelta.X;
				Current.X += Step.X;
			}
			else if (TMax.Y <= TMax.Z)
			{
				CurrentDistance = TMax.Y;
				TMax.Y += TDelta.Y;
				Current.Y += Step.Y;
			}
			else
			{
				CurrentDistance = TMax.Z;
				TMax.Z += TDelta.Z;
				Current.Z += Step.Z;
			}
		}
	}
	Impl->DynamicTree.QueryRay(
		Origin,
		Direction,
		MaxDistance,
		Scratch.NodeStack,
		Scratch.LastVisitedDynamicNodes,
		OutHits);
	Algo::Sort(OutHits, [](const FWorldObjectSpatialRayHit& Left, const FWorldObjectSpatialRayHit& Right)
	{
		return Left.Distance != Right.Distance
			? Left.Distance < Right.Distance
			: Left.Entity < Right.Entity;
	});
}

void FWorldObjectSpatialIndex::QueryPortableRay(
	const FVector& Origin,
	const FVector& UnitDirection,
	const double MaxDistance,
	FWorldObjectSpatialQueryScratch& Scratch,
	TArray<FWorldObjectSpatialRayHit>& OutHits) const
{
	OutHits.Reset();
	Scratch.LastVisitedStaticNodes = 0;
	Scratch.LastVisitedDynamicNodes = 0;
	if (!Impl || UnitDirection.IsNearlyZero() || MaxDistance < 0.0)
	{
		return;
	}
	Impl->DynamicTree.QueryRay(
		Origin,
		UnitDirection.GetSafeNormal(),
		MaxDistance,
		Scratch.NodeStack,
		Scratch.LastVisitedDynamicNodes,
		OutHits);
	Algo::Sort(OutHits, [](const FWorldObjectSpatialRayHit& Left, const FWorldObjectSpatialRayHit& Right)
	{
		return Left.Distance != Right.Distance
			? Left.Distance < Right.Distance
			: Left.Entity < Right.Entity;
	});
}

bool FWorldObjectSpatialIndex::RebuildStaticIfDirty()
{
	return Impl && Impl->RebuildDirtyStaticChunks();
}

bool FWorldObjectSpatialIndex::IsStaticDirty() const
{
	return Impl && !Impl->DirtyStaticChunks.IsEmpty();
}

int32 FWorldObjectSpatialIndex::GetEntityCount() const
{
	return Impl ? Impl->StaticChunkByEntity.Num() + Impl->DynamicTree.Num() : 0;
}

int32 FWorldObjectSpatialIndex::GetPermanentStaticCount() const
{
	return Impl ? Impl->StaticChunkByEntity.Num() : 0;
}

int32 FWorldObjectSpatialIndex::GetPortableCount() const
{
	return Impl ? Impl->DynamicTree.Num() : 0;
}

uint64 FWorldObjectSpatialIndex::GetStaticBuildCount() const
{
	return Impl ? Impl->StaticBuildCount : 0;
}

int32 FWorldObjectSpatialIndex::GetStaticLinearChunkCount() const
{
	if (!Impl)
	{
		return 0;
	}
	int32 Count = 0;
	for (const TPair<FIntVector, FWorldObjectSpatialIndexImpl::FStaticChunk>& Pair : Impl->StaticChunks)
	{
		Count += Pair.Value.Entries.Num() <= StaticLinearScanThreshold ? 1 : 0;
	}
	return Count;
}

int32 FWorldObjectSpatialIndex::GetStaticBVHChunkCount() const
{
	if (!Impl)
	{
		return 0;
	}
	int32 Count = 0;
	for (const TPair<FIntVector, FWorldObjectSpatialIndexImpl::FStaticChunk>& Pair : Impl->StaticChunks)
	{
		Count += Pair.Value.Entries.Num() > StaticLinearScanThreshold && !Pair.Value.bTreeDirty ? 1 : 0;
	}
	return Count;
}

uint64 FWorldObjectSpatialIndex::GetDynamicReinsertCount() const
{
	return Impl ? Impl->DynamicTree.GetReinsertCount() : 0;
}

SIZE_T FWorldObjectSpatialIndex::GetEstimatedAllocatedSize() const
{
	return Impl ? Impl->GetAllocatedSize() : 0;
}

bool FWorldObjectSpatialIndex::ValidateDynamicTree() const
{
	return Impl && Impl->DynamicTree.Validate();
}
