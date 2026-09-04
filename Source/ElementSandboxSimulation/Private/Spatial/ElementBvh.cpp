#include "Spatial/ElementBvh.h"

#include "Algo/Sort.h"
#include "Async/Async.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	struct FElementBvhLeaf final
	{
		FElementSpatialSnapshotHandle Handle;
		FBox Bounds = FBox(ForceInit);
		int32 Node = INDEX_NONE;
		bool bAlive = false;
	};

	struct FNode final
	{
		FBox Bounds = FBox(ForceInit);
		int32 Parent = INDEX_NONE;
		int32 Left = INDEX_NONE;
		int32 Right = INDEX_NONE;
		int32 LeafIndex = INDEX_NONE;
		bool IsLeaf() const { return LeafIndex != INDEX_NONE; }
	};

	struct FElementBvhBuildResult final
	{
		TArray<FNode> Nodes;
		TArray<FElementBvhLeaf> Leaves;
		TArray<FNode> SnapshotNodes;
		TArray<FElementBvhLeaf> SnapshotLeaves;
		int32 Root = INDEX_NONE;
		uint64 MutationSerial = 0;
		double NormalizedCost = 1.0;
	};

	bool IsUsableBounds(const FBox& Bounds)
	{
		return Bounds.IsValid != 0 && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN();
	}

	uint32 AdvanceGeneration(const uint32 Generation)
	{
		return Generation == MAX_uint32 ? 1 : Generation + 1;
	}

	uint32 ExpandMortonBits(uint32 Value)
	{
		Value &= 0x000003ff;
		Value = (Value | (Value << 16)) & 0x030000ff;
		Value = (Value | (Value << 8)) & 0x0300f00f;
		Value = (Value | (Value << 4)) & 0x030c30c3;
		Value = (Value | (Value << 2)) & 0x09249249;
		return Value;
	}

	uint32 CalculateMortonCode(const FVector& Center, const FBox& CenterBounds)
	{
		const FVector Size = CenterBounds.GetSize();
		auto Quantize = [](const double Value, const double Minimum, const double Range)
		{
			if (Range <= UE_DOUBLE_SMALL_NUMBER) return uint32(0);
			return static_cast<uint32>(FMath::Clamp(
				FMath::FloorToInt(((Value - Minimum) / Range) * 1023.0), 0, 1023));
		};
		const uint32 X = Quantize(Center.X, CenterBounds.Min.X, Size.X);
		const uint32 Y = Quantize(Center.Y, CenterBounds.Min.Y, Size.Y);
		const uint32 Z = Quantize(Center.Z, CenterBounds.Min.Z, Size.Z);
		return ExpandMortonBits(X) | (ExpandMortonBits(Y) << 1) | (ExpandMortonBits(Z) << 2);
	}

	double SurfaceArea(const FBox& Bounds)
	{
		if (!IsUsableBounds(Bounds)) return 0.0;
		const FVector Size = Bounds.GetSize();
		return 2.0 * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
	}

	int32 BuildNode(
		TArray<FNode>& Nodes,
		TArray<FElementBvhLeaf>& Leaves,
		TArray<int32>& Indices,
		const int32 Begin,
		const int32 End,
		const int32 Parent)
	{
		const int32 NodeIndex = Nodes.AddDefaulted();
		Nodes[NodeIndex].Parent = Parent;
		FBox Bounds(ForceInit);
		for (int32 Cursor = Begin; Cursor < End; ++Cursor)
		{
			Bounds += Leaves[Indices[Cursor]].Bounds;
		}
		Nodes[NodeIndex].Bounds = Bounds;
		if (End - Begin == 1)
		{
			Nodes[NodeIndex].LeafIndex = Indices[Begin];
			Leaves[Indices[Begin]].Node = NodeIndex;
			return NodeIndex;
		}
		const int32 Middle = Begin + (End - Begin) / 2;
		const int32 Left = BuildNode(Nodes, Leaves, Indices, Begin, Middle, NodeIndex);
		const int32 Right = BuildNode(Nodes, Leaves, Indices, Middle, End, NodeIndex);
		Nodes[NodeIndex].Left = Left;
		Nodes[NodeIndex].Right = Right;
		return NodeIndex;
	}

	void BuildTree(TArray<FElementBvhLeaf>& Leaves, TArray<FNode>& Nodes, int32& Root)
	{
		Nodes.Reset();
		FBox CenterBounds(ForceInit);
		for (int32 Index = 0; Index < Leaves.Num(); ++Index)
		{
			Leaves[Index].Node = INDEX_NONE;
			if (Leaves[Index].bAlive) CenterBounds += Leaves[Index].Bounds.GetCenter();
		}
		struct FMortonLeaf final
		{
			uint32 Code = 0;
			int32 Index = INDEX_NONE;
		};
		TArray<FMortonLeaf> MortonLeaves;
		MortonLeaves.Reserve(Leaves.Num());
		for (int32 Index = 0; Index < Leaves.Num(); ++Index)
		{
			if (Leaves[Index].bAlive)
			{
				MortonLeaves.Add({CalculateMortonCode(Leaves[Index].Bounds.GetCenter(), CenterBounds), Index});
			}
		}
		Algo::Sort(MortonLeaves, [](const FMortonLeaf& Left, const FMortonLeaf& Right)
		{
			return Left.Code != Right.Code ? Left.Code < Right.Code : Left.Index < Right.Index;
		});
		TArray<int32> Indices;
		Indices.Reserve(MortonLeaves.Num());
		for (const FMortonLeaf& Leaf : MortonLeaves) Indices.Add(Leaf.Index);
		if (!Indices.IsEmpty()) Nodes.Reserve(Indices.Num() * 2 - 1);
		Root = Indices.IsEmpty()
			? INDEX_NONE : BuildNode(Nodes, Leaves, Indices, 0, Indices.Num(), INDEX_NONE);
	}

	double CalculateNormalizedCost(const TArray<FNode>& Nodes, const int32 Root)
	{
		if (!Nodes.IsValidIndex(Root)) return 1.0;
		const double RootArea = FMath::Max(SurfaceArea(Nodes[Root].Bounds), UE_DOUBLE_SMALL_NUMBER);
		double InternalArea = 0.0;
		for (const FNode& Node : Nodes)
		{
			if (!Node.IsLeaf()) InternalArea += SurfaceArea(Node.Bounds);
		}
		return FMath::Max(1.0, InternalArea / RootArea);
	}
}

class FElementBvhSnapshotData final
{
public:
	TArray<FNode> Nodes;
	TArray<FElementBvhLeaf> Leaves;
	int32 Root = INDEX_NONE;
};

class FElementBvhData final
{
public:
		void Rebuild()
		{
			BuildTree(Leaves, Nodes, Root);
			bTopologyDirty = false;
			bBoundsDirty = false;
			LastRebuildNormalizedCost = CalculateNormalizedCost(Nodes, Root);
			Stats.QualityRatio = 1.0;
			++Stats.RebuildCount;
		}

		bool ConsumeCompletedBackgroundRebuild()
		{
			if (!PendingBackgroundRebuild.IsValid() || !PendingBackgroundRebuild.IsReady()) return false;
			FElementBvhBuildResult Result = PendingBackgroundRebuild.Get();
			if (Result.MutationSerial != MutationSerial)
			{
				++Stats.BackgroundRebuildDiscardedCount;
				return false;
			}
			Leaves = MoveTemp(Result.Leaves);
			Nodes = MoveTemp(Result.Nodes);
			Root = Result.Root;
			PreparedSnapshotLeaves = MoveTemp(Result.SnapshotLeaves);
			PreparedSnapshotNodes = MoveTemp(Result.SnapshotNodes);
			PreparedSnapshotRoot = Result.Root;
			bHasPreparedSnapshot = true;
			bTopologyDirty = false;
			LastRebuildNormalizedCost = Result.NormalizedCost;
			Stats.QualityRatio = 1.0;
			bBoundsDirty = true;
			++Stats.BackgroundRebuildPublishedCount;
			++Stats.RebuildCount;
			return true;
		}

		void ScheduleBackgroundRebuild()
		{
			if (PendingBackgroundRebuild.IsValid()) return;
			TArray<FElementBvhLeaf> LeavesCopy = Leaves;
			const uint64 ScheduledSerial = MutationSerial;
			PendingBackgroundRebuild = Async(EAsyncExecution::LargeThreadPool,
				[Leaves = MoveTemp(LeavesCopy), ScheduledSerial]() mutable
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(ElementBvh_BackgroundRebuild);
					FElementBvhBuildResult Result;
					Result.Leaves = MoveTemp(Leaves);
					Result.MutationSerial = ScheduledSerial;
					BuildTree(Result.Leaves, Result.Nodes, Result.Root);
					Result.NormalizedCost = CalculateNormalizedCost(Result.Nodes, Result.Root);
					// 不可变 Snapshot 的大数组也在 Worker 复制；GameThread 只移动所有权。
					Result.SnapshotLeaves = Result.Leaves;
					Result.SnapshotNodes = Result.Nodes;
					return Result;
				});
			++Stats.BackgroundRebuildScheduledCount;
		}

	void RefitFrom(const int32 LeafNode)
	{
		int32 Node = LeafNode;
		while (Nodes.IsValidIndex(Node))
		{
			FNode& Current = Nodes[Node];
			if (Current.IsLeaf())
			{
				Current.Bounds = Leaves[Current.LeafIndex].Bounds;
			}
				else
				{
					Current.Bounds = Nodes[Current.Left].Bounds;
					Current.Bounds += Nodes[Current.Right].Bounds;
			}
			Node = Current.Parent;
		}
		++Stats.RefitCount;
	}

	TArray<FElementBvhLeaf> Leaves;
	TArray<FNode> Nodes;
	TArray<int32> FreeLeaves;
	int32 Root = INDEX_NONE;
		bool bTopologyDirty = false;
		bool bBoundsDirty = false;
		uint64 MutationSerial = 0;
		double LastRebuildNormalizedCost = 1.0;
		TFuture<FElementBvhBuildResult> PendingBackgroundRebuild;
		TArray<FElementBvhLeaf> PreparedSnapshotLeaves;
		TArray<FNode> PreparedSnapshotNodes;
		int32 PreparedSnapshotRoot = INDEX_NONE;
		bool bHasPreparedSnapshot = false;
	TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> Published;
	FElementBvhStats Stats;
};

FElementBvhSnapshot::FElementBvhSnapshot()
	: Data(MakeUnique<FElementBvhSnapshotData>())
{
}

FElementBvhSnapshot::~FElementBvhSnapshot() = default;
FElementBvhSnapshot::FElementBvhSnapshot(FElementBvhSnapshot&&) noexcept = default;
FElementBvhSnapshot& FElementBvhSnapshot::operator=(FElementBvhSnapshot&&) noexcept = default;

void FElementBvhSnapshot::Query(
	const FBox& Bounds,
	TArray<FElementSpatialSnapshotHandle>& OutHandles,
	FElementBvhStats* Stats) const
{
	OutHandles.Reset();
	if (!Data || !IsUsableBounds(Bounds) || !Data->Nodes.IsValidIndex(Data->Root)) return;
	if (Stats) ++Stats->QueryCount;
	TArray<int32, TInlineAllocator<64>> Stack;
	Stack.Add(Data->Root);
	while (!Stack.IsEmpty())
	{
		const int32 NodeIndex = Stack.Pop(EAllowShrinking::No);
		const FNode& Node = Data->Nodes[NodeIndex];
		if (Stats) ++Stats->NodeVisitCount;
		if (!Node.Bounds.Intersect(Bounds)) continue;
		if (Node.IsLeaf())
		{
			const FElementBvhLeaf& Leaf = Data->Leaves[Node.LeafIndex];
			if (Leaf.bAlive && Leaf.Bounds.Intersect(Bounds)) OutHandles.Add(Leaf.Handle);
		}
		else
		{
			Stack.Add(Node.Left);
			Stack.Add(Node.Right);
		}
	}
	OutHandles.Sort([](const FElementSpatialSnapshotHandle& Left, const FElementSpatialSnapshotHandle& Right)
	{
		return Left.Index != Right.Index ? Left.Index < Right.Index : Left.Generation < Right.Generation;
	});
	if (Stats) Stats->CandidateCount += OutHandles.Num();
}

int32 FElementBvhSnapshot::Num() const
{
	int32 Count = 0;
	if (Data) for (const FElementBvhLeaf& Leaf : Data->Leaves) Count += Leaf.bAlive ? 1 : 0;
	return Count;
}

FElementBvh::FElementBvh()
	: Data(MakeUnique<FElementBvhData>())
{
}

FElementBvh::~FElementBvh() = default;

void FElementBvh::Reserve(const int32 LeafCapacity)
{
	check(IsInGameThread());
	if (Data && LeafCapacity > 0) Data->Leaves.Reserve(LeafCapacity);
}

FElementSpatialSnapshotHandle FElementBvh::Insert(const FBox& Bounds)
{
	check(IsInGameThread());
	if (!Data || !IsUsableBounds(Bounds)) return {};
	int32 Index = INDEX_NONE;
	if (!Data->FreeLeaves.IsEmpty())
	{
		Index = Data->FreeLeaves.Pop(EAllowShrinking::No);
	}
	else
	{
		Index = Data->Leaves.AddDefaulted();
		Data->Leaves[Index].Handle.Index = Index;
		Data->Leaves[Index].Handle.Generation = 1;
	}
	FElementBvhLeaf& Leaf = Data->Leaves[Index];
	Leaf.Bounds = Bounds;
	Leaf.Node = INDEX_NONE;
	Leaf.bAlive = true;
	++Data->MutationSerial;
	Data->bTopologyDirty = true;
	return Leaf.Handle;
}

bool FElementBvh::Update(const FElementSpatialSnapshotHandle Handle, const FBox& Bounds)
{
	check(IsInGameThread());
	if (!IsAlive(Handle) || !IsUsableBounds(Bounds)) return false;
	FElementBvhLeaf& Leaf = Data->Leaves[Handle.Index];
	if (Leaf.Bounds.Min.Equals(Bounds.Min) && Leaf.Bounds.Max.Equals(Bounds.Max)) return true;
	Leaf.Bounds = Bounds;
	++Data->MutationSerial;
	if (!Data->bTopologyDirty && Data->Nodes.IsValidIndex(Leaf.Node)) Data->RefitFrom(Leaf.Node);
	Data->bBoundsDirty = true;
	return true;
}

bool FElementBvh::Remove(const FElementSpatialSnapshotHandle Handle)
{
	check(IsInGameThread());
	if (!IsAlive(Handle)) return false;
	FElementBvhLeaf& Leaf = Data->Leaves[Handle.Index];
	Leaf.bAlive = false;
	Leaf.Bounds = FBox(ForceInit);
	Leaf.Node = INDEX_NONE;
	Leaf.Handle.Generation = AdvanceGeneration(Leaf.Handle.Generation);
	Data->FreeLeaves.Add(Handle.Index);
	++Data->MutationSerial;
	Data->bTopologyDirty = true;
	return true;
}

bool FElementBvh::IsAlive(const FElementSpatialSnapshotHandle Handle) const
{
	return Data && Handle.IsSet() && Data->Leaves.IsValidIndex(Handle.Index)
		&& Data->Leaves[Handle.Index].bAlive
		&& Data->Leaves[Handle.Index].Handle.Generation == Handle.Generation;
}

bool FElementBvh::TryGetBounds(const FElementSpatialSnapshotHandle Handle, FBox& OutBounds) const
{
	if (!IsAlive(Handle))
	{
		OutBounds = FBox(ForceInit);
		return false;
	}
	OutBounds = Data->Leaves[Handle.Index].Bounds;
	return true;
}

bool FElementBvh::PublishSnapshot(const EElementBvhPublishMode Mode)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(ElementBvh_PublishSnapshot);
	if (!Data) return false;
	Data->ConsumeCompletedBackgroundRebuild();
	if (!Data->bTopologyDirty && !Data->bBoundsDirty) return false;
	constexpr int32 DeferredBuildLeafThreshold = 4096;
	const bool bDeferLargeBuild =
		Mode == EElementBvhPublishMode::DeferredLargeTopology
		&& Data->Leaves.Num() > DeferredBuildLeafThreshold
		&& !Data->bHasPreparedSnapshot;
	if (bDeferLargeBuild)
	{
		Data->ScheduleBackgroundRebuild();
		return false;
	}
	if (!Data->bHasPreparedSnapshot)
	{
		if (Data->bTopologyDirty) Data->Rebuild();
		else
		{
			const double CurrentCost = CalculateNormalizedCost(Data->Nodes, Data->Root);
			Data->Stats.QualityRatio = Data->LastRebuildNormalizedCost > UE_DOUBLE_SMALL_NUMBER
				? CurrentCost / Data->LastRebuildNormalizedCost : 1.0;
			if (Data->Stats.QualityRatio >= 1.5) Data->ScheduleBackgroundRebuild();
		}
	}
	TSharedRef<FElementBvhSnapshot, ESPMode::ThreadSafe> Snapshot = MakeShared<FElementBvhSnapshot, ESPMode::ThreadSafe>();
	if (Data->bHasPreparedSnapshot)
	{
		Snapshot->Data->Nodes = MoveTemp(Data->PreparedSnapshotNodes);
		Snapshot->Data->Leaves = MoveTemp(Data->PreparedSnapshotLeaves);
		Snapshot->Data->Root = Data->PreparedSnapshotRoot;
		Data->PreparedSnapshotRoot = INDEX_NONE;
		Data->bHasPreparedSnapshot = false;
	}
	else
	{
		Snapshot->Data->Nodes = Data->Nodes;
		Snapshot->Data->Leaves = Data->Leaves;
		Snapshot->Data->Root = Data->Root;
	}
	Data->Published = Snapshot;
	Data->bBoundsDirty = false;
	Data->Stats.LeafCount = Snapshot->Num();
	Data->Stats.NodeCount = Data->Nodes.Num();
	return true;
}

TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> FElementBvh::GetPublishedSnapshot() const
{
	return Data ? Data->Published : nullptr;
}

const FElementBvhStats& FElementBvh::GetStats() const
{
	check(Data);
	return Data->Stats;
}

SIZE_T FElementBvh::GetAllocatedSize() const
{
	return Data ? Data->Leaves.GetAllocatedSize() + Data->Nodes.GetAllocatedSize()
		+ Data->FreeLeaves.GetAllocatedSize() : 0;
}
