#include "Storage/WorldStorageArchive.h"

#include "Algo/Sort.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr uint32 ManifestMagic = 0x4e414d57; // WMAN
	constexpr uint32 PackMagic = 0x4b415057; // WPAK
	constexpr uint16 PackFormatVersion = 1;
	constexpr int32 MaximumManifestPacks = 1000000;
	constexpr int32 MaximumPackChunks = 1000;

	struct FWorldManifestPackRef final
	{
		FWorldChunkPackCoord Coord;
		FString RelativePath;
		bool bSeed = false;
	};

	struct FWorldPackChunkEntry final
	{
		FWorldChunkCoord Coord;
		uint32 Revision = 0;
		FWorldChunkContentHash ContentHash;
		int64 DataOffset = 0;
		int32 CompressedSize = 0;
		int32 UncompressedSize = 0;
		int32 BuildingEntityCount = 0;
		int32 WorldObjectEntityCount = 0;
		int32 ElementEntityCount = 0;

		bool IsValid(const FWorldChunkPackCoord& PackCoord) const
		{
			return FWorldChunkPackCoord::FromChunk(Coord) == PackCoord && Revision != 0 && ContentHash.IsSet()
				&& DataOffset > 0 && CompressedSize > 0 && UncompressedSize > 0
				&& BuildingEntityCount >= 0 && WorldObjectEntityCount >= 0 && ElementEntityCount >= 0;
		}
	};

	struct FWorldPackIndex final
	{
		FWorldManifestPackRef Ref;
		uint64 Generation = 0;
		TArray<FWorldPackChunkEntry> Entries;
	};

	struct FWorldStorageManifest final
	{
		FWorldStorageManifestInfo Info;
		TMap<FWorldChunkPackCoord, FWorldManifestPackRef> Packs;
	};

	FString ResolvePackPath(
		const FString& WritableRoot,
		const FString& SeedRoot,
		const FWorldManifestPackRef& Ref)
	{
		return FPaths::Combine(Ref.bSeed ? SeedRoot : WritableRoot, Ref.RelativePath);
	}

	void SerializeChunkCoord(FArchive& Archive, FWorldChunkCoord& Coord)
	{
		Archive << Coord.X << Coord.Y << Coord.Z;
	}

	void SerializePackCoord(FArchive& Archive, FWorldChunkPackCoord& Coord)
	{
		Archive << Coord.X << Coord.Y << Coord.Z;
	}

	bool SaveBytesAtomically(const FString& Path, const TArray<uint8>& Bytes, FString& OutError)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*FPaths::GetPath(Path), true))
		{
			OutError = FString::Printf(TEXT("无法创建目录：%s"), *FPaths::GetPath(Path));
			return false;
		}
		const FString TemporaryPath = Path + TEXT(".tmp");
		if (!FFileHelper::SaveArrayToFile(Bytes, *TemporaryPath))
		{
			OutError = FString::Printf(TEXT("无法写入临时文件：%s"), *TemporaryPath);
			return false;
		}
		if (!FileManager.Move(*Path, *TemporaryPath, true, true))
		{
			FileManager.Delete(*TemporaryPath, false, true);
			OutError = FString::Printf(TEXT("无法原子替换文件：%s"), *Path);
			return false;
		}
		return true;
	}

	bool SaveManifest(const FString& Path, const FWorldStorageManifest& Manifest, FString& OutError)
	{
		TArray<FWorldManifestPackRef> SortedPacks;
		Manifest.Packs.GenerateValueArray(SortedPacks);
		Algo::Sort(SortedPacks, [](const FWorldManifestPackRef& Left, const FWorldManifestPackRef& Right)
		{
			return Left.Coord < Right.Coord;
		});
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes, true);
		uint32 Magic = ManifestMagic;
		uint16 Version = FWorldStorageManifestInfo::FormatVersion;
		Writer << Magic << Version;
		FGuid WorldId = Manifest.Info.WorldId;
		uint64 Generation = Manifest.Info.Generation;
		uint64 NextEntityId = Manifest.Info.NextEntityId;
		int64 SimulationTime = Manifest.Info.WorldSimulationTimeMilliseconds;
		int64 CheckpointTicks = Manifest.Info.LastCheckpointUtc.GetTicks();
		int64 StructureCount = Manifest.Info.CompleteStructureCount;
		int64 BuildingCount = Manifest.Info.BuildingEntityCount;
		int64 WorldObjectCount = Manifest.Info.WorldObjectEntityCount;
		Writer << WorldId << Generation << NextEntityId << SimulationTime << CheckpointTicks
			<< StructureCount << BuildingCount << WorldObjectCount;
		int32 PackCount = SortedPacks.Num();
		Writer << PackCount;
		for (FWorldManifestPackRef& Ref : SortedPacks)
		{
			SerializePackCoord(Writer, Ref.Coord);
			Writer << Ref.RelativePath << Ref.bSeed;
		}
		if (Writer.IsError() || !SaveBytesAtomically(Path, Bytes, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Manifest 序列化失败。");
			}
			return false;
		}
		return true;
	}

	bool LoadManifest(const FString& Path, FWorldStorageManifest& OutManifest, FString& OutError)
	{
		OutManifest = {};
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			OutError = FString::Printf(TEXT("无法读取 Manifest：%s"), *Path);
			return false;
		}
		FMemoryReader Reader(Bytes, true);
		uint32 Magic = 0;
		uint16 Version = 0;
		Reader << Magic << Version;
		if (Magic != ManifestMagic || Version != FWorldStorageManifestInfo::FormatVersion)
		{
			OutError = TEXT("世界存档格式版本不匹配；请重新生成种子存档。");
			return false;
		}
		int64 CheckpointTicks = 0;
		Reader << OutManifest.Info.WorldId << OutManifest.Info.Generation << OutManifest.Info.NextEntityId
			<< OutManifest.Info.WorldSimulationTimeMilliseconds << CheckpointTicks
			<< OutManifest.Info.CompleteStructureCount << OutManifest.Info.BuildingEntityCount
			<< OutManifest.Info.WorldObjectEntityCount;
		OutManifest.Info.LastCheckpointUtc = FDateTime(CheckpointTicks);
		int32 PackCount = 0;
		Reader << PackCount;
		if (!OutManifest.Info.IsValid() || PackCount < 0 || PackCount > MaximumManifestPacks)
		{
			OutError = TEXT("Manifest Header 无效。");
			return false;
		}
		for (int32 Index = 0; Index < PackCount; ++Index)
		{
			FWorldManifestPackRef Ref;
			SerializePackCoord(Reader, Ref.Coord);
			Reader << Ref.RelativePath << Ref.bSeed;
			if (Ref.RelativePath.IsEmpty() || OutManifest.Packs.Contains(Ref.Coord))
			{
				OutError = TEXT("Manifest 包含重复或空 Pack 引用。");
				return false;
			}
			OutManifest.Packs.Add(Ref.Coord, MoveTemp(Ref));
		}
		if (Reader.IsError() || Reader.Tell() != Reader.TotalSize())
		{
			OutError = TEXT("Manifest 未完整解码。");
			return false;
		}
		return true;
	}

	bool WritePack(
		const FString& Path,
		const FWorldChunkPackCoord& PackCoord,
		const uint64 Generation,
		TConstArrayView<FWorldCompressedChunk> Chunks,
		FString& OutError)
	{
		if (Chunks.IsEmpty() || Chunks.Num() > MaximumPackChunks)
		{
			OutError = TEXT("Pack Chunk 数无效。");
			return false;
		}
		TArray<FWorldCompressedChunk> Sorted(Chunks);
		Algo::Sort(Sorted, [](const FWorldCompressedChunk& Left, const FWorldCompressedChunk& Right)
		{
			return FWorldChunkPackCoord::GetLocalChunkIndex(Left.Coord)
				< FWorldChunkPackCoord::GetLocalChunkIndex(Right.Coord);
		});
		for (int32 Index = 0; Index < Sorted.Num(); ++Index)
		{
			if (!Sorted[Index].IsValid() || FWorldChunkPackCoord::FromChunk(Sorted[Index].Coord) != PackCoord
				|| (Index > 0 && Sorted[Index - 1].Coord == Sorted[Index].Coord))
			{
				OutError = TEXT("Pack 含无效、重复或跨 Pack 的 Chunk。");
				return false;
			}
		}

		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes, true);
		uint32 Magic = PackMagic;
		uint16 Version = PackFormatVersion;
		Writer << Magic << Version;
		FWorldChunkPackCoord MutablePackCoord = PackCoord;
		SerializePackCoord(Writer, MutablePackCoord);
		uint64 MutableGeneration = Generation;
		int32 ChunkCount = Sorted.Num();
		Writer << MutableGeneration << ChunkCount;
		const int64 TableOffset = Writer.Tell();
		FWorldPackChunkEntry EmptyEntry;
		for (int32 Index = 0; Index < Sorted.Num(); ++Index)
		{
			FWorldPackChunkEntry Entry = EmptyEntry;
			SerializeChunkCoord(Writer, Entry.Coord);
			Writer << Entry.Revision << Entry.ContentHash.High << Entry.ContentHash.Low << Entry.DataOffset
				<< Entry.CompressedSize << Entry.UncompressedSize << Entry.BuildingEntityCount
				<< Entry.WorldObjectEntityCount << Entry.ElementEntityCount;
		}
		TArray<FWorldPackChunkEntry> Entries;
		Entries.Reserve(Sorted.Num());
		for (const FWorldCompressedChunk& Chunk : Sorted)
		{
			FWorldPackChunkEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Coord = Chunk.Coord;
			Entry.Revision = Chunk.Revision;
			Entry.ContentHash = Chunk.ContentHash;
			Entry.DataOffset = Writer.Tell();
			Entry.CompressedSize = Chunk.Bytes.Num();
			Entry.UncompressedSize = Chunk.UncompressedSize;
			Entry.BuildingEntityCount = Chunk.BuildingEntityCount;
			Entry.WorldObjectEntityCount = Chunk.WorldObjectEntityCount;
			Entry.ElementEntityCount = Chunk.ElementEntityCount;
			Writer.Serialize(const_cast<uint8*>(Chunk.Bytes.GetData()), Chunk.Bytes.Num());
		}
		const int64 EndOffset = Writer.Tell();
		Writer.Seek(TableOffset);
		for (FWorldPackChunkEntry& Entry : Entries)
		{
			SerializeChunkCoord(Writer, Entry.Coord);
			Writer << Entry.Revision << Entry.ContentHash.High << Entry.ContentHash.Low << Entry.DataOffset
				<< Entry.CompressedSize << Entry.UncompressedSize << Entry.BuildingEntityCount
				<< Entry.WorldObjectEntityCount << Entry.ElementEntityCount;
		}
		Writer.Seek(EndOffset);
		if (Writer.IsError() || !SaveBytesAtomically(Path, Bytes, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Pack 序列化失败。");
			}
			return false;
		}
		return true;
	}

	bool ReadPackIndex(const FString& Path, const FWorldManifestPackRef& Ref, FWorldPackIndex& OutIndex, FString& OutError)
	{
		OutIndex = {};
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader)
		{
			OutError = FString::Printf(TEXT("无法读取 Pack：%s"), *Path);
			return false;
		}
			uint32 Magic = 0;
			uint16 Version = 0;
			*Reader << Magic;
			*Reader << Version;
		FWorldChunkPackCoord PackCoord;
		SerializePackCoord(*Reader, PackCoord);
		int32 ChunkCount = 0;
		*Reader << OutIndex.Generation << ChunkCount;
		if (Magic != PackMagic || Version != PackFormatVersion || PackCoord != Ref.Coord
			|| ChunkCount <= 0 || ChunkCount > MaximumPackChunks)
		{
			OutError = TEXT("Pack Header 或版本无效。");
			return false;
		}
		OutIndex.Ref = Ref;
		OutIndex.Entries.Reserve(ChunkCount);
		TSet<FWorldChunkCoord> Seen;
		for (int32 Index = 0; Index < ChunkCount; ++Index)
		{
			FWorldPackChunkEntry& Entry = OutIndex.Entries.AddDefaulted_GetRef();
			SerializeChunkCoord(*Reader, Entry.Coord);
			*Reader << Entry.Revision << Entry.ContentHash.High << Entry.ContentHash.Low << Entry.DataOffset
				<< Entry.CompressedSize << Entry.UncompressedSize << Entry.BuildingEntityCount
				<< Entry.WorldObjectEntityCount << Entry.ElementEntityCount;
			if (!Entry.IsValid(PackCoord) || Entry.DataOffset + Entry.CompressedSize > Reader->TotalSize()
				|| Seen.Contains(Entry.Coord))
			{
				OutError = TEXT("Pack TOC 含无效或重复条目。");
				return false;
			}
			Seen.Add(Entry.Coord);
		}
		return !Reader->IsError();
	}
}

class FWorldStorageArchiveData final
{
public:
	bool ReloadIndexes(FString& OutError)
	{
		PackIndices.Reset();
		ChunkEntries.Reset();
		bHasMostPopulatedChunk = false;
		MostPopulatedChunk = {};
		FWorldChunkCoord MinimumCoord(MAX_int32, MAX_int32, MAX_int32);
		FWorldChunkCoord MaximumCoord(MIN_int32, MIN_int32, MIN_int32);
		for (const TPair<FWorldChunkPackCoord, FWorldManifestPackRef>& Pair : Manifest.Packs)
		{
			FWorldPackIndex Index;
			const FString Path = ResolvePackPath(WritableRoot, SeedRoot, Pair.Value);
			if (!ReadPackIndex(Path, Pair.Value, Index, OutError))
			{
				return false;
			}
			for (const FWorldPackChunkEntry& Entry : Index.Entries)
			{
				if (ChunkEntries.Contains(Entry.Coord))
				{
					OutError = TEXT("多个 Pack 声明了同一个逻辑 Chunk。");
					return false;
				}
				ChunkEntries.Add(Entry.Coord, Entry);
				MinimumCoord.X = FMath::Min(MinimumCoord.X, Entry.Coord.X);
				MinimumCoord.Y = FMath::Min(MinimumCoord.Y, Entry.Coord.Y);
				MinimumCoord.Z = FMath::Min(MinimumCoord.Z, Entry.Coord.Z);
				MaximumCoord.X = FMath::Max(MaximumCoord.X, Entry.Coord.X);
				MaximumCoord.Y = FMath::Max(MaximumCoord.Y, Entry.Coord.Y);
				MaximumCoord.Z = FMath::Max(MaximumCoord.Z, Entry.Coord.Z);
			}
			PackIndices.Add(Pair.Key, MoveTemp(Index));
		}
		if (!ChunkEntries.IsEmpty())
		{
			const double CenterX = 0.5 * (static_cast<double>(MinimumCoord.X) + MaximumCoord.X);
			const double CenterY = 0.5 * (static_cast<double>(MinimumCoord.Y) + MaximumCoord.Y);
			int64 BestPopulation = -1;
			double BestCenterDistanceSquared = TNumericLimits<double>::Max();
			int32 BestAbsoluteZ = MAX_int32;
			for (const TPair<FWorldChunkCoord, FWorldPackChunkEntry>& ChunkPair : ChunkEntries)
			{
				const FWorldPackChunkEntry& Entry = ChunkPair.Value;
				const int64 Population = static_cast<int64>(Entry.BuildingEntityCount)
					+ Entry.WorldObjectEntityCount;
				const double DeltaX = static_cast<double>(Entry.Coord.X) - CenterX;
				const double DeltaY = static_cast<double>(Entry.Coord.Y) - CenterY;
				const double CenterDistanceSquared = DeltaX * DeltaX + DeltaY * DeltaY;
				const int32 AbsoluteZ = FMath::Abs(Entry.Coord.Z);
				const bool bBetter = Population > BestPopulation
					|| (Population == BestPopulation && CenterDistanceSquared < BestCenterDistanceSquared)
					|| (Population == BestPopulation && CenterDistanceSquared == BestCenterDistanceSquared
						&& AbsoluteZ < BestAbsoluteZ)
					|| (Population == BestPopulation && CenterDistanceSquared == BestCenterDistanceSquared
						&& AbsoluteZ == BestAbsoluteZ && Entry.Coord < MostPopulatedChunk);
				if (bBetter)
				{
					BestPopulation = Population;
					BestCenterDistanceSquared = CenterDistanceSquared;
					BestAbsoluteZ = AbsoluteZ;
					MostPopulatedChunk = Entry.Coord;
					bHasMostPopulatedChunk = true;
				}
			}
		}
		return true;
	}

	bool ReadCompressedChunkUnlocked(const FWorldChunkCoord& Coord, FWorldCompressedChunk& OutChunk, FString& OutError) const
	{
		OutChunk = {};
		const FWorldPackChunkEntry* Entry = ChunkEntries.Find(Coord);
		const FWorldChunkPackCoord PackCoord = FWorldChunkPackCoord::FromChunk(Coord);
		const FWorldPackIndex* Index = PackIndices.Find(PackCoord);
		if (!Entry || !Index)
		{
			OutError = TEXT("逻辑 Chunk 不存在。");
			return false;
		}
		const FString Path = ResolvePackPath(WritableRoot, SeedRoot, Index->Ref);
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader)
		{
			OutError = FString::Printf(TEXT("无法打开 Pack：%s"), *Path);
			return false;
		}
		Reader->Seek(Entry->DataOffset);
		OutChunk.Coord = Entry->Coord;
		OutChunk.Revision = Entry->Revision;
		OutChunk.ContentHash = Entry->ContentHash;
		OutChunk.UncompressedSize = Entry->UncompressedSize;
		OutChunk.BuildingEntityCount = Entry->BuildingEntityCount;
		OutChunk.WorldObjectEntityCount = Entry->WorldObjectEntityCount;
		OutChunk.ElementEntityCount = Entry->ElementEntityCount;
		OutChunk.Bytes.SetNumUninitialized(Entry->CompressedSize);
		Reader->Serialize(OutChunk.Bytes.GetData(), Entry->CompressedSize);
		if (Reader->IsError() || !OutChunk.IsValid())
		{
			OutChunk = {};
			OutError = TEXT("读取 Pack 中的 Chunk Blob 失败。");
			return false;
		}
		return true;
	}

	mutable FCriticalSection Mutex;
	FString WritableRoot;
	FString SeedRoot;
	FWorldStorageManifest Manifest;
	TMap<FWorldChunkPackCoord, FWorldPackIndex> PackIndices;
	TMap<FWorldChunkCoord, FWorldPackChunkEntry> ChunkEntries;
	FWorldChunkCoord MostPopulatedChunk;
	bool bHasMostPopulatedChunk = false;
	bool bOpen = false;
};

class FWorldSeedArchiveWriterData final
{
public:
	FString Root;
	FWorldStorageManifest Manifest;
	TSet<FWorldChunkPackCoord> WrittenPacks;
	TSet<FWorldChunkCoord> WrittenChunks;
	bool bOpen = false;
	bool bCommitted = false;
};

bool FWorldStorageManifestInfo::IsValid() const
{
	return WorldId.IsValid() && NextEntityId != 0 && WorldSimulationTimeMilliseconds >= 0
		&& LastCheckpointUtc.GetTicks() > 0 && CompleteStructureCount >= 0
		&& BuildingEntityCount >= 0 && WorldObjectEntityCount >= 0;
}

void FWorldStorageArchiveDeleter::operator()(FWorldStorageArchiveData* Data) const
{
	delete Data;
}

void FWorldSeedArchiveWriterDeleter::operator()(FWorldSeedArchiveWriterData* Data) const
{
	delete Data;
}

FWorldSeedArchiveWriter::FWorldSeedArchiveWriter()
	: Data(new FWorldSeedArchiveWriterData())
{
}

FWorldSeedArchiveWriter::~FWorldSeedArchiveWriter() = default;

bool FWorldSeedArchiveWriter::Begin(
	const FString& Root, const FWorldStorageManifestInfo& ManifestInfo, FString& OutError)
{
	OutError.Reset();
	if (!Data || Root.IsEmpty() || !ManifestInfo.IsValid() || Data->bOpen)
	{
		OutError = TEXT("种子流式写入器输入或状态无效。");
		return false;
	}

	Data->Root = FPaths::ConvertRelativePathToFull(Root);
	const FString ManifestPath = FPaths::Combine(Data->Root, TEXT("World.manifest"));
	if (IFileManager::Get().FileExists(*ManifestPath))
	{
		OutError = FString::Printf(TEXT("种子目标已经包含 Manifest：%s"), *ManifestPath);
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*Data->Root, true))
	{
		OutError = FString::Printf(TEXT("无法创建种子目标目录：%s"), *Data->Root);
		return false;
	}

	Data->Manifest = {};
	Data->Manifest.Info = ManifestInfo;
	Data->WrittenPacks.Reset();
	Data->WrittenChunks.Reset();
	Data->bCommitted = false;
	Data->bOpen = true;
	return true;
}

bool FWorldSeedArchiveWriter::WritePack(
	const FWorldChunkPackCoord& PackCoord,
	const TConstArrayView<FWorldCompressedChunk> Chunks,
	FString& OutError)
{
	OutError.Reset();
	if (!Data || !Data->bOpen || Data->bCommitted || Chunks.IsEmpty() || Data->WrittenPacks.Contains(PackCoord))
	{
		OutError = TEXT("种子 Pack 提交状态无效或 Pack 已经写入。");
		return false;
	}
	for (const FWorldCompressedChunk& Chunk : Chunks)
	{
		if (!Chunk.IsValid() || FWorldChunkPackCoord::FromChunk(Chunk.Coord) != PackCoord
			|| Data->WrittenChunks.Contains(Chunk.Coord))
		{
			OutError = TEXT("种子 Pack 含无效、跨 Pack 或重复 Chunk。");
			return false;
		}
	}

	const FString RelativePath = FString::Printf(
		TEXT("Packs/Pack_%d_%d_%d_g%llu.wpack"),
		PackCoord.X, PackCoord.Y, PackCoord.Z, Data->Manifest.Info.Generation);
	if (!::WritePack(
		FPaths::Combine(Data->Root, RelativePath), PackCoord, Data->Manifest.Info.Generation, Chunks, OutError))
	{
		return false;
	}

	FWorldManifestPackRef Ref;
	Ref.Coord = PackCoord;
	Ref.RelativePath = RelativePath;
	Data->Manifest.Packs.Add(PackCoord, MoveTemp(Ref));
	Data->WrittenPacks.Add(PackCoord);
	for (const FWorldCompressedChunk& Chunk : Chunks)
	{
		Data->WrittenChunks.Add(Chunk.Coord);
	}
	return true;
}

bool FWorldSeedArchiveWriter::Commit(FString& OutError)
{
	OutError.Reset();
	if (!Data || !Data->bOpen || Data->bCommitted || Data->Manifest.Packs.IsEmpty())
	{
		OutError = TEXT("种子流式写入器没有可提交的 Pack。");
		return false;
	}
	if (!SaveManifest(FPaths::Combine(Data->Root, TEXT("World.manifest")), Data->Manifest, OutError))
	{
		return false;
	}
	Data->bCommitted = true;
	Data->bOpen = false;
	return true;
}

bool FWorldSeedArchiveWriter::IsOpen() const
{
	return Data && Data->bOpen && !Data->bCommitted;
}

FWorldStorageArchive::FWorldStorageArchive()
	: Data(new FWorldStorageArchiveData())
{
}

FWorldStorageArchive::~FWorldStorageArchive() = default;

bool FWorldStorageArchive::Open(const FWorldStorageOpenOptions& Options, FString& OutError)
{
	OutError.Reset();
	if (!Data || Options.WritableRoot.IsEmpty())
	{
		OutError = TEXT("WorldStorage WritableRoot 为空。");
		return false;
	}
	FScopeLock Lock(&Data->Mutex);
	Data->WritableRoot = FPaths::ConvertRelativePathToFull(Options.WritableRoot);
	Data->SeedRoot = Options.SeedRoot.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Options.SeedRoot);
	const FString WritableManifestPath = FPaths::Combine(Data->WritableRoot, TEXT("World.manifest"));
	const FString SeedManifestPath = FPaths::Combine(Data->SeedRoot, TEXT("World.manifest"));
	if (IFileManager::Get().FileExists(*WritableManifestPath))
	{
		if (!LoadManifest(WritableManifestPath, Data->Manifest, OutError))
		{
			return false;
		}
	}
	else if (!Data->SeedRoot.IsEmpty() && IFileManager::Get().FileExists(*SeedManifestPath))
	{
		if (!LoadManifest(SeedManifestPath, Data->Manifest, OutError))
		{
			return false;
		}
		for (TPair<FWorldChunkPackCoord, FWorldManifestPackRef>& Pair : Data->Manifest.Packs)
		{
			Pair.Value.bSeed = true;
		}
		// The seed timestamp describes when the immutable template was generated, not when this
		// playable world last simulated.  Rebase only the newly-created writable manifest so the
		// first boot cannot accidentally settle months of offline time.
		Data->Manifest.Info.LastCheckpointUtc = FDateTime::UtcNow();
		if (!SaveManifest(WritableManifestPath, Data->Manifest, OutError))
		{
			return false;
		}
	}
	else if (Options.bCreateIfMissing)
	{
		Data->Manifest.Info.WorldId = FGuid::NewGuid();
		Data->Manifest.Info.Generation = 0;
		Data->Manifest.Info.NextEntityId = 1;
		Data->Manifest.Info.WorldSimulationTimeMilliseconds = 0;
		Data->Manifest.Info.LastCheckpointUtc = FDateTime::UtcNow();
		if (!SaveManifest(WritableManifestPath, Data->Manifest, OutError))
		{
			return false;
		}
	}
	else
	{
		OutError = TEXT("世界 Manifest 不存在。");
		return false;
	}
	if (!Data->ReloadIndexes(OutError))
	{
		return false;
	}
	Data->bOpen = true;
	return true;
}

bool FWorldStorageArchive::IsOpen() const
{
	return Data && Data->bOpen;
}

FWorldStorageManifestInfo FWorldStorageArchive::GetManifestInfo() const
{
	if (!Data)
	{
		return {};
	}
	FScopeLock Lock(&Data->Mutex);
	return Data->Manifest.Info;
}

FWorldStorageArchiveStats FWorldStorageArchive::GetArchiveStats() const
{
	FWorldStorageArchiveStats Stats;
	if (!Data)
	{
		return Stats;
	}
	FScopeLock Lock(&Data->Mutex);
	Stats.PackCount = Data->PackIndices.Num();
	Stats.OccupiedChunkCount = Data->ChunkEntries.Num();
	return Stats;
}

void FWorldStorageArchive::GetOccupiedChunkCoords(TArray<FWorldChunkCoord>& OutChunks) const
{
	OutChunks.Reset();
	if (!Data || !Data->bOpen)
	{
		return;
	}
	FScopeLock Lock(&Data->Mutex);
	Data->ChunkEntries.GenerateKeyArray(OutChunks);
	Algo::Sort(OutChunks);
}

bool FWorldStorageArchive::TryGetMostPopulatedChunk(FWorldChunkCoord& OutChunk) const
{
	OutChunk = {};
	if (!Data || !Data->bOpen)
	{
		return false;
	}
	FScopeLock Lock(&Data->Mutex);
	if (!Data->bHasMostPopulatedChunk)
	{
		return false;
	}
	OutChunk = Data->MostPopulatedChunk;
	return true;
}

bool FWorldStorageArchive::ReadCompressedChunk(
	const FWorldChunkCoord& Coord, FWorldCompressedChunk& OutChunk, FString& OutError) const
{
	if (!Data || !Data->bOpen)
	{
		OutError = TEXT("WorldStorage Archive 尚未打开。");
		return false;
	}
	FScopeLock Lock(&Data->Mutex);
	return Data->ReadCompressedChunkUnlocked(Coord, OutChunk, OutError);
}

bool FWorldStorageArchive::TryGetChunkOffer(const FWorldChunkCoord& Coord, FWorldChunkOffer& OutOffer) const
{
	if (!Data || !Data->bOpen)
	{
		return false;
	}
	FScopeLock Lock(&Data->Mutex);
	const FWorldPackChunkEntry* Entry = Data->ChunkEntries.Find(Coord);
	if (!Entry)
	{
		return false;
	}
	OutOffer.WorldId = Data->Manifest.Info.WorldId;
	OutOffer.Coord = Coord;
	OutOffer.Revision = Entry->Revision;
	OutOffer.ContentHash = Entry->ContentHash;
	OutOffer.CompressedSize = Entry->CompressedSize;
	OutOffer.UncompressedSize = Entry->UncompressedSize;
	return true;
}

void FWorldStorageArchive::QueryOccupiedChunks(const FWorldChunkBox& Box, TArray<FWorldChunkCoord>& OutChunks) const
{
	OutChunks.Reset();
	if (!Data || !Data->bOpen)
	{
		return;
	}
	FScopeLock Lock(&Data->Mutex);
	const FWorldChunkCoord MaximumInclusive(
		Box.MaximumExclusive.X - 1,
		Box.MaximumExclusive.Y - 1,
		Box.MaximumExclusive.Z - 1);
	const FWorldChunkPackCoord MinPack = FWorldChunkPackCoord::FromChunk(Box.Minimum);
	const FWorldChunkPackCoord MaxPack = FWorldChunkPackCoord::FromChunk(MaximumInclusive);
	for (int32 Z = MinPack.Z; Z <= MaxPack.Z; ++Z)
	{
		for (int32 Y = MinPack.Y; Y <= MaxPack.Y; ++Y)
		{
			for (int32 X = MinPack.X; X <= MaxPack.X; ++X)
			{
				const FWorldPackIndex* Index = Data->PackIndices.Find(FWorldChunkPackCoord(X, Y, Z));
				if (!Index)
				{
					continue;
				}
				for (const FWorldPackChunkEntry& Entry : Index->Entries)
				{
					if (Box.Contains(Entry.Coord))
					{
						OutChunks.Add(Entry.Coord);
					}
				}
			}
		}
	}
	Algo::Sort(OutChunks);
}

bool FWorldStorageArchive::WriteCheckpoint(
	const TConstArrayView<FWorldChunkCheckpointChange> Changes,
	const FWorldStorageManifestInfo& NewManifestInfo,
	FString& OutError)
{
	OutError.Reset();
	if (!Data || !Data->bOpen || !NewManifestInfo.IsValid()
		|| NewManifestInfo.WorldId != Data->Manifest.Info.WorldId
		|| NewManifestInfo.Generation <= Data->Manifest.Info.Generation)
	{
		OutError = TEXT("Checkpoint Manifest 状态无效。");
		return false;
	}
	FScopeLock Lock(&Data->Mutex);
	TMap<FWorldChunkPackCoord, TArray<const FWorldChunkCheckpointChange*>> ChangesByPack;
	for (const FWorldChunkCheckpointChange& Change : Changes)
	{
		if (Change.Chunk.IsSet() && (!Change.Chunk->IsValid() || Change.Chunk->Coord != Change.Coord))
		{
			OutError = TEXT("Checkpoint 包含无效 Chunk 变更。");
			return false;
		}
		ChangesByPack.FindOrAdd(FWorldChunkPackCoord::FromChunk(Change.Coord)).Add(&Change);
	}

	FWorldStorageManifest NewManifest = Data->Manifest;
	NewManifest.Info = NewManifestInfo;
	for (const TPair<FWorldChunkPackCoord, TArray<const FWorldChunkCheckpointChange*>>& Pair : ChangesByPack)
	{
		TMap<FWorldChunkCoord, FWorldCompressedChunk> PackChunks;
		if (const FWorldPackIndex* ExistingIndex = Data->PackIndices.Find(Pair.Key))
		{
			for (const FWorldPackChunkEntry& ExistingEntry : ExistingIndex->Entries)
			{
				FWorldCompressedChunk ExistingChunk;
				if (!Data->ReadCompressedChunkUnlocked(ExistingEntry.Coord, ExistingChunk, OutError))
				{
					return false;
				}
				PackChunks.Add(ExistingEntry.Coord, MoveTemp(ExistingChunk));
			}
		}
		for (const FWorldChunkCheckpointChange* Change : Pair.Value)
		{
			if (Change->Chunk.IsSet())
			{
				PackChunks.Add(Change->Coord, Change->Chunk.GetValue());
			}
			else
			{
				PackChunks.Remove(Change->Coord);
			}
		}
		if (PackChunks.IsEmpty())
		{
			NewManifest.Packs.Remove(Pair.Key);
			continue;
		}
		TArray<FWorldCompressedChunk> SortedChunks;
		PackChunks.GenerateValueArray(SortedChunks);
		const FString RelativePath = FString::Printf(
			TEXT("Packs/Pack_%d_%d_%d_g%llu.wpack"),
			Pair.Key.X, Pair.Key.Y, Pair.Key.Z, NewManifestInfo.Generation);
		const FString FullPath = FPaths::Combine(Data->WritableRoot, RelativePath);
		if (!WritePack(FullPath, Pair.Key, NewManifestInfo.Generation, SortedChunks, OutError))
		{
			return false;
		}
		FWorldManifestPackRef Ref;
		Ref.Coord = Pair.Key;
		Ref.RelativePath = RelativePath;
		Ref.bSeed = false;
		NewManifest.Packs.Add(Pair.Key, MoveTemp(Ref));
	}
	const FString ManifestPath = FPaths::Combine(Data->WritableRoot, TEXT("World.manifest"));
	if (!SaveManifest(ManifestPath, NewManifest, OutError))
	{
		return false;
	}
	Data->Manifest = MoveTemp(NewManifest);
	return Data->ReloadIndexes(OutError);
}

bool FWorldStorageArchive::WriteSeedArchive(
	const FString& Root,
	const FWorldStorageManifestInfo& ManifestInfo,
	const TConstArrayView<FWorldCompressedChunk> Chunks,
	FString& OutError)
{
	OutError.Reset();
	if (Root.IsEmpty() || !ManifestInfo.IsValid() || Chunks.IsEmpty())
	{
		OutError = TEXT("种子存档输入无效。");
		return false;
	}
	TMap<FWorldChunkPackCoord, TArray<FWorldCompressedChunk>> ByPack;
	TSet<FWorldChunkCoord> Seen;
	for (const FWorldCompressedChunk& Chunk : Chunks)
	{
		if (!Chunk.IsValid() || Seen.Contains(Chunk.Coord))
		{
			OutError = TEXT("种子存档含无效或重复 Chunk。");
			return false;
		}
		Seen.Add(Chunk.Coord);
		ByPack.FindOrAdd(FWorldChunkPackCoord::FromChunk(Chunk.Coord)).Add(Chunk);
	}
	FWorldSeedArchiveWriter Writer;
	if (!Writer.Begin(Root, ManifestInfo, OutError))
	{
		return false;
	}
	for (const TPair<FWorldChunkPackCoord, TArray<FWorldCompressedChunk>>& Pair : ByPack)
	{
		if (!Writer.WritePack(Pair.Key, Pair.Value, OutError))
		{
			return false;
		}
	}
	return Writer.Commit(OutError);
}
