#include "MeteorSettlementQueue.h"

namespace UE::ElementSandbox::Meteor
{
	bool FMeteorSettlementLane::IsValid() const
	{
		return Key.IsSet() && WorldEntityId.IsSet() && !ProductDefinitionId.IsNone()
			&& !WorldTransform.ContainsNaN()
			&& WorldTransform.GetScale3D().GetAbs().GetMin() > UE_SMALL_NUMBER
			&& FMath::IsFinite(DueTimeSeconds) && DueTimeSeconds >= 0.0;
	}

	bool FMeteorSettlementQueue::Initialize(
		const FMeteorBurstId InBurstId,
		const float InCellSize,
		const float InPriorityRadius)
	{
		Reset();
		if (!InBurstId.IsSet() || !FMath::IsFinite(InCellSize) || InCellSize <= 0.0f
			|| !FMath::IsFinite(InPriorityRadius) || InPriorityRadius < InCellSize)
		{
			return false;
		}
		BurstId = InBurstId;
		CellSize = InCellSize;
		PriorityRadius = InPriorityRadius;
		return true;
	}

	void FMeteorSettlementQueue::Reset()
	{
		BurstId = {};
		CellSize = 0.0f;
		PriorityRadius = 0.0f;
		OutstandingLaneCount = 0;
		LanesByOrdinal.Reset();
		GlobalDueHeap.Reset();
		Cells.Reset();
	}

	bool FMeteorSettlementQueue::Enqueue(FMeteorSettlementLane Lane)
	{
		if (!BurstId.IsSet() || !Lane.IsValid() || Lane.Key.BurstId != BurstId)
		{
			return false;
		}
		const uint32 Ordinal = Lane.Key.DebrisOrdinal;
		if (Ordinal > static_cast<uint32>(MAX_int32 - 1))
		{
			return false;
		}
		if (LanesByOrdinal.Num() <= static_cast<int32>(Ordinal))
		{
			LanesByOrdinal.SetNum(static_cast<int32>(Ordinal) + 1, EAllowShrinking::No);
		}
		FStoredLane& Stored = LanesByOrdinal[Ordinal];
		if (Stored.State != ELaneState::Missing)
		{
			return false;
		}
		Stored.Cell = MakeCell(Lane.WorldTransform.GetLocation());
		Stored.Lane = MoveTemp(Lane);
		Stored.State = ELaneState::Pending;
		const FQueueToken Token{Stored.Lane.DueTimeSeconds, Ordinal};
		TokenHeapPush(GlobalDueHeap, Token);
		TokenHeapPush(Cells.FindOrAdd(Stored.Cell).DueHeap, Token);
		++OutstandingLaneCount;
		return true;
	}

	int32 FMeteorSettlementQueue::ReserveDue(
		const TConstArrayView<FVector> AuthorityPlayerLocations,
		const double NowSeconds,
		const int32 MaximumCount,
		const int32 MinimumGlobalOldestCount,
		TArray<FMeteorSettlementReservation>& OutReservations)
	{
		OutReservations.Reset();
		if (!BurstId.IsSet() || !FMath::IsFinite(NowSeconds) || MaximumCount <= 0
			|| OutstandingLaneCount <= 0)
		{
			return 0;
		}

		const int32 GlobalReservation = FMath::Clamp(
			MinimumGlobalOldestCount, 0, MaximumCount);
		const int32 NearbyReservation = MaximumCount - GlobalReservation;
		TArray<FCellCandidate> NearbyCandidates;
		BuildNearbyCellCandidates(AuthorityPlayerLocations, NowSeconds, NearbyCandidates);
		ReserveNearby(
			NearbyCandidates, NowSeconds, NearbyReservation, OutReservations);

		// 近场没有用满的配额允许全局最老队列接管；反过来全局不足时，近场也可吃满整批。
		ReserveGlobalOldest(
			NowSeconds, MaximumCount - OutReservations.Num(), OutReservations);
		ReserveNearby(
			NearbyCandidates, NowSeconds, MaximumCount - OutReservations.Num(), OutReservations);
		return OutReservations.Num();
	}

	const FMeteorSettlementLane* FMeteorSettlementQueue::FindReserved(
		const FMeteorSettlementReservation& Reservation) const
	{
		return Reservation.IsSet()
			&& LanesByOrdinal.IsValidIndex(static_cast<int32>(Reservation.Ordinal))
			&& LanesByOrdinal[Reservation.Ordinal].State == ELaneState::Reserved
			? &LanesByOrdinal[Reservation.Ordinal].Lane : nullptr;
	}

	bool FMeteorSettlementQueue::CommitReserved(
		const FMeteorSettlementReservation& Reservation)
	{
		if (!FindReserved(Reservation))
		{
			return false;
		}
		LanesByOrdinal[Reservation.Ordinal].State = ELaneState::Settled;
		OutstandingLaneCount = FMath::Max(0, OutstandingLaneCount - 1);
		return true;
	}

	bool FMeteorSettlementQueue::RollbackReserved(
		const FMeteorSettlementReservation& Reservation)
	{
		const FMeteorSettlementLane* Lane = FindReserved(Reservation);
		if (!Lane)
		{
			return false;
		}
		FStoredLane& Stored = LanesByOrdinal[Reservation.Ordinal];
		Stored.State = ELaneState::Pending;
		const FQueueToken Token{Lane->DueTimeSeconds, Reservation.Ordinal};
		// Reserve 期间另一索引里的同一 Token 可能已经被懒清理。回滚时同时恢复
		// 两个索引；尚未被清理的重复 Token 会在下一次 Reserve 后按 State 淘汰。
		TokenHeapPush(Cells.FindOrAdd(Stored.Cell).DueHeap, Token);
		TokenHeapPush(GlobalDueHeap, Token);
		return true;
	}

	bool FMeteorSettlementQueue::IsTokenLess(
		const FQueueToken& Left,
		const FQueueToken& Right)
	{
		return Left.DueTimeSeconds != Right.DueTimeSeconds
			? Left.DueTimeSeconds < Right.DueTimeSeconds
			: Left.Ordinal < Right.Ordinal;
	}

	void FMeteorSettlementQueue::TokenHeapPush(
		TArray<FQueueToken>& Heap,
		FQueueToken Token)
	{
		int32 Index = Heap.Add(MoveTemp(Token));
		while (Index > 0)
		{
			const int32 Parent = (Index - 1) / 2;
			if (!IsTokenLess(Heap[Index], Heap[Parent])) break;
			Swap(Heap[Index], Heap[Parent]);
			Index = Parent;
		}
	}

	bool FMeteorSettlementQueue::TokenHeapPop(
		TArray<FQueueToken>& Heap,
		FQueueToken& OutToken)
	{
		if (Heap.IsEmpty()) return false;
		OutToken = Heap[0];
		if (Heap.Num() == 1)
		{
			Heap.Pop(EAllowShrinking::No);
			return true;
		}
		Heap[0] = Heap.Last();
		Heap.Pop(EAllowShrinking::No);
		int32 Index = 0;
		while (true)
		{
			const int32 Left = Index * 2 + 1;
			const int32 Right = Left + 1;
			if (Left >= Heap.Num()) break;
			int32 Smallest = Left;
			if (Right < Heap.Num() && IsTokenLess(Heap[Right], Heap[Left])) Smallest = Right;
			if (!IsTokenLess(Heap[Smallest], Heap[Index])) break;
			Swap(Heap[Index], Heap[Smallest]);
			Index = Smallest;
		}
		return true;
	}

	bool FMeteorSettlementQueue::IsCellCandidateLess(
		const FCellCandidate& Left,
		const FCellCandidate& Right)
	{
		if (Left.DistanceSquared != Right.DistanceSquared)
		{
			return Left.DistanceSquared < Right.DistanceSquared;
		}
		if (IsTokenLess(Left.Token, Right.Token)) return true;
		if (IsTokenLess(Right.Token, Left.Token)) return false;
		return Left.Cell.X != Right.Cell.X
			? Left.Cell.X < Right.Cell.X : Left.Cell.Y < Right.Cell.Y;
	}

	void FMeteorSettlementQueue::CellCandidateHeapPush(
		TArray<FCellCandidate>& Heap,
		FCellCandidate Candidate)
	{
		int32 Index = Heap.Add(MoveTemp(Candidate));
		while (Index > 0)
		{
			const int32 Parent = (Index - 1) / 2;
			if (!IsCellCandidateLess(Heap[Index], Heap[Parent])) break;
			Swap(Heap[Index], Heap[Parent]);
			Index = Parent;
		}
	}

	bool FMeteorSettlementQueue::CellCandidateHeapPop(
		TArray<FCellCandidate>& Heap,
		FCellCandidate& OutCandidate)
	{
		if (Heap.IsEmpty()) return false;
		OutCandidate = Heap[0];
		if (Heap.Num() == 1)
		{
			Heap.Pop(EAllowShrinking::No);
			return true;
		}
		Heap[0] = Heap.Last();
		Heap.Pop(EAllowShrinking::No);
		int32 Index = 0;
		while (true)
		{
			const int32 Left = Index * 2 + 1;
			const int32 Right = Left + 1;
			if (Left >= Heap.Num()) break;
			int32 Smallest = Left;
			if (Right < Heap.Num()
				&& IsCellCandidateLess(Heap[Right], Heap[Left])) Smallest = Right;
			if (!IsCellCandidateLess(Heap[Smallest], Heap[Index])) break;
			Swap(Heap[Index], Heap[Smallest]);
			Index = Smallest;
		}
		return true;
	}

	FIntPoint FMeteorSettlementQueue::MakeCell(const FVector& Location) const
	{
		return FIntPoint(
			FMath::FloorToInt(Location.X / CellSize),
			FMath::FloorToInt(Location.Y / CellSize));
	}

	double FMeteorSettlementQueue::DistanceSquaredToCell(
		const FVector& Location,
		const FIntPoint Cell) const
	{
		const double MinimumX = static_cast<double>(Cell.X) * CellSize;
		const double MinimumY = static_cast<double>(Cell.Y) * CellSize;
		const double MaximumX = MinimumX + CellSize;
		const double MaximumY = MinimumY + CellSize;
		const double DeltaX = FMath::Max3(MinimumX - Location.X, 0.0, Location.X - MaximumX);
		const double DeltaY = FMath::Max3(MinimumY - Location.Y, 0.0, Location.Y - MaximumY);
		return DeltaX * DeltaX + DeltaY * DeltaY;
	}

	bool FMeteorSettlementQueue::IsPendingToken(const FQueueToken& Token) const
	{
		return Token.Ordinal != MAX_uint32
			&& LanesByOrdinal.IsValidIndex(static_cast<int32>(Token.Ordinal))
			&& LanesByOrdinal[Token.Ordinal].State == ELaneState::Pending
			&& LanesByOrdinal[Token.Ordinal].Lane.DueTimeSeconds == Token.DueTimeSeconds;
	}

	bool FMeteorSettlementQueue::TrimGlobalHeap()
	{
		FQueueToken Discarded;
		while (!GlobalDueHeap.IsEmpty() && !IsPendingToken(GlobalDueHeap[0]))
		{
			TokenHeapPop(GlobalDueHeap, Discarded);
		}
		return !GlobalDueHeap.IsEmpty();
	}

	bool FMeteorSettlementQueue::TrimCellHeap(
		const FIntPoint CellKey,
		FCell& Cell)
	{
		FQueueToken Discarded;
		while (!Cell.DueHeap.IsEmpty())
		{
			const FQueueToken& Token = Cell.DueHeap[0];
			if (IsPendingToken(Token)
				&& LanesByOrdinal[Token.Ordinal].Cell == CellKey)
			{
				break;
			}
			TokenHeapPop(Cell.DueHeap, Discarded);
		}
		return !Cell.DueHeap.IsEmpty();
	}

	void FMeteorSettlementQueue::BuildNearbyCellCandidates(
		const TConstArrayView<FVector> AuthorityPlayerLocations,
		const double NowSeconds,
		TArray<FCellCandidate>& OutCandidates)
	{
		OutCandidates.Reset();
		if (AuthorityPlayerLocations.IsEmpty() || Cells.IsEmpty()) return;

		TMap<FIntPoint, double> NearbyCellDistances;
		const int32 RadiusInCells = FMath::CeilToInt(PriorityRadius / CellSize) + 1;
		const double RadiusSquared = FMath::Square(static_cast<double>(PriorityRadius));
		for (const FVector& PlayerLocation : AuthorityPlayerLocations)
		{
			if (PlayerLocation.ContainsNaN()) continue;
			const FIntPoint Center = MakeCell(PlayerLocation);
			for (int32 Y = Center.Y - RadiusInCells; Y <= Center.Y + RadiusInCells; ++Y)
			{
				for (int32 X = Center.X - RadiusInCells; X <= Center.X + RadiusInCells; ++X)
				{
					const FIntPoint CellKey(X, Y);
					if (!Cells.Contains(CellKey)) continue;
					const double DistanceSquared = DistanceSquaredToCell(PlayerLocation, CellKey);
					if (DistanceSquared > RadiusSquared) continue;
					double* Existing = NearbyCellDistances.Find(CellKey);
					if (Existing)
					{
						*Existing = FMath::Min(*Existing, DistanceSquared);
					}
					else
					{
						NearbyCellDistances.Add(CellKey, DistanceSquared);
					}
				}
			}
		}

		OutCandidates.Reserve(NearbyCellDistances.Num());
		for (const TPair<FIntPoint, double>& Pair : NearbyCellDistances)
		{
			FCell* Cell = Cells.Find(Pair.Key);
			if (!Cell || !TrimCellHeap(Pair.Key, *Cell)
				|| Cell->DueHeap[0].DueTimeSeconds > NowSeconds)
			{
				continue;
			}
			CellCandidateHeapPush(OutCandidates, {Pair.Key, Pair.Value, Cell->DueHeap[0]});
		}
	}

	int32 FMeteorSettlementQueue::ReserveNearby(
		TArray<FCellCandidate>& Candidates,
		const double NowSeconds,
		const int32 MaximumCount,
		TArray<FMeteorSettlementReservation>& OutReservations)
	{
		const int32 StartCount = OutReservations.Num();
		while (OutReservations.Num() - StartCount < MaximumCount)
		{
			FCellCandidate Candidate;
			if (!CellCandidateHeapPop(Candidates, Candidate)) break;
			FCell* Cell = Cells.Find(Candidate.Cell);
			if (!Cell || !TrimCellHeap(Candidate.Cell, *Cell)
				|| Cell->DueHeap[0].DueTimeSeconds > NowSeconds)
			{
				continue;
			}
			FQueueToken Token;
			verify(TokenHeapPop(Cell->DueHeap, Token));
			if (!IsPendingToken(Token)) continue;
			FStoredLane& Stored = LanesByOrdinal[Token.Ordinal];
			Stored.State = ELaneState::Reserved;
			OutReservations.Add({
				Token.Ordinal,
				EMeteorSettlementReservationSource::ProximityCell,
				Candidate.Cell});

			if (TrimCellHeap(Candidate.Cell, *Cell)
				&& Cell->DueHeap[0].DueTimeSeconds <= NowSeconds)
			{
				Candidate.Token = Cell->DueHeap[0];
				CellCandidateHeapPush(Candidates, MoveTemp(Candidate));
			}
		}
		return OutReservations.Num() - StartCount;
	}

	int32 FMeteorSettlementQueue::ReserveGlobalOldest(
		const double NowSeconds,
		const int32 MaximumCount,
		TArray<FMeteorSettlementReservation>& OutReservations)
	{
		const int32 StartCount = OutReservations.Num();
		while (OutReservations.Num() - StartCount < MaximumCount
			&& TrimGlobalHeap() && GlobalDueHeap[0].DueTimeSeconds <= NowSeconds)
		{
			FQueueToken Token;
			verify(TokenHeapPop(GlobalDueHeap, Token));
			if (!IsPendingToken(Token)) continue;
			FStoredLane& Stored = LanesByOrdinal[Token.Ordinal];
			Stored.State = ELaneState::Reserved;
			OutReservations.Add({
				Token.Ordinal,
				EMeteorSettlementReservationSource::GlobalOldest,
				Stored.Cell});
		}
		return OutReservations.Num() - StartCount;
	}
}
