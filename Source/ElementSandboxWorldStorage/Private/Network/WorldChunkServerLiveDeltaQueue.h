#pragma once

#include "Chunk/WorldChunkTypes.h"

namespace UE::ElementSandbox::WorldStorage::Private
{

/**
 * 单连接尚未发送的权威增量。按 WorldEntityId 合并最新状态，Chunk 索引只持有身份。
 * 每次发送按当前权威 Pawn 位置选最近 Chunk；删除优先只在该 Chunk 内生效。
 * 每七个近场批次后轮转一个后台 Chunk，持续交互不会让远处永久积压。
 * Snapshot ACK 与可靠 Sequence 窗口仍由协议端点拥有。
 */
class FWorldChunkServerLiveDeltaQueue final
{
public:
	void Enqueue(FWorldChunkLiveDelta Delta);
	void RemoveChunk(FWorldChunkCoord Coord);
	int32 Num() const { return DeltasByEntity.Num(); }
	bool IsEmpty() const { return DeltasByEntity.IsEmpty(); }

	/** 仅消费已有 Snapshot ACK 的 Chunk，输出同质且同时满足条数/字节预算的批次。 */
	bool BuildBatch(
		const FVector& AuthorityLocation,
		TFunctionRef<bool(FWorldChunkCoord)> HasSnapshotAcknowledged,
		int32 MaximumCount,
		int32 MaximumBytes,
		TArray<FWorldChunkLiveDelta>& OutBatch);

private:
	struct FChunkEntities
	{
		TSet<FWorldEntityId> EntityIds;
		// 不同类型不能混批；先看计数，避免每个近场 Upsert 批次扫描全部远处 Tombstone。
		int32 KindCounts[3] = {};
	};
	void RemoveChunkMembership(FWorldChunkCoord Coord, FWorldEntityId EntityId, EWorldChunkLiveDeltaKind Kind);

	TMap<FWorldEntityId, FWorldChunkLiveDelta> DeltasByEntity;
	TMap<FWorldChunkCoord, FChunkEntities> EntitiesByChunk;
	TOptional<FWorldChunkCoord> LastBackgroundChunk;
	int32 ConsecutivePriorityBatches = 0;
};

}
