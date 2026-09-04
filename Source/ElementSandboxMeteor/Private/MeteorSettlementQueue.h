#pragma once

#include "CoreMinimal.h"
#include "MeteorRuntimeTypes.h"

namespace UE::ElementSandbox::Meteor
{
	/** 一条已经 Activate、等待转换为普通 WorldObject 的解析产物。 */
	struct FMeteorSettlementLane final
	{
		FMeteorDebrisKey Key;
		FWorldEntityId WorldEntityId;
		FName ProductDefinitionId = NAME_None;
		/** 与客户端解析终点完全相同的最终木块 Transform。 */
		FTransform WorldTransform = FTransform::Identity;
		double DueTimeSeconds = 0.0;

		bool IsValid() const;
	};

	enum class EMeteorSettlementReservationSource : uint8
	{
		ProximityCell,
		GlobalOldest
	};

	/** Reserve/Commit/Rollback 令牌；一次 WorldObject Stage 批次期间独占对应 Lane。 */
	struct FMeteorSettlementReservation final
	{
		uint32 Ordinal = MAX_uint32;
		EMeteorSettlementReservationSource Source =
			EMeteorSettlementReservationSource::GlobalOldest;
		FIntPoint Cell = FIntPoint::ZeroValue;

		bool IsSet() const { return Ordinal != MAX_uint32; }
	};

	/**
	 * Authority 落地队列。
	 *
	 * 每条 Lane 同时写入一个全局到期堆和一个二维 Cell 到期堆。角色附近只枚举固定半径
	 * 内的 Cell，因此近场优先不需要每帧扫描十万条 Backlog；全局堆固定保留少量批额，
	 * 让远处最终完成且不会被移动中的角色永久饿死。
	 */
	class FMeteorSettlementQueue final
	{
	public:
		bool Initialize(
			FMeteorBurstId InBurstId,
			float InCellSize,
			float InPriorityRadius);
		void Reset();
		bool Enqueue(FMeteorSettlementLane Lane);

		int32 ReserveDue(
			TConstArrayView<FVector> AuthorityPlayerLocations,
			double NowSeconds,
			int32 MaximumCount,
			int32 MinimumGlobalOldestCount,
			TArray<FMeteorSettlementReservation>& OutReservations);
		const FMeteorSettlementLane* FindReserved(
			const FMeteorSettlementReservation& Reservation) const;
		bool CommitReserved(const FMeteorSettlementReservation& Reservation);
		bool RollbackReserved(const FMeteorSettlementReservation& Reservation);

		int32 Num() const { return OutstandingLaneCount; }
		bool IsEmpty() const { return OutstandingLaneCount == 0; }

	private:
		enum class ELaneState : uint8
		{
			Missing,
			Pending,
			Reserved,
			Settled
		};

		struct FQueueToken final
		{
			double DueTimeSeconds = 0.0;
			uint32 Ordinal = MAX_uint32;
		};

		struct FStoredLane final
		{
			FMeteorSettlementLane Lane;
			FIntPoint Cell = FIntPoint::ZeroValue;
			ELaneState State = ELaneState::Missing;
		};

		struct FCell final
		{
			TArray<FQueueToken> DueHeap;
		};

		struct FCellCandidate final
		{
			FIntPoint Cell = FIntPoint::ZeroValue;
			double DistanceSquared = 0.0;
			FQueueToken Token;
		};

		static bool IsTokenLess(const FQueueToken& Left, const FQueueToken& Right);
		static void TokenHeapPush(TArray<FQueueToken>& Heap, FQueueToken Token);
		static bool TokenHeapPop(TArray<FQueueToken>& Heap, FQueueToken& OutToken);
		static bool IsCellCandidateLess(
			const FCellCandidate& Left,
			const FCellCandidate& Right);
		static void CellCandidateHeapPush(
			TArray<FCellCandidate>& Heap,
			FCellCandidate Candidate);
		static bool CellCandidateHeapPop(
			TArray<FCellCandidate>& Heap,
			FCellCandidate& OutCandidate);

		FIntPoint MakeCell(const FVector& Location) const;
		double DistanceSquaredToCell(const FVector& Location, FIntPoint Cell) const;
		bool IsPendingToken(const FQueueToken& Token) const;
		bool TrimGlobalHeap();
		bool TrimCellHeap(FIntPoint CellKey, FCell& Cell);
		void BuildNearbyCellCandidates(
			TConstArrayView<FVector> AuthorityPlayerLocations,
			double NowSeconds,
			TArray<FCellCandidate>& OutCandidates);
		int32 ReserveNearby(
			TArray<FCellCandidate>& Candidates,
			double NowSeconds,
			int32 MaximumCount,
			TArray<FMeteorSettlementReservation>& OutReservations);
		int32 ReserveGlobalOldest(
			double NowSeconds,
			int32 MaximumCount,
			TArray<FMeteorSettlementReservation>& OutReservations);

		FMeteorBurstId BurstId;
		float CellSize = 0.0f;
		float PriorityRadius = 0.0f;
		int32 OutstandingLaneCount = 0;
		TArray<FStoredLane> LanesByOrdinal;
		TArray<FQueueToken> GlobalDueHeap;
		TMap<FIntPoint, FCell> Cells;
	};
}
