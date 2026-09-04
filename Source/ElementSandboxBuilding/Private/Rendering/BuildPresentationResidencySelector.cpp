#include "Rendering/BuildPresentationResidencySelector.h"

#include "Algo/Sort.h"

namespace
{
	using FSectorVisibility = FBuildPresentationSectorVisibility;

	FSectorVisibility EvaluateBounds(
		const FBox& Bounds,
		const FVector& ViewLocation,
		const FVector& SubjectLocation,
		const FVector2D& UnitForward,
		const double HalfAngleDegrees)
	{
		FSectorVisibility Result;
		if (!Bounds.IsValid)
		{
			return Result;
		}

		const FVector Center3D = Bounds.GetCenter();
		const FVector Extent3D = Bounds.GetExtent();
		const FVector2D Delta(Center3D.X - ViewLocation.X, Center3D.Y - ViewLocation.Y);
		const double CenterDistance = Delta.Size();
		const double HorizontalRadius = FMath::Sqrt(FMath::Square(Extent3D.X) + FMath::Square(Extent3D.Y));
		const FVector2D Right(-UnitForward.Y, UnitForward.X);
		const double LateralRadius =
			FMath::Abs(Right.X) * Extent3D.X + FMath::Abs(Right.Y) * Extent3D.Y;
		const double LateralGap = FMath::Max(
			0.0,
			FMath::Abs(FVector2D::DotProduct(Delta, Right)) - LateralRadius);
		const double SurfaceDistance = FMath::Sqrt(Bounds.ComputeSquaredDistanceToPoint(SubjectLocation));
		Result.Score = SurfaceDistance + LateralGap * 0.35;

		if (HalfAngleDegrees >= 179.999 || CenterDistance <= HorizontalRadius + UE_SMALL_NUMBER)
		{
			Result.bIntersects = true;
			Result.bFullyInside = HalfAngleDegrees >= 179.999;
			return Result;
		}
		const double CenterAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
			FVector2D::DotProduct(Delta / CenterDistance, UnitForward), -1.0, 1.0)));
		const double AngularRadius = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(
			HorizontalRadius / CenterDistance, 0.0, 1.0)));
		Result.bIntersects = CenterAngle <= HalfAngleDegrees + AngularRadius;
		Result.bFullyInside = Result.bIntersects && CenterAngle + AngularRadius <= HalfAngleDegrees;
		return Result;
	}

	bool EntityLess(const FBuildEntityHandle Left, const FBuildEntityHandle Right)
	{
		if (Left.GetRegistryId() != Right.GetRegistryId())
		{
			return Left.GetRegistryId() < Right.GetRegistryId();
		}
		if (Left.GetIndex() != Right.GetIndex())
		{
			return Left.GetIndex() < Right.GetIndex();
		}
		return Left.GetGeneration() < Right.GetGeneration();
	}

	template <typename RequestType>
	bool IsStaticEntryTombstoned(
		const RequestType& Request,
		const FBuildPresentationSelectorEntry& Entry)
	{
		return Entry.StaticSnapshotSerial != INDEX_NONE && Request.StaticEntryTombstones.IsValid() &&
			Request.StaticEntryTombstones->IsValidIndex(Entry.StaticSnapshotSerial) &&
			(*Request.StaticEntryTombstones)[Entry.StaticSnapshotSerial];
	}

		enum class ECandidateKind : uint8
		{
			StaticNode,
			StaticEntry,
			StaticDeltaBlockEntry,
			DeltaEntry,
		MutableEntry
	};

	struct FCandidate final
	{
		ECandidateKind Kind = ECandidateKind::StaticNode;
		int32 OwnerIndex = INDEX_NONE;
		int32 ItemIndex = INDEX_NONE;
		double Score = 0.0;
	};

	struct FCandidateMinHeap final
	{
		bool operator()(const FCandidate& Left, const FCandidate& Right) const
		{
			if (!FMath::IsNearlyEqual(Left.Score, Right.Score, UE_DOUBLE_SMALL_NUMBER))
			{
				return Left.Score < Right.Score;
			}
			if (Left.Kind != Right.Kind)
			{
				return static_cast<uint8>(Left.Kind) < static_cast<uint8>(Right.Kind);
			}
			if (Left.OwnerIndex != Right.OwnerIndex)
			{
				return Left.OwnerIndex < Right.OwnerIndex;
			}
			return Left.ItemIndex < Right.ItemIndex;
		}
	};

	void SelectLocalEntries(
		const FBuildLocalSelectionRequest& Request,
		TSet<FBuildEntityHandle>& Selected,
		FBuildLocalSelectionResult& Result)
	{
		TArray<FCandidate> Heap;
		const auto Push = [&Heap](const FCandidate& Candidate)
		{
			Heap.HeapPush(Candidate, FCandidateMinHeap());
		};
		for (int32 CellIndex = 0; CellIndex < Request.StaticCells.Num(); ++CellIndex)
		{
			const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>& Cell =
				Request.StaticCells[CellIndex];
			if (Cell.IsValid() && !Cell->Nodes.IsEmpty())
			{
				Push({
					ECandidateKind::StaticNode,
					CellIndex,
					0,
					Cell->Nodes[0].Bounds.ComputeSquaredDistanceToPoint(Request.SubjectLocation)});
			}
		}

		const double RequiredRadiusSquared =
			FMath::Square(FMath::Max(Request.MinimumLocalRadius, Request.HotPromotionRadius));
			const double HotRadiusSquared = FMath::Square(Request.HotPromotionRadius);
			const auto CanExplore = [&Request, &Result, RequiredRadiusSquared](const int32 MinimumCost, const double Score)
			{
				return Score <= RequiredRadiusSquared
					|| (Result.TargetMeshPartCost < Request.TargetLocalMeshParts && MinimumCost > 0
						&& MinimumCost <= Request.TargetLocalMeshParts - Result.TargetMeshPartCost);
			};
			for (int32 BlockIndex = 0; BlockIndex < Request.StaticDeltaBlocks.Num(); ++BlockIndex)
			{
				const TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>& Block =
					Request.StaticDeltaBlocks[BlockIndex];
				if (!Block.IsValid())
				{
					continue;
				}
				for (int32 EntryIndex = 0; EntryIndex < Block->Num(); ++EntryIndex)
				{
					const FBuildPresentationSelectorEntry& Entry = (*Block)[EntryIndex];
					if (IsStaticEntryTombstoned(Request, Entry))
					{
						continue;
					}
					const double Score = Entry.Bounds.ComputeSquaredDistanceToPoint(Request.SubjectLocation);
					if (CanExplore(Entry.MeshPartCost, Score))
					{
						Push({ECandidateKind::StaticDeltaBlockEntry, BlockIndex, EntryIndex, Score});
					}
					else
					{
						++Result.PrunedNodeCount;
					}
				}
			}
			for (int32 Index = 0; Index < Request.DeltaEntries.Num(); ++Index)
		{
			const FBuildPresentationSelectorEntry& Entry = Request.DeltaEntries[Index];
			const double Score = Entry.Bounds.ComputeSquaredDistanceToPoint(Request.SubjectLocation);
			if (CanExplore(Entry.MeshPartCost, Score))
			{
				Push({ECandidateKind::DeltaEntry, INDEX_NONE, Index, Score});
			}
			else
			{
				++Result.PrunedNodeCount;
			}
		}
		for (int32 Index = 0; Index < Request.MutableEntries.Num(); ++Index)
		{
			const FBuildPresentationSelectorEntry& Entry = Request.MutableEntries[Index];
			const double Score = Entry.Bounds.ComputeSquaredDistanceToPoint(Request.SubjectLocation);
			if (CanExplore(Entry.MeshPartCost, Score))
			{
				Push({ECandidateKind::MutableEntry, INDEX_NONE, Index, Score});
			}
			else
			{
				++Result.PrunedNodeCount;
			}
		}

		const auto TryAccept =
			[&Request, &Selected, &Result, RequiredRadiusSquared, HotRadiusSquared](
				const FBuildPresentationSelectorEntry& Entry,
				const double Score)
		{
			++Result.CandidateEntryCount;
			if (!Entry.Entity.IsSet() || Entry.MeshPartCost <= 0 || IsStaticEntryTombstoned(Request, Entry) ||
				Selected.Contains(Entry.Entity))
			{
				return;
			}
			if (Score <= HotRadiusSquared)
			{
				Result.HotPinnedEntities.Add(Entry.Entity);
			}
			const bool bMandatory = Score <= RequiredRadiusSquared;
			if (!bMandatory && Entry.MeshPartCost > Request.TargetLocalMeshParts - Result.TargetMeshPartCost)
			{
				return;
			}
			Selected.Add(Entry.Entity);
			Result.OrderedTargetEntities.Add(Entry.Entity);
			Result.TargetMeshPartCost += Entry.MeshPartCost;
			Result.Boundary = FMath::Max(Result.Boundary, FMath::Sqrt(Score));
		};

		while (!Heap.IsEmpty())
		{
			if (Result.TargetMeshPartCost >= Request.TargetLocalMeshParts && Heap[0].Score > RequiredRadiusSquared)
			{
				break;
			}
				FCandidate Candidate;
				Heap.HeapPop(Candidate, FCandidateMinHeap(), EAllowShrinking::No);
				if (Candidate.Kind == ECandidateKind::StaticDeltaBlockEntry)
				{
					if (Request.StaticDeltaBlocks.IsValidIndex(Candidate.OwnerIndex))
					{
						const TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>& Block =
							Request.StaticDeltaBlocks[Candidate.OwnerIndex];
						if (Block.IsValid() && Block->IsValidIndex(Candidate.ItemIndex))
						{
							TryAccept((*Block)[Candidate.ItemIndex], Candidate.Score);
						}
					}
					continue;
				}
				if (Candidate.Kind == ECandidateKind::DeltaEntry)
			{
				if (Request.DeltaEntries.IsValidIndex(Candidate.ItemIndex))
				{
					TryAccept(Request.DeltaEntries[Candidate.ItemIndex], Candidate.Score);
				}
				continue;
			}
			if (Candidate.Kind == ECandidateKind::MutableEntry)
			{
				if (Request.MutableEntries.IsValidIndex(Candidate.ItemIndex))
				{
					TryAccept(Request.MutableEntries[Candidate.ItemIndex], Candidate.Score);
				}
				continue;
			}
			if (Candidate.Kind == ECandidateKind::StaticEntry)
			{
				if (Request.StaticCells.IsValidIndex(Candidate.OwnerIndex))
				{
					const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>& Cell =
						Request.StaticCells[Candidate.OwnerIndex];
					if (Cell.IsValid() && Cell->OrderedEntries.IsValidIndex(Candidate.ItemIndex))
					{
						TryAccept(Cell->OrderedEntries[Candidate.ItemIndex], Candidate.Score);
					}
				}
				continue;
			}

			++Result.CandidateNodeCount;
			if (!Request.StaticCells.IsValidIndex(Candidate.OwnerIndex))
			{
				continue;
			}
			const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>& Cell =
				Request.StaticCells[Candidate.OwnerIndex];
			if (!Cell.IsValid() || !Cell->Nodes.IsValidIndex(Candidate.ItemIndex))
			{
				continue;
			}
			const FBuildPresentationSelectorNode& Node = Cell->Nodes[Candidate.ItemIndex];
			if (!CanExplore(Node.MinimumMeshPartCost, Candidate.Score))
			{
				++Result.PrunedNodeCount;
				continue;
			}
			if (Node.IsLeaf())
			{
				for (int32 EntryIndex = Node.FirstEntry;
					EntryIndex < Node.FirstEntry + Node.EntryCount;
					++EntryIndex)
				{
					if (!Cell->OrderedEntries.IsValidIndex(EntryIndex))
					{
						continue;
					}
					const FBuildPresentationSelectorEntry& Entry = Cell->OrderedEntries[EntryIndex];
					if (IsStaticEntryTombstoned(Request, Entry))
					{
						continue;
					}
					const double Score = Entry.Bounds.ComputeSquaredDistanceToPoint(Request.SubjectLocation);
					if (CanExplore(Entry.MeshPartCost, Score))
					{
						Push({ECandidateKind::StaticEntry, Candidate.OwnerIndex, EntryIndex, Score});
					}
					else
					{
						++Result.PrunedNodeCount;
					}
				}
				continue;
			}
			for (const int32 ChildIndex : {Node.LeftChild, Node.RightChild})
			{
				if (!Cell->Nodes.IsValidIndex(ChildIndex))
				{
					continue;
				}
				const FBuildPresentationSelectorNode& Child = Cell->Nodes[ChildIndex];
				const double Score = Child.Bounds.ComputeSquaredDistanceToPoint(Request.SubjectLocation);
				if (CanExplore(Child.MinimumMeshPartCost, Score))
				{
					Push({ECandidateKind::StaticNode, Candidate.OwnerIndex, ChildIndex, Score});
				}
				else
				{
					++Result.PrunedNodeCount;
				}
			}
		}
	}

	void SelectSector(
		const FBuildFarSelectionRequest& Request,
		const double HalfAngleDegrees,
		TSet<FBuildEntityHandle>& Selected,
		FBuildFarSelectionResult& Result)
	{
		if (Result.TargetMeshPartCost >= Request.TargetFarMeshParts)
		{
			return;
		}

		TArray<FCandidate> Heap;
		const auto Push = [&Heap](const FCandidate& Candidate)
		{
			Heap.HeapPush(Candidate, FCandidateMinHeap());
		};
		for (int32 CellIndex = 0; CellIndex < Request.StaticCells.Num(); ++CellIndex)
		{
			const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>& Cell =
				Request.StaticCells[CellIndex];
			if (!Cell.IsValid() || Cell->Nodes.IsEmpty())
			{
				continue;
			}
			const FSectorVisibility Visibility = EvaluateBounds(
				Cell->Nodes[0].Bounds,
				Request.ViewLocation,
				Request.SubjectLocation,
				Request.Forward,
				HalfAngleDegrees);
			if (Visibility.bIntersects)
			{
				Push({ECandidateKind::StaticNode, CellIndex, 0, Visibility.Score});
			}
				else
				{
					++Result.PrunedNodeCount;
				}
			}
			for (int32 BlockIndex = 0; BlockIndex < Request.StaticDeltaBlocks.Num(); ++BlockIndex)
			{
			const TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>& Block =
				Request.StaticDeltaBlocks[BlockIndex];
			if (!Block.IsValid())
			{
				continue;
			}
				for (int32 EntryIndex = 0; EntryIndex < Block->Num(); ++EntryIndex)
				{
					const FBuildPresentationSelectorEntry& Entry = (*Block)[EntryIndex];
					if (IsStaticEntryTombstoned(Request, Entry))
					{
						continue;
					}
					const FSectorVisibility Visibility = EvaluateBounds(
						Entry.Bounds,
						Request.ViewLocation,
						Request.SubjectLocation,
						Request.Forward,
						HalfAngleDegrees);
					if (Visibility.bIntersects)
					{
						Push({
							ECandidateKind::StaticDeltaBlockEntry,
							BlockIndex,
							EntryIndex,
							Visibility.Score});
					}
			}
		}
		for (int32 Index = 0; Index < Request.DeltaEntries.Num(); ++Index)
		{
			const FSectorVisibility Visibility = EvaluateBounds(
				Request.DeltaEntries[Index].Bounds,
				Request.ViewLocation,
				Request.SubjectLocation,
				Request.Forward,
				HalfAngleDegrees);
			if (Visibility.bIntersects)
			{
				Push({ECandidateKind::DeltaEntry, INDEX_NONE, Index, Visibility.Score});
			}
		}
		for (int32 Index = 0; Index < Request.MutableEntries.Num(); ++Index)
		{
			const FSectorVisibility Visibility = EvaluateBounds(
				Request.MutableEntries[Index].Bounds,
				Request.ViewLocation,
				Request.SubjectLocation,
				Request.Forward,
				HalfAngleDegrees);
			if (Visibility.bIntersects)
			{
				Push({ECandidateKind::MutableEntry, INDEX_NONE, Index, Visibility.Score});
			}
		}

		const auto TryAccept = [&Request, &Selected, &Result](
			const FBuildPresentationSelectorEntry& Entry,
			const double Score)
		{
			++Result.CandidateEntryCount;
				if (!Entry.Entity.IsSet() || Entry.MeshPartCost <= 0 || IsStaticEntryTombstoned(Request, Entry)
					|| (Request.LocalExclusions.IsValid()
						&& Request.LocalExclusions->Contains(Entry.Entity))
					|| Selected.Contains(Entry.Entity))
			{
				return;
			}
			const int32 Remaining = Request.TargetFarMeshParts - Result.TargetMeshPartCost;
			if (Entry.MeshPartCost > Remaining)
			{
				return;
			}
			Selected.Add(Entry.Entity);
			Result.OrderedTargetEntries.Add(Entry);
			Result.TargetMeshPartCost += Entry.MeshPartCost;
			Result.BoundaryScore = FMath::Max(Result.BoundaryScore, Score);
		};

		while (!Heap.IsEmpty() && Result.TargetMeshPartCost < Request.TargetFarMeshParts)
		{
				FCandidate Candidate;
				Heap.HeapPop(Candidate, FCandidateMinHeap(), EAllowShrinking::No);
				if (Candidate.Kind == ECandidateKind::StaticDeltaBlockEntry)
				{
					if (Request.StaticDeltaBlocks.IsValidIndex(Candidate.OwnerIndex))
					{
						const TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>& Block =
							Request.StaticDeltaBlocks[Candidate.OwnerIndex];
						if (Block.IsValid() && Block->IsValidIndex(Candidate.ItemIndex))
						{
							TryAccept((*Block)[Candidate.ItemIndex], Candidate.Score);
						}
					}
					continue;
				}
				if (Candidate.Kind == ECandidateKind::DeltaEntry)
			{
				if (Request.DeltaEntries.IsValidIndex(Candidate.ItemIndex))
				{
					TryAccept(Request.DeltaEntries[Candidate.ItemIndex], Candidate.Score);
				}
				continue;
			}
			if (Candidate.Kind == ECandidateKind::MutableEntry)
			{
				if (Request.MutableEntries.IsValidIndex(Candidate.ItemIndex))
				{
					TryAccept(Request.MutableEntries[Candidate.ItemIndex], Candidate.Score);
				}
				continue;
			}
			if (Candidate.Kind == ECandidateKind::StaticEntry)
			{
				if (Request.StaticCells.IsValidIndex(Candidate.OwnerIndex))
				{
					const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>& Cell =
						Request.StaticCells[Candidate.OwnerIndex];
					if (Cell.IsValid() && Cell->OrderedEntries.IsValidIndex(Candidate.ItemIndex))
					{
						TryAccept(Cell->OrderedEntries[Candidate.ItemIndex], Candidate.Score);
					}
				}
				continue;
			}

			++Result.CandidateNodeCount;
			if (!Request.StaticCells.IsValidIndex(Candidate.OwnerIndex))
			{
				continue;
			}
			const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>& Cell =
				Request.StaticCells[Candidate.OwnerIndex];
			if (!Cell.IsValid() || !Cell->Nodes.IsValidIndex(Candidate.ItemIndex))
			{
				continue;
			}
				const FBuildPresentationSelectorNode& Node = Cell->Nodes[Candidate.ItemIndex];
				const int32 RemainingMeshParts = Request.TargetFarMeshParts - Result.TargetMeshPartCost;
				if (Node.MinimumMeshPartCost <= 0 || Node.MinimumMeshPartCost > RemainingMeshParts)
				{
					++Result.PrunedNodeCount;
					continue;
				}
				const FSectorVisibility NodeVisibility = EvaluateBounds(
				Node.Bounds,
				Request.ViewLocation,
				Request.SubjectLocation,
				Request.Forward,
				HalfAngleDegrees);
			if (NodeVisibility.bFullyInside
				&& Node.MeshPartCost <= Request.TargetFarMeshParts - Result.TargetMeshPartCost)
			{
				for (int32 EntryIndex = Node.FirstEntry;
					EntryIndex < Node.FirstEntry + Node.EntryCount;
					++EntryIndex)
				{
					if (Cell->OrderedEntries.IsValidIndex(EntryIndex))
					{
							const FBuildPresentationSelectorEntry& Entry = Cell->OrderedEntries[EntryIndex];
							TryAccept(
								Entry,
								EvaluateBounds(
									Entry.Bounds,
									Request.ViewLocation,
									Request.SubjectLocation,
									Request.Forward,
									HalfAngleDegrees).Score);
					}
				}
				++Result.AcceptedSubtreeCount;
				continue;
			}
			if (Node.IsLeaf())
			{
				for (int32 EntryIndex = Node.FirstEntry;
					EntryIndex < Node.FirstEntry + Node.EntryCount;
					++EntryIndex)
				{
					if (!Cell->OrderedEntries.IsValidIndex(EntryIndex))
					{
						continue;
					}
					const FBuildPresentationSelectorEntry& Entry = Cell->OrderedEntries[EntryIndex];
					if (IsStaticEntryTombstoned(Request, Entry))
					{
						continue;
					}
					const FSectorVisibility Visibility = EvaluateBounds(
						Entry.Bounds,
						Request.ViewLocation,
						Request.SubjectLocation,
						Request.Forward,
						HalfAngleDegrees);
					if (Visibility.bIntersects)
					{
						Push({ECandidateKind::StaticEntry, Candidate.OwnerIndex, EntryIndex, Visibility.Score});
					}
				}
				continue;
			}

			for (const int32 ChildIndex : {Node.LeftChild, Node.RightChild})
			{
				if (!Cell->Nodes.IsValidIndex(ChildIndex))
				{
					continue;
				}
				const FBuildPresentationSelectorNode& Child = Cell->Nodes[ChildIndex];
				const FSectorVisibility Visibility = EvaluateBounds(
					Child.Bounds,
					Request.ViewLocation,
					Request.SubjectLocation,
					Request.Forward,
					HalfAngleDegrees);
				if (Visibility.bIntersects)
				{
					Push({ECandidateKind::StaticNode, Candidate.OwnerIndex, ChildIndex, Visibility.Score});
				}
				else
				{
					++Result.PrunedNodeCount;
				}
			}
		}
	}
}

FBuildPresentationSectorVisibility FBuildPresentationResidencySelector::EvaluateSectorBounds(
	const FBox& Bounds,
	const FVector& ViewLocation,
	const FVector& SubjectLocation,
	const FVector2D& UnitForward,
	const double HalfAngleDegrees)
{
	return EvaluateBounds(Bounds, ViewLocation, SubjectLocation, UnitForward, HalfAngleDegrees);
}

FBuildLocalSelectionResult FBuildPresentationResidencySelector::SelectLocal(FBuildLocalSelectionRequest&& Request)
{
	FBuildLocalSelectionResult Result;
	Result.SourceToken = Request.SourceToken;
	Result.RequestId = Request.RequestId;
	Result.SourceRevision = Request.SourceRevision;
	Result.IndexChangeSerial = Request.IndexChangeSerial;
	Result.SubjectLocation = Request.SubjectLocation;
	if (Request.TargetLocalMeshParts <= 0)
	{
		return Result;
	}

	TSet<FBuildEntityHandle> Selected;
	SelectLocalEntries(Request, Selected, Result);
	Result.TargetEntities = MoveTemp(Selected);
	Result.TargetEntitiesSnapshot =
		MakeShared<TSet<FBuildEntityHandle>, ESPMode::ThreadSafe>(Result.TargetEntities);
	return Result;
}

FBuildFarSelectionResult FBuildPresentationResidencySelector::Select(FBuildFarSelectionRequest&& Request)
{
	FBuildFarSelectionResult Result;
	Result.SourceToken = Request.SourceToken;
	Result.RequestId = Request.RequestId;
	Result.SourceRevision = Request.SourceRevision;
	Result.IndexChangeSerial = Request.IndexChangeSerial;
	Result.ViewLocation = Request.ViewLocation;
	Result.SubjectLocation = Request.SubjectLocation;
	Result.HorizontalFOVDegrees = Request.HorizontalFOVDegrees;
	Result.CoverageAngleDegrees = Request.CoverageAngleDegrees;
	Result.RequestedMeshPartCost = Request.TargetFarMeshParts;
	Request.Forward = Request.Forward.GetSafeNormal();
	if (Request.Forward.IsNearlyZero())
	{
		Request.Forward = FVector2D(1.0, 0.0);
	}
	Result.Forward = Request.Forward;
	if (Request.TargetFarMeshParts <= 0)
	{
		if (Request.ExistingTransitionEntries.IsValid())
		{
			Result.SupersededTransitionEntities.Reserve(Request.ExistingTransitionEntries->Num());
			for (const FBuildPresentationSelectorEntry& Entry : *Request.ExistingTransitionEntries)
			{
				if (Entry.Entity.IsSet())
				{
					Result.SupersededTransitionEntities.Add(Entry.Entity);
				}
			}
		}
		return Result;
	}

	TSet<FBuildEntityHandle> Selected;
	const double CoreHalfAngle = FMath::Min(
		Request.CoverageAngleDegrees * 0.5,
		Request.HorizontalFOVDegrees * 0.5 + Request.FOVSafetyAngleDegrees);
	SelectSector(Request, CoreHalfAngle, Selected, Result);
	Result.VisibleCoreEntryCount = Result.OrderedTargetEntries.Num();
	Result.VisibleCoreMeshPartCost = Result.TargetMeshPartCost;
	SelectSector(Request, Request.CoverageAngleDegrees * 0.5, Selected, Result);
	if (Request.ExistingTransitionEntries.IsValid())
	{
		Result.SupersededTransitionEntities.Reserve(Request.ExistingTransitionEntries->Num());
		for (const FBuildPresentationSelectorEntry& Entry : *Request.ExistingTransitionEntries)
		{
			if (Entry.Entity.IsSet() && !Selected.Contains(Entry.Entity))
			{
				Result.SupersededTransitionEntities.Add(Entry.Entity);
			}
		}
	}
	Result.OrderedTargetEntriesSnapshot =
		MakeShared<TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>(Result.OrderedTargetEntries);

	struct FReclaimCandidate final
	{
		FBuildEntityHandle Entity;
		double MinimumSurfaceDistanceSquared = TNumericLimits<double>::Max();
		double LastRequiredTimeSeconds = 0.0;
		};
		TArray<FReclaimCandidate> ReclaimCandidates;
		if (Request.ExistingActiveEntries.IsValid())
		{
			for (const FBuildPresentationSelectorEntry& Entry : *Request.ExistingActiveEntries)
			{
				if (!Entry.Entity.IsSet() || Selected.Contains(Entry.Entity)
					|| (Request.LocalExclusions.IsValid()
						&& Request.LocalExclusions->Contains(Entry.Entity)))
				{
					continue;
				}
				double MinimumDistanceSquared = TNumericLimits<double>::Max();
				for (const FVector& Subject : Request.AllSubjectLocations)
				{
					MinimumDistanceSquared = FMath::Min(
						MinimumDistanceSquared,
						Entry.Bounds.ComputeSquaredDistanceToPoint(Subject));
				}
				ReclaimCandidates.Add({Entry.Entity, MinimumDistanceSquared, Entry.LastRequiredTimeSeconds});
			}
		}
	ReclaimCandidates.Sort([](const FReclaimCandidate& Left, const FReclaimCandidate& Right)
	{
		if (Left.MinimumSurfaceDistanceSquared != Right.MinimumSurfaceDistanceSquared)
		{
			return Left.MinimumSurfaceDistanceSquared > Right.MinimumSurfaceDistanceSquared;
		}
		if (Left.LastRequiredTimeSeconds != Right.LastRequiredTimeSeconds)
		{
			return Left.LastRequiredTimeSeconds < Right.LastRequiredTimeSeconds;
		}
		return EntityLess(Left.Entity, Right.Entity);
	});
	Result.ReclaimOrder.Reserve(ReclaimCandidates.Num());
	for (const FReclaimCandidate& Candidate : ReclaimCandidates)
	{
		Result.ReclaimOrder.Add(Candidate.Entity);
	}
	Result.TargetEntities = MoveTemp(Selected);
	return Result;
}
