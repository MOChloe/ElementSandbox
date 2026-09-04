#include "Network/WorldChunkClientCache.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Storage/WorldChunkCodec.h"

namespace UE::ElementSandbox::WorldStorage::Private
{
	namespace
	{
		constexpr uint32 ChunkCacheMagic = 0x31434357; // WCC1
		constexpr uint16 ChunkCacheVersion = 1;
		constexpr int32 MaximumChunkPayloadBytes = 64 * 1024 * 1024;
	}

	FString FWorldChunkClientCache::MakeFilename(
		const FString& Root,
		const FWorldChunkCoord& Coord)
	{
		return FPaths::Combine(Root, FString::Printf(
			TEXT("%d_%d_%d.wcc"), Coord.X, Coord.Y, Coord.Z));
	}

	bool FWorldChunkClientCache::Load(
		const FString& Root,
		const FWorldChunkOffer& Offer,
		FWorldCompressedChunk& OutChunk)
	{
		OutChunk = {};
		const FString Filename = MakeFilename(Root, Offer.Coord);
		if (!IFileManager::Get().FileExists(*Filename))
		{
			return false;
		}
		TArray<uint8> FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, *Filename))
		{
			return false;
		}
		FMemoryReader Reader(FileBytes, true);
		uint32 Magic = 0;
		uint16 Version = 0;
		FGuid WorldId;
		int32 ByteCount = 0;
		Reader << Magic << Version << WorldId
			<< OutChunk.Coord.X << OutChunk.Coord.Y << OutChunk.Coord.Z
			<< OutChunk.Revision << OutChunk.ContentHash.High << OutChunk.ContentHash.Low
			<< OutChunk.UncompressedSize << ByteCount;
		if (Magic != ChunkCacheMagic || Version != ChunkCacheVersion
			|| WorldId != Offer.WorldId || OutChunk.Coord != Offer.Coord
			|| OutChunk.Revision != Offer.Revision
			|| OutChunk.ContentHash != Offer.ContentHash
			|| OutChunk.UncompressedSize != Offer.UncompressedSize
			|| ByteCount != Offer.CompressedSize || ByteCount <= 0
			|| ByteCount > MaximumChunkPayloadBytes
			|| Reader.Tell() + ByteCount != Reader.TotalSize())
		{
			return false;
		}
		OutChunk.Bytes.SetNumUninitialized(ByteCount);
		Reader.Serialize(OutChunk.Bytes.GetData(), ByteCount);
		return !Reader.IsError()
			&& FWorldChunkCodec::Hash(OutChunk.Bytes) == OutChunk.ContentHash
			&& OutChunk.IsValid();
	}

	bool FWorldChunkClientCache::Save(
		const FString& Root,
		const FGuid& WorldId,
		const FWorldCompressedChunk& Chunk)
	{
		if (!WorldId.IsValid() || !Chunk.IsValid()
			|| Chunk.Bytes.Num() > MaximumChunkPayloadBytes
			|| !IFileManager::Get().MakeDirectory(*Root, true))
		{
			return false;
		}
		TArray<uint8> FileBytes;
		FMemoryWriter Writer(FileBytes, true);
		uint32 Magic = ChunkCacheMagic;
		uint16 Version = ChunkCacheVersion;
		FGuid StoredWorldId = WorldId;
		int32 CoordX = Chunk.Coord.X;
		int32 CoordY = Chunk.Coord.Y;
		int32 CoordZ = Chunk.Coord.Z;
		uint32 Revision = Chunk.Revision;
		uint64 ContentHashHigh = Chunk.ContentHash.High;
		uint64 ContentHashLow = Chunk.ContentHash.Low;
		int32 UncompressedSize = Chunk.UncompressedSize;
		int32 ByteCount = Chunk.Bytes.Num();
		Writer << Magic << Version << StoredWorldId
			<< CoordX << CoordY << CoordZ
			<< Revision << ContentHashHigh << ContentHashLow
			<< UncompressedSize << ByteCount;
		// FArchive 的保存接口仍接收 void*；保存路径不会修改源缓冲区。
		Writer.Serialize(const_cast<uint8*>(Chunk.Bytes.GetData()), Chunk.Bytes.Num());
		if (Writer.IsError())
		{
			return false;
		}
		const FString FinalPath = MakeFilename(Root, Chunk.Coord);
		// 同一 Chunk 的重发完成可能并发落盘；固定 .tmp 会让两个 Worker 互相移动/删除。
		const FString TemporaryPath = FPaths::CreateTempFilename(
			*Root,
			*FString::Printf(TEXT("%d_%d_%d_"), Chunk.Coord.X, Chunk.Coord.Y, Chunk.Coord.Z),
			TEXT(".tmp"));
		if (!FFileHelper::SaveArrayToFile(FileBytes, *TemporaryPath))
		{
			return false;
		}
		const bool bMoved = IFileManager::Get().Move(*FinalPath, *TemporaryPath, true, true);
		if (!bMoved)
		{
			IFileManager::Get().Delete(*TemporaryPath, false, true);
		}
		return bMoved;
	}

	EWorldChunkSegmentAcceptResult FWorldChunkSegmentAssembly::Accept(
		const FWorldChunkPayloadSegment& Segment,
		const int32 SegmentPayloadBytes)
	{
		if (SegmentPayloadBytes <= 0 || !Segment.Offer.WorldId.IsValid()
			|| Segment.Offer.Revision == 0 || !Segment.Offer.ContentHash.IsSet()
			|| Segment.Offer.CompressedSize <= 0
			|| Segment.Offer.CompressedSize > MaximumChunkPayloadBytes
			|| Segment.Offer.UncompressedSize <= 0
			|| Segment.Offer.UncompressedSize > MaximumChunkPayloadBytes
			|| Segment.SegmentCount <= 0
			|| Segment.SegmentCount != FMath::DivideAndRoundUp(
				Segment.Offer.CompressedSize, SegmentPayloadBytes)
			|| Segment.SegmentIndex < 0 || Segment.SegmentIndex >= Segment.SegmentCount
			|| Segment.Bytes.IsEmpty() || Segment.Bytes.Num() > SegmentPayloadBytes)
		{
			return EWorldChunkSegmentAcceptResult::Rejected;
		}
		const int32 ExpectedSegmentBytes = Segment.SegmentIndex + 1 == Segment.SegmentCount
			? Segment.Offer.CompressedSize
				- SegmentPayloadBytes * (Segment.SegmentCount - 1)
			: SegmentPayloadBytes;
		if (Segment.Bytes.Num() != ExpectedSegmentBytes)
		{
			return EWorldChunkSegmentAcceptResult::Rejected;
		}

		if (!bInitialized)
		{
			Offer = Segment.Offer;
			Bytes.SetNumUninitialized(Segment.Offer.CompressedSize);
			Received.Init(false, Segment.SegmentCount);
			bInitialized = true;
		}
		if (Offer.WorldId != Segment.Offer.WorldId
			|| Offer.Coord != Segment.Offer.Coord
			|| Offer.Revision != Segment.Offer.Revision
			|| Offer.ContentHash != Segment.Offer.ContentHash
			|| Offer.CompressedSize != Segment.Offer.CompressedSize
			|| Offer.UncompressedSize != Segment.Offer.UncompressedSize
			|| Bytes.Num() != Segment.Offer.CompressedSize
			|| Received.Num() != Segment.SegmentCount)
		{
			return EWorldChunkSegmentAcceptResult::Rejected;
		}
		if (Received[Segment.SegmentIndex])
		{
			return EWorldChunkSegmentAcceptResult::Duplicate;
		}
		FMemory::Memcpy(
			Bytes.GetData() + Segment.SegmentIndex * SegmentPayloadBytes,
			Segment.Bytes.GetData(),
			ExpectedSegmentBytes);
		Received[Segment.SegmentIndex] = true;
		return IsComplete()
			? EWorldChunkSegmentAcceptResult::Completed
			: EWorldChunkSegmentAcceptResult::Accepted;
	}

	bool FWorldChunkSegmentAssembly::IsComplete() const
	{
		return bInitialized && !Bytes.IsEmpty()
			&& Received.CountSetBits() == Received.Num();
	}

	bool FWorldChunkSegmentAssembly::Build(FWorldCompressedChunk& OutChunk)
	{
		OutChunk = {};
		if (!IsComplete())
		{
			return false;
		}
		OutChunk.Coord = Offer.Coord;
		OutChunk.Revision = Offer.Revision;
		OutChunk.ContentHash = Offer.ContentHash;
		OutChunk.UncompressedSize = Offer.UncompressedSize;
		OutChunk.Bytes = MoveTemp(Bytes);
		return OutChunk.Bytes.Num() == Offer.CompressedSize
			&& FWorldChunkCodec::Hash(OutChunk.Bytes) == OutChunk.ContentHash
			&& OutChunk.IsValid();
	}
}
