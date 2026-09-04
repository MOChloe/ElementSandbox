#pragma once

#include "CoreMinimal.h"
#include "Chunk/WorldChunkTypes.h"

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageManifestInfo final
{
	/** v3 removes every legacy thermal payload; older manifests are intentionally rejected. */
	static constexpr uint16 FormatVersion = 3;

	FGuid WorldId;
	uint64 Generation = 0;
	uint64 NextEntityId = 1;
	int64 WorldSimulationTimeMilliseconds = 0;
	FDateTime LastCheckpointUtc;
	int64 CompleteStructureCount = 0;
	int64 BuildingEntityCount = 0;
	int64 WorldObjectEntityCount = 0;

	bool IsValid() const;
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageOpenOptions final
{
	FString WritableRoot;
	FString SeedRoot;
	bool bCreateIfMissing = true;
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageArchiveStats final
{
	int32 PackCount = 0;
	int32 OccupiedChunkCount = 0;
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkCheckpointChange final
{
	FWorldChunkCoord Coord;
	TOptional<FWorldCompressedChunk> Chunk;
};

class FWorldStorageArchiveData;
class FWorldSeedArchiveWriterData;

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageArchiveDeleter
{
	void operator()(FWorldStorageArchiveData* Data) const;
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldSeedArchiveWriterDeleter
{
	void operator()(FWorldSeedArchiveWriterData* Data) const;
};

/**
 * Editor Commandlet 使用的内存有界种子写入器。
 *
 * 调用方一次只提交一个 Pack；Pack 文件立即落盘，直到 Commit 才原子发布 Manifest。
 * 因此生成数千万 Entity 时无需把全世界的压缩 Chunk 同时保留在内存中。
 */
class ELEMENTSANDBOXWORLDSTORAGE_API FWorldSeedArchiveWriter final
{
public:
	FWorldSeedArchiveWriter();
	~FWorldSeedArchiveWriter();

	FWorldSeedArchiveWriter(const FWorldSeedArchiveWriter&) = delete;
	FWorldSeedArchiveWriter& operator=(const FWorldSeedArchiveWriter&) = delete;

	bool Begin(const FString& Root, const FWorldStorageManifestInfo& ManifestInfo, FString& OutError);
	bool WritePack(
		const FWorldChunkPackCoord& PackCoord,
		TConstArrayView<FWorldCompressedChunk> Chunks,
		FString& OutError);
	bool Commit(FString& OutError);
	bool IsOpen() const;

private:
	TUniquePtr<FWorldSeedArchiveWriterData, FWorldSeedArchiveWriterDeleter> Data;
};

/**
 * Manifest + immutable Pack 的线程安全访问边界。Checkpoint 只重写受影响 Pack，最后原子替换 Manifest。
 */
class ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageArchive final
{
public:
	FWorldStorageArchive();
	~FWorldStorageArchive();

	FWorldStorageArchive(const FWorldStorageArchive&) = delete;
	FWorldStorageArchive& operator=(const FWorldStorageArchive&) = delete;

	bool Open(const FWorldStorageOpenOptions& Options, FString& OutError);
	bool IsOpen() const;
	FWorldStorageManifestInfo GetManifestInfo() const;
	FWorldStorageArchiveStats GetArchiveStats() const;
	void GetOccupiedChunkCoords(TArray<FWorldChunkCoord>& OutChunks) const;
	/** 返回 Building/WorldObject 最密集的 Chunk；同密度时稳定选择更靠近占用范围中心者。 */
	bool TryGetMostPopulatedChunk(FWorldChunkCoord& OutChunk) const;
	bool ReadCompressedChunk(const FWorldChunkCoord& Coord, FWorldCompressedChunk& OutChunk, FString& OutError) const;
	bool TryGetChunkOffer(const FWorldChunkCoord& Coord, FWorldChunkOffer& OutOffer) const;
	void QueryOccupiedChunks(const FWorldChunkBox& Box, TArray<FWorldChunkCoord>& OutChunks) const;
	bool WriteCheckpoint(
		TConstArrayView<FWorldChunkCheckpointChange> Changes,
		const FWorldStorageManifestInfo& NewManifestInfo,
		FString& OutError);

	/** Editor Commandlet 使用：创建全新的种子存档，不读取或迁移旧格式。 */
	static bool WriteSeedArchive(
		const FString& Root,
		const FWorldStorageManifestInfo& ManifestInfo,
		TConstArrayView<FWorldCompressedChunk> Chunks,
		FString& OutError);

private:
	TUniquePtr<FWorldStorageArchiveData, FWorldStorageArchiveDeleter> Data;
};
