#include "WorldStorageSubsystem.h"
#include "WorldStorageProcessRole.h"

#include "Async/Async.h"
#include "Containers/Queue.h"
#include "ElementSandboxWorldStorage.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "CoreGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Storage/WorldChunkCodec.h"
#include "UObject/Class.h"
#include "WorldStorageRuntime.h"

CSV_DEFINE_CATEGORY(ElementSandboxWorldStorage, true);

namespace
{
uint32 NextRevision(const uint32 Revision) { return Revision == MAX_uint32 ? 1 : Revision + 1; }

bool IsPersistentDomain(const EWorldEntityDomain Domain)
{
	return Domain == EWorldEntityDomain::Building || Domain == EWorldEntityDomain::WorldObject ||
		   Domain == EWorldEntityDomain::Element;
}

bool IsClassifiableDomain(const EWorldEntityDomain Domain)
{
	return IsPersistentDomain(Domain) || Domain == EWorldEntityDomain::Character;
}

double ChunkLoadPriorityScore(const FWorldChunkCoord& Coord, const FVector& SourceLocation,
								  const FVector& SourceForward)
{
	const FVector ChunkCenter = Coord.GetWorldMinimum() + FVector(FWorldChunkCoord::EdgeCentimeters * 0.5);
	const FVector ToChunk = ChunkCenter - SourceLocation;
	const double SquaredDistance = ToChunk.SquaredLength();
	if (SquaredDistance <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	const double Facing = FVector::DotProduct(ToChunk.GetSafeNormal(),
											  SourceForward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector));
	// 距离仍是主因；同量级距离中，视线前方最多获得 1.5 倍于背后的优先级。
	return SquaredDistance * (1.25 - 0.25 * FMath::Clamp(Facing, -1.0, 1.0));
}

struct FNetworkApplyGroupKey final
{
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
	FWorldChunkCoord HomeChunk;
	uint8 Variant = 0;

	bool operator==(const FNetworkApplyGroupKey& Other) const
	{
		return Domain == Other.Domain && HomeChunk == Other.HomeChunk && Variant == Other.Variant;
	}

	friend uint32 GetTypeHash(const FNetworkApplyGroupKey& Key)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(static_cast<uint8>(Key.Domain)), GetTypeHash(Key.HomeChunk)),
			GetTypeHash(Key.Variant));
	}
};
} // namespace

namespace
{
bool CaptureRecords(FWorldStorageRuntime& Runtime, TConstArrayView<FWorldEntityId> EntityIds,
					TArray<FWorldPersistentEntityRecord>& OutRecords, FString& OutError)
{
	OutRecords.Reset();
	TMap<EWorldEntityDomain, TArray<FWorldEntityId>> ByDomain;
	for (const FWorldEntityId EntityId : EntityIds)
	{
		const FResidentEntry* Resident = Runtime.Entities.Residents.Find(EntityId);
		if (!Resident)
		{
			OutError = FString::Printf(TEXT("持久化捕获找不到 Resident Entity %llu。"), EntityId.GetValue());
			return false;
		}
		ByDomain.FindOrAdd(Resident->Domain).Add(EntityId);
	}
	for (const TPair<EWorldEntityDomain, TArray<FWorldEntityId>>& Pair : ByDomain)
	{
		const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime.Core.Adapters.Find(Pair.Key);
		if (!Adapter)
		{
			OutError = TEXT("持久化领域尚未注册 Adapter。");
			return false;
		}
		TArray<FWorldPersistentEntityRecord> DomainRecords;
		if (!Adapter->Get().CaptureBatch(Pair.Value, DomainRecords, OutError) ||
			DomainRecords.Num() != Pair.Value.Num())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("领域 Adapter 未返回完整的 CaptureBatch。");
			}
			return false;
		}
		TSet<FWorldEntityId> Expected;
		Expected.Append(Pair.Value);
		for (FWorldPersistentEntityRecord& Record : DomainRecords)
		{
			const FResidentEntry* Resident = Runtime.Entities.Residents.Find(Record.EntityId);
			if (!Record.IsValid() || Record.Domain != Pair.Key || !Expected.Remove(Record.EntityId) || !Resident ||
				FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()) != Resident->HomeChunk)
			{
				OutError = TEXT("领域 Adapter 返回了无效、重复或 HomeChunk 不一致的记录。");
				return false;
			}
			OutRecords.Add(MoveTemp(Record));
		}
		if (!Expected.IsEmpty())
		{
			OutError = TEXT("领域 Adapter 漏掉了待捕获 Entity。");
			return false;
		}
	}
	return true;
}

bool BuildCapturedChunkSnapshot(const TSharedRef<FWorldStorageArchive, ESPMode::ThreadSafe>& Archive,
								const FWorldChunkCoord& Coord, const uint32 Revision,
								const TMap<FWorldEntityId, FDirtyEntityState>& DirtyEntities,
								TArray<FWorldPersistentEntityRecord> ResidentRecords,
								FWorldCompressedChunk& OutCompressed, FString& OutError)
{
	FWorldChunkData OutChunk;
	OutChunk.Coord = Coord;
	OutChunk.Revision = Revision;
	TMap<FWorldEntityId, FWorldPersistentEntityRecord> RecordsById;
	FWorldChunkOffer ExistingOffer;
	if (Archive->TryGetChunkOffer(Coord, ExistingOffer))
	{
		FWorldCompressedChunk ExistingCompressed;
		FWorldChunkData ExistingData;
		if (!Archive->ReadCompressedChunk(Coord, ExistingCompressed, OutError) ||
			!FWorldChunkCodec::Decompress(ExistingCompressed, ExistingData, OutError))
		{
			return false;
		}
		for (FWorldPersistentEntityRecord& Record : ExistingData.Records)
		{
			RecordsById.Add(Record.EntityId, MoveTemp(Record));
		}
	}

	for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : DirtyEntities)
	{
		const FDirtyEntityState& Dirty = Pair.Value;
		if (Dirty.RemovedFromChunks.Contains(Coord) || (Dirty.bGameplayDestroyed && Dirty.CurrentChunk == Coord))
		{
			RecordsById.Remove(Pair.Key);
		}
		if (!Dirty.bGameplayDestroyed && Dirty.CurrentChunk == Coord && Dirty.StoredRecord.IsSet())
		{
			RecordsById.Add(Pair.Key, Dirty.StoredRecord.GetValue());
		}
	}
	for (FWorldPersistentEntityRecord& Record : ResidentRecords)
	{
		RecordsById.Add(Record.EntityId, MoveTemp(Record));
	}
	for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : DirtyEntities)
	{
		const FDirtyEntityState& Dirty = Pair.Value;
		if (!Dirty.bGameplayDestroyed && Dirty.CurrentChunk == Coord && !RecordsById.Contains(Pair.Key))
		{
			OutError = TEXT("Dirty Chunk 快照缺少当前 Entity 的 Capture 记录。");
			return false;
		}
	}
	RecordsById.GenerateValueArray(OutChunk.Records);
	return OutChunk.IsValid() && FWorldChunkCodec::Compress(OutChunk, OutCompressed, OutError);
}

bool ApplyDirtyOverlayToDecodedChunk(FWorldStorageRuntime& Runtime, FWorldChunkData& Chunk, FString& OutError)
{
	if (!Runtime.Entities.DirtyChunks.Contains(Chunk.Coord))
	{
		return true;
	}
	TMap<FWorldEntityId, FWorldPersistentEntityRecord> RecordsById;
	for (FWorldPersistentEntityRecord& Record : Chunk.Records)
	{
		RecordsById.Add(Record.EntityId, MoveTemp(Record));
	}
	TArray<FWorldEntityId> NeedCapture;
	for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : Runtime.Entities.DirtyEntities)
	{
		const FDirtyEntityState& Dirty = Pair.Value;
		if (Dirty.RemovedFromChunks.Contains(Chunk.Coord) ||
			(Dirty.bGameplayDestroyed && Dirty.CurrentChunk == Chunk.Coord))
		{
			RecordsById.Remove(Pair.Key);
		}
		if (Dirty.bGameplayDestroyed || Dirty.CurrentChunk != Chunk.Coord)
		{
			continue;
		}
		if (Dirty.StoredRecord.IsSet())
		{
			RecordsById.Add(Pair.Key, Dirty.StoredRecord.GetValue());
		}
		else if (Runtime.Entities.Residents.Contains(Pair.Key))
		{
			NeedCapture.Add(Pair.Key);
		}
		else
		{
			OutError = TEXT("Dirty Overlay 缺少 Resident 或 Evict 时冻结的记录。");
			return false;
		}
	}
	TArray<FWorldPersistentEntityRecord> Captured;
	if (!NeedCapture.IsEmpty() && !CaptureRecords(Runtime, NeedCapture, Captured, OutError))
	{
		return false;
	}
	for (FWorldPersistentEntityRecord& Record : Captured)
	{
		RecordsById.Add(Record.EntityId, MoveTemp(Record));
	}
	Chunk.Revision = Runtime.Residency.Chunks.FindOrAdd(Chunk.Coord).Revision;
	RecordsById.GenerateValueArray(Chunk.Records);
	return Chunk.IsValid();
}

void RemoveResident(FWorldStorageRuntime& Runtime, const FWorldEntityId EntityId)
{
	const FResidentEntry* Resident = Runtime.Entities.Residents.Find(EntityId);
	if (!Resident)
	{
		return;
	}
	const FWorldChunkCoord HomeChunk = Resident->HomeChunk;
	if (TSet<FWorldEntityId>* ChunkIds = Runtime.Entities.ResidentIdsByChunk.Find(HomeChunk))
	{
		ChunkIds->Remove(EntityId);
		if (ChunkIds->IsEmpty())
		{
			Runtime.Entities.ResidentIdsByChunk.Remove(HomeChunk);
		}
	}
	Runtime.Entities.Residents.Remove(EntityId);
	Runtime.Entities.AwakePhysicsPinnedSinceSeconds.Remove(EntityId);
}

bool ExecuteCheckpointWork(const TSharedRef<FWorldStorageArchive, ESPMode::ThreadSafe>& Archive,
						   const TMap<FWorldEntityId, FDirtyEntityState>& DirtySnapshot,
						   const TMap<FWorldChunkCoord, uint32>& ChunkRevisions,
						   const FWorldStorageManifestInfo& ManifestInfo, FString& OutError)
{
	TSet<FWorldChunkCoord> AffectedChunks;
	for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : DirtySnapshot)
	{
		AffectedChunks.Add(Pair.Value.CurrentChunk);
		for (const FWorldChunkCoord& RemovedChunk : Pair.Value.RemovedFromChunks)
		{
			AffectedChunks.Add(RemovedChunk);
		}
	}
	TArray<FWorldChunkCheckpointChange> Changes;
	Changes.Reserve(AffectedChunks.Num());
	for (const FWorldChunkCoord& Coord : AffectedChunks)
	{
		FWorldChunkData Chunk;
		Chunk.Coord = Coord;
		Chunk.Revision = ChunkRevisions.FindRef(Coord);
		if (Chunk.Revision == 0)
		{
			Chunk.Revision = 1;
		}
		TMap<FWorldEntityId, FWorldPersistentEntityRecord> Records;
		FWorldChunkOffer ExistingOffer;
		if (Archive->TryGetChunkOffer(Coord, ExistingOffer))
		{
			FWorldCompressedChunk ExistingCompressed;
			FWorldChunkData ExistingData;
			if (!Archive->ReadCompressedChunk(Coord, ExistingCompressed, OutError) ||
				!FWorldChunkCodec::Decompress(ExistingCompressed, ExistingData, OutError))
			{
				return false;
			}
			for (FWorldPersistentEntityRecord& Record : ExistingData.Records)
			{
				Records.Add(Record.EntityId, MoveTemp(Record));
			}
		}
		for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : DirtySnapshot)
		{
			const FDirtyEntityState& Dirty = Pair.Value;
			if (Dirty.RemovedFromChunks.Contains(Coord) || (Dirty.bGameplayDestroyed && Dirty.CurrentChunk == Coord))
			{
				Records.Remove(Pair.Key);
			}
			if (!Dirty.bGameplayDestroyed && Dirty.CurrentChunk == Coord && Dirty.StoredRecord.IsSet())
			{
				Records.Add(Pair.Key, Dirty.StoredRecord.GetValue());
			}
		}
		FWorldChunkCheckpointChange& Change = Changes.AddDefaulted_GetRef();
		Change.Coord = Coord;
		if (!Records.IsEmpty())
		{
			Records.GenerateValueArray(Chunk.Records);
			FWorldCompressedChunk Compressed;
			if (!FWorldChunkCodec::Compress(Chunk, Compressed, OutError))
			{
				return false;
			}
			Change.Chunk = MoveTemp(Compressed);
		}
	}
	return Archive->WriteCheckpoint(Changes, ManifestInfo, OutError);
}
} // namespace

#include "WorldStorageLifecycle.inl"
#include "WorldStorageResidency.inl"
#include "WorldStorageSnapshots.inl"
