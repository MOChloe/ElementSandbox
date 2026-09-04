#include "Network/WorldChunkServerLiveDeltaQueue.h"

namespace UE::ElementSandbox::WorldStorage::Private
{

void FWorldChunkServerLiveDeltaQueue::RemoveChunkMembership(
	const FWorldChunkCoord Coord, const FWorldEntityId EntityId, const EWorldChunkLiveDeltaKind Kind)
{
	FChunkEntities& Members = EntitiesByChunk.FindChecked(Coord);
	Members.EntityIds.Remove(EntityId);
	--Members.KindCounts[static_cast<uint8>(Kind)];
	if (Members.EntityIds.IsEmpty()) EntitiesByChunk.Remove(Coord);
}

void FWorldChunkServerLiveDeltaQueue::Enqueue(FWorldChunkLiveDelta Delta)
{
	const FWorldEntityId EntityId = Delta.EntityId;
	// 同一物件在积压期间跨 Chunk 移动时也只发送最新状态，不能在重排后再回放旧位置。
	if (const FWorldChunkLiveDelta* Previous = DeltasByEntity.Find(EntityId))
	{
		RemoveChunkMembership(Previous->ChunkCoord, EntityId, Previous->Kind);
	}
	FChunkEntities& Members = EntitiesByChunk.FindOrAdd(Delta.ChunkCoord);
	Members.EntityIds.Add(EntityId);
	++Members.KindCounts[static_cast<uint8>(Delta.Kind)];
	DeltasByEntity.Add(EntityId, MoveTemp(Delta));
}

void FWorldChunkServerLiveDeltaQueue::RemoveChunk(const FWorldChunkCoord Coord)
{
	if (const FChunkEntities* Members = EntitiesByChunk.Find(Coord))
	{
		for (const FWorldEntityId EntityId : Members->EntityIds) DeltasByEntity.Remove(EntityId);
		EntitiesByChunk.Remove(Coord);
	}
}

bool FWorldChunkServerLiveDeltaQueue::BuildBatch(
	const FVector& AuthorityLocation,
	TFunctionRef<bool(FWorldChunkCoord)> HasSnapshotAcknowledged,
	const int32 MaximumCount,
	const int32 MaximumBytes,
	TArray<FWorldChunkLiveDelta>& OutBatch)
{
	OutBatch.Reset();
	if (MaximumCount <= 0 || MaximumBytes <= 0 || IsEmpty()) return false;

	struct FChunkCandidate
	{
		FWorldChunkCoord Coord;
		double DistanceSquared = 0.0;
	};
	TArray<FChunkCandidate> Chunks;
	Chunks.Reserve(EntitiesByChunk.Num());
	for (const auto& Pair : EntitiesByChunk)
	{
		if (!HasSnapshotAcknowledged(Pair.Key)) continue;
		const FVector Minimum = Pair.Key.GetWorldMinimum();
		const FBox Bounds(Minimum, Minimum + FVector(FWorldChunkCoord::EdgeCentimeters));
		Chunks.Add({Pair.Key, Bounds.ComputeSquaredDistanceToPoint(AuthorityLocation)});
	}
	if (Chunks.IsEmpty()) return false;

	constexpr int32 MaximumConsecutivePriorityBatches = 7;
	const bool bBackground = ConsecutivePriorityBatches >= MaximumConsecutivePriorityBatches;
	Chunks.Sort([bBackground](const FChunkCandidate& Left, const FChunkCandidate& Right)
	{
		if (!bBackground && Left.DistanceSquared != Right.DistanceSquared)
			return Left.DistanceSquared < Right.DistanceSquared;
		return Left.Coord < Right.Coord;
	});
	int32 FirstChunk = 0;
	if (bBackground && LastBackgroundChunk.IsSet())
	{
		const int32 Next = Chunks.IndexOfByPredicate([this](const FChunkCandidate& Candidate)
		{
			return LastBackgroundChunk.GetValue() < Candidate.Coord;
		});
		if (Next != INDEX_NONE) FirstChunk = Next;
	}

	// 先选位置，再在该位置选择删除或 Upsert。远方的 Tombstone 不得压住脚边的产物。
	const FChunkEntities& FirstMembers = EntitiesByChunk.FindChecked(Chunks[FirstChunk].Coord);
	const EWorldChunkLiveDeltaKind Kind =
		FirstMembers.KindCounts[static_cast<uint8>(EWorldChunkLiveDeltaKind::GameplayTombstone)] > 0
		? EWorldChunkLiveDeltaKind::GameplayTombstone
		: FirstMembers.KindCounts[static_cast<uint8>(EWorldChunkLiveDeltaKind::ProjectionRemove)] > 0
			? EWorldChunkLiveDeltaKind::ProjectionRemove : EWorldChunkLiveDeltaKind::Upsert;
	struct FEntityCandidate
	{
		FWorldEntityId EntityId;
		double DistanceSquared = 0.0;
	};
	TArray<FEntityCandidate> Entities;
	int32 EstimatedBytes = 0;
	bool bBudgetReached = false;
	for (int32 Offset = 0; Offset < Chunks.Num() && !bBudgetReached; ++Offset)
	{
		const FWorldChunkCoord Coord = Chunks[(FirstChunk + Offset) % Chunks.Num()].Coord;
		const FChunkEntities& Members = EntitiesByChunk.FindChecked(Coord);
		if (Members.KindCounts[static_cast<uint8>(Kind)] == 0) continue;
		Entities.Reset();
		for (const FWorldEntityId EntityId : Members.EntityIds)
		{
			const FWorldChunkLiveDelta& Delta = DeltasByEntity.FindChecked(EntityId);
			if (Delta.Kind != Kind) continue;
			const double DistanceSquared = !bBackground && Kind == EWorldChunkLiveDeltaKind::Upsert
				? FVector::DistSquared(AuthorityLocation, Delta.Record.WorldTransform.GetLocation()) : 0.0;
			Entities.Add({EntityId, DistanceSquared});
		}
		Entities.Sort([](const FEntityCandidate& Left, const FEntityCandidate& Right)
		{
			return Left.DistanceSquared != Right.DistanceSquared
				? Left.DistanceSquared < Right.DistanceSquared : Left.EntityId < Right.EntityId;
		});
		for (const FEntityCandidate& Candidate : Entities)
		{
			FWorldChunkLiveDelta& Delta = DeltasByEntity.FindChecked(Candidate.EntityId);
			const int32 DeltaBytes = 256 + Delta.Record.Payload.Num();
			if (OutBatch.Num() >= MaximumCount || EstimatedBytes + DeltaBytes > MaximumBytes)
			{
				bBudgetReached = true;
				break;
			}
			EstimatedBytes += DeltaBytes;
			OutBatch.Add(MoveTemp(Delta));
			DeltasByEntity.Remove(Candidate.EntityId);
			RemoveChunkMembership(Coord, Candidate.EntityId, Kind);
		}
	}
	if (OutBatch.IsEmpty()) return false;
	if (bBackground)
	{
		LastBackgroundChunk = Chunks[FirstChunk].Coord;
		ConsecutivePriorityBatches = 0;
	}
	else
	{
		++ConsecutivePriorityBatches;
	}
	return true;
}

}
