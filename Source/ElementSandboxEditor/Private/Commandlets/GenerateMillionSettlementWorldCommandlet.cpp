#include "Commandlets/GenerateMillionSettlementWorldCommandlet.h"

#include "Algo/Sort.h"
#include "Chunk/WorldChunkCoordinates.h"
#include "City/CityBuildingPieceDefinition.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Storage/WorldChunkCodec.h"
#include "Storage/WorldStorageArchive.h"
#include "Torch/TorchDefinition.h"
#include "Tree/SettlementTreeTypes.h"
#include "WorldSeed/MillionSettlementSeedLayout.h"
#include "WorldSeed/MillionBuildingRecipe.h"

DEFINE_LOG_CATEGORY_STATIC(LogMillionSettlementWorldCommandlet, Log, All);

namespace
{
	using namespace UE::ElementSandbox::WorldSeed;

#pragma pack(push, 1)
	struct FSeedSpoolAddress final
	{
		uint32 StructureIndex = 0;
		uint16 PieceIndex = 0;
	};
#pragma pack(pop)

	static_assert(sizeof(FSeedSpoolAddress) == 6, "Spool 地址必须保持紧凑；它只在一次 Commandlet 运行内使用。");
	static constexpr uint16 MountedTorchSpoolPieceIndexBase = MAX_uint16 - MountedTorchFixturesPerStructure;
	static constexpr uint16 TreeSpoolPieceIndex = MAX_uint16;

	struct FPackSpoolState final
	{
		FString Path;
		TArray<uint8> BufferedBytes;
		int64 RecordCount = 0;
	};

	class FTemporarySeedDirectoryGuard final
	{
	public:
		explicit FTemporarySeedDirectoryGuard(FString InPath) : Path(MoveTemp(InPath)) {}
		~FTemporarySeedDirectoryGuard()
		{
			if (bArmed && !Path.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Path, false, true);
			}
		}
		void Release() { bArmed = false; }

	private:
		FString Path;
		bool bArmed = true;
	};

		FName ResolvePieceDefinitionId(const FCityBuildingPieceRecipe& Piece)
		{
			return Piece.Kind == ECityBuildingPieceKind::Door
				? FName(TEXT("Settlement.Door"))
				: GetCityBuildingPieceDefinitionId(Piece.Kind, Piece.SurfaceProfileId);
	}

	FString MakeSpoolPath(const FString& SpoolRoot, const FWorldChunkPackCoord& PackCoord)
	{
		return FPaths::Combine(SpoolRoot, FString::Printf(
			TEXT("Pack_%d_%d_%d.spool"), PackCoord.X, PackCoord.Y, PackCoord.Z));
	}

	bool FlushSpoolBuffers(TMap<FWorldChunkPackCoord, FPackSpoolState>& Spools, int64& InOutBufferedBytes, FString& OutError)
	{
		for (TPair<FWorldChunkPackCoord, FPackSpoolState>& Pair : Spools)
		{
			FPackSpoolState& Spool = Pair.Value;
			if (Spool.BufferedBytes.IsEmpty())
			{
				continue;
			}
			TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Spool.Path, FILEWRITE_Append));
			if (!Writer)
			{
				OutError = FString::Printf(TEXT("无法打开 Pack 临时流：%s"), *Spool.Path);
				return false;
			}
			Writer->Serialize(Spool.BufferedBytes.GetData(), Spool.BufferedBytes.Num());
			if (Writer->IsError())
			{
				OutError = FString::Printf(TEXT("写入 Pack 临时流失败：%s"), *Spool.Path);
				return false;
			}
			InOutBufferedBytes -= Spool.BufferedBytes.Num();
			Spool.BufferedBytes.Reset();
		}
		check(InOutBufferedBytes == 0);
		return true;
	}
}

UGenerateMillionSettlementWorldCommandlet::UGenerateMillionSettlementWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UGenerateMillionSettlementWorldCommandlet::Main(const FString& Params)
{
	using namespace UE::ElementSandbox::WorldSeed;

	int32 StructureCount = CompleteStructureCount;
	int32 Seed = DefaultSeed;
	FString TreeModeValue = TEXT("OnePerStructure");
	FParse::Value(*Params, TEXT("StructureCount="), StructureCount);
	FParse::Value(*Params, TEXT("Seed="), Seed);
	FParse::Value(*Params, TEXT("TreeMode="), TreeModeValue);
	const bool bValidateOnly = FParse::Param(*Params, TEXT("ValidateOnly"));
	ESettlementTreeMode TreeMode = ESettlementTreeMode::OnePerStructure;
	if (!TryParseTreeMode(TreeModeValue, TreeMode))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("TreeMode 必须是 None 或 OnePerStructure，实际为 %s。"), *TreeModeValue);
		return 1;
	}
	if (StructureCount <= 0 || StructureCount > CompleteStructureCount)
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("StructureCount 必须位于 [1, %d]。"), CompleteStructureCount);
		return 1;
	}

	TArray<UCityBuildingRecipe*> Recipes;
	FString Error;
	if (!BuildRecipeCatalog(this, Recipes, Error))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	TArray<int64> StructureHistogram;
	TArray<uint8> ArchetypeAssignments;
	int64 RecipeBuildingEntityCount = 0;
	if (!BuildArchetypeAssignments(
		StructureCount, Recipes, Seed, ArchetypeAssignments, RecipeBuildingEntityCount, Error))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	StructureHistogram.Init(0, Recipes.Num());
	for (const uint8 ArchetypeIndex : ArchetypeAssignments)
	{
		++StructureHistogram[ArchetypeIndex];
	}
	const int64 TreeWorldObjectCount = TreeMode == ESettlementTreeMode::OnePerStructure
		? StructureCount
		: 0;
	const int64 MountedTorchBuildingEntityCount =
		static_cast<int64>(StructureCount) * MountedTorchFixturesPerStructure;
	const int64 BuildingEntityCount = RecipeBuildingEntityCount + MountedTorchBuildingEntityCount;
	if (RecipeBuildingEntityCount <= 0
		|| (StructureCount == CompleteStructureCount
			&& (RecipeBuildingEntityCount != ExpectedRecipeBuildingEntityCount
				|| MountedTorchBuildingEntityCount != ExpectedMountedTorchBuildingEntityCount
				|| BuildingEntityCount != ExpectedBuildingEntityCount)))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("确定性计数失败：Structures=%d, RecipeBuilding=%lld, MountedTorch=%lld, Building=%lld/%lld。"),
			StructureCount, RecipeBuildingEntityCount, MountedTorchBuildingEntityCount,
			BuildingEntityCount, ExpectedBuildingEntityCount);
		return 1;
	}
	UE_LOG(LogMillionSettlementWorldCommandlet, Display,
		TEXT("离线布局已验证：%d Structures，%lld Recipe Building + %lld Mounted Torch = %lld Building，%lld Tree WorldObject；TreeMode=%s。"),
		StructureCount, RecipeBuildingEntityCount, MountedTorchBuildingEntityCount,
		BuildingEntityCount, TreeWorldObjectCount, LexToString(TreeMode));
	if (bValidateOnly)
	{
		return 0;
	}

	const FString WorldSeedsRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldSeeds")));
	FString OutputArgument;
	FParse::Value(*Params, TEXT("Output="), OutputArgument);
	FString OutputRoot = OutputArgument.IsEmpty()
		? FPaths::Combine(WorldSeedsRoot, TEXT("MillionSettlement"))
		: (FPaths::IsRelative(OutputArgument)
			? FPaths::Combine(WorldSeedsRoot, OutputArgument)
			: FPaths::ConvertRelativePathToFull(OutputArgument));
	FPaths::NormalizeDirectoryName(OutputRoot);
	if (!FPaths::IsUnderDirectory(OutputRoot, WorldSeedsRoot))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("种子输出必须位于 Saved/WorldSeeds 下：%s"), *OutputRoot);
		return 1;
	}
	if (IFileManager::Get().DirectoryExists(*OutputRoot) || IFileManager::Get().FileExists(*OutputRoot))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("输出目标已存在；为避免覆盖存档，本次拒绝写入：%s"), *OutputRoot);
		return 1;
	}
	if (!IFileManager::Get().MakeDirectory(*WorldSeedsRoot, true))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("无法创建 WorldSeeds 目录：%s"), *WorldSeedsRoot);
		return 1;
	}

	const FString GenerationToken = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString StagingRoot = FPaths::Combine(WorldSeedsRoot, TEXT(".Generating_") + GenerationToken);
	const FString SpoolRoot = FPaths::Combine(StagingRoot, TEXT("_Spool"));
	FTemporarySeedDirectoryGuard StagingGuard(StagingRoot);
	if (!IFileManager::Get().MakeDirectory(*SpoolRoot, true))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("无法创建临时流目录：%s"), *SpoolRoot);
		return 1;
	}

	FWorldStorageManifestInfo ManifestInfo;
		ManifestInfo.WorldId = MakeWorldId(Seed, StructureCount, TreeMode);
	ManifestInfo.Generation = 1;
	ManifestInfo.NextEntityId = static_cast<uint64>(BuildingEntityCount + TreeWorldObjectCount) + 1;
	ManifestInfo.WorldSimulationTimeMilliseconds = 0;
	ManifestInfo.LastCheckpointUtc = FDateTime(2026, 1, 1);
	ManifestInfo.CompleteStructureCount = StructureCount;
	ManifestInfo.BuildingEntityCount = BuildingEntityCount;
	ManifestInfo.WorldObjectEntityCount = TreeWorldObjectCount;
	FWorldSeedArchiveWriter ArchiveWriter;
	if (!ArchiveWriter.Begin(StagingRoot, ManifestInfo, Error))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}

	TArray<uint64> FirstEntityIdByStructure;
	FirstEntityIdByStructure.SetNumUninitialized(StructureCount + 1);
	TMap<FWorldChunkPackCoord, FPackSpoolState> Spools;
	int64 BufferedSpoolBytes = 0;
	uint64 NextRecipeBuildingEntityId = 1;
	static constexpr int64 MaximumBufferedSpoolBytes = 64ll * 1024ll * 1024ll;
	for (uint32 StructureIndex = 0; StructureIndex < static_cast<uint32>(StructureCount); ++StructureIndex)
	{
		FirstEntityIdByStructure[StructureIndex] = NextRecipeBuildingEntityId;
		const uint8 ArchetypeIndex = ArchetypeAssignments[StructureIndex];
		UCityBuildingRecipe* Recipe = Recipes.IsValidIndex(ArchetypeIndex) ? Recipes[ArchetypeIndex] : nullptr;
		if (!Recipe)
		{
			UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("结构 %u 的配方无效。"), StructureIndex);
			return 1;
		}
		const FTransform StructureTransform = ResolveStructureTransform(StructureIndex, Seed);
		const TConstArrayView<FCityBuildingPieceRecipe> Pieces = Recipe->GetPieces();
		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			const FTransform WorldTransform = Pieces[PieceIndex].LocalTransform * StructureTransform;
			const FWorldChunkCoord ChunkCoord = FWorldChunkCoord::FromWorldLocation(WorldTransform.GetLocation());
			const FWorldChunkPackCoord PackCoord = FWorldChunkPackCoord::FromChunk(ChunkCoord);
			FPackSpoolState& Spool = Spools.FindOrAdd(PackCoord);
			if (Spool.Path.IsEmpty())
			{
				Spool.Path = MakeSpoolPath(SpoolRoot, PackCoord);
			}
			FSeedSpoolAddress Address;
			Address.StructureIndex = StructureIndex;
			Address.PieceIndex = static_cast<uint16>(PieceIndex);
			const int32 WriteOffset = Spool.BufferedBytes.AddUninitialized(sizeof(Address));
			FMemory::Memcpy(Spool.BufferedBytes.GetData() + WriteOffset, &Address, sizeof(Address));
			BufferedSpoolBytes += sizeof(Address);
			++Spool.RecordCount;
		}
		NextRecipeBuildingEntityId += Pieces.Num();
		const TConstArrayView<FTransform> MountedTorchTransforms =
			Recipe->GetMountedTorchLocalTransforms();
		if (MountedTorchTransforms.Num() != MountedTorchFixturesPerStructure)
		{
			UE_LOG(LogMillionSettlementWorldCommandlet, Error,
				TEXT("结构 %u 的挂墙火把插槽数不是 %d。"),
				StructureIndex, MountedTorchFixturesPerStructure);
			return 1;
		}
		for (int32 FixtureIndex = 0; FixtureIndex < MountedTorchTransforms.Num(); ++FixtureIndex)
		{
			const FTransform TorchTransform = MountedTorchTransforms[FixtureIndex] * StructureTransform;
			const FWorldChunkCoord TorchChunk = FWorldChunkCoord::FromWorldLocation(
				TorchTransform.GetLocation());
			const FWorldChunkPackCoord TorchPack = FWorldChunkPackCoord::FromChunk(TorchChunk);
			FPackSpoolState& TorchSpool = Spools.FindOrAdd(TorchPack);
			if (TorchSpool.Path.IsEmpty())
			{
				TorchSpool.Path = MakeSpoolPath(SpoolRoot, TorchPack);
			}
			FSeedSpoolAddress TorchAddress;
			TorchAddress.StructureIndex = StructureIndex;
			TorchAddress.PieceIndex = static_cast<uint16>(
				MountedTorchSpoolPieceIndexBase + FixtureIndex);
			const int32 WriteOffset = TorchSpool.BufferedBytes.AddUninitialized(sizeof(TorchAddress));
			FMemory::Memcpy(
				TorchSpool.BufferedBytes.GetData() + WriteOffset,
				&TorchAddress,
				sizeof(TorchAddress));
			BufferedSpoolBytes += sizeof(TorchAddress);
			++TorchSpool.RecordCount;
		}
		if (TreeMode == ESettlementTreeMode::OnePerStructure)
		{
			const FTransform TreeTransform = ResolveSettlementTreeTransform(
				StructureIndex, Recipe->GetNominalFootprintCentimeters(), Seed);
			const FWorldChunkCoord TreeChunk = FWorldChunkCoord::FromWorldLocation(
				TreeTransform.GetLocation());
			const FWorldChunkPackCoord TreePack = FWorldChunkPackCoord::FromChunk(TreeChunk);
			FPackSpoolState& TreeSpool = Spools.FindOrAdd(TreePack);
			if (TreeSpool.Path.IsEmpty())
			{
				TreeSpool.Path = MakeSpoolPath(SpoolRoot, TreePack);
			}
			FSeedSpoolAddress TreeAddress;
			TreeAddress.StructureIndex = StructureIndex;
			TreeAddress.PieceIndex = TreeSpoolPieceIndex;
			const int32 WriteOffset = TreeSpool.BufferedBytes.AddUninitialized(sizeof(TreeAddress));
			FMemory::Memcpy(
				TreeSpool.BufferedBytes.GetData() + WriteOffset,
				&TreeAddress,
				sizeof(TreeAddress));
			BufferedSpoolBytes += sizeof(TreeAddress);
			++TreeSpool.RecordCount;
		}
		if (BufferedSpoolBytes >= MaximumBufferedSpoolBytes && !FlushSpoolBuffers(Spools, BufferedSpoolBytes, Error))
		{
			UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("%s"), *Error);
			return 1;
		}
		if ((StructureIndex + 1) % 100000 == 0)
		{
			UE_LOG(LogMillionSettlementWorldCommandlet, Display,
				TEXT("已建立临时 Pack 路由：%u / %d 个完整结构。"), StructureIndex + 1, StructureCount);
		}
	}
	FirstEntityIdByStructure[StructureCount] = NextRecipeBuildingEntityId;
	if (!FlushSpoolBuffers(Spools, BufferedSpoolBytes, Error))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	if (NextRecipeBuildingEntityId != static_cast<uint64>(RecipeBuildingEntityCount) + 1)
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("Recipe Building Entity ID 计数不一致：Next=%llu, Expected=%lld。"),
			NextRecipeBuildingEntityId, RecipeBuildingEntityCount + 1);
		return 1;
	}

	TArray<FWorldChunkPackCoord> PackCoords;
	Spools.GenerateKeyArray(PackCoords);
	Algo::Sort(PackCoords);
	int64 TotalCompressedBytes = 0;
	int64 WrittenBuildingEntityCount = 0;
	int64 WrittenMountedTorchBuildingEntityCount = 0;
	int64 WrittenTreeWorldObjectCount = 0;
	for (int32 PackIndex = 0; PackIndex < PackCoords.Num(); ++PackIndex)
	{
		const FWorldChunkPackCoord PackCoord = PackCoords[PackIndex];
		const FPackSpoolState& Spool = Spools.FindChecked(PackCoord);
		TArray<uint8> SpoolBytes;
		if (!FFileHelper::LoadFileToArray(SpoolBytes, *Spool.Path)
			|| SpoolBytes.Num() % sizeof(FSeedSpoolAddress) != 0
			|| SpoolBytes.Num() / sizeof(FSeedSpoolAddress) != Spool.RecordCount)
		{
			UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("Pack 临时流损坏：%s"), *Spool.Path);
			return 1;
		}

		TMap<FWorldChunkCoord, FWorldChunkData> Chunks;
		uint32 CachedStructureIndex = MAX_uint32;
		const UCityBuildingRecipe* CachedRecipe = nullptr;
		FTransform CachedStructureTransform = FTransform::Identity;
		for (int32 ByteOffset = 0; ByteOffset < SpoolBytes.Num(); ByteOffset += sizeof(FSeedSpoolAddress))
		{
			FSeedSpoolAddress Address;
			FMemory::Memcpy(&Address, SpoolBytes.GetData() + ByteOffset, sizeof(Address));
			if (Address.StructureIndex >= static_cast<uint32>(StructureCount))
			{
				UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("临时流含越界结构索引。"));
				return 1;
			}
			if (CachedStructureIndex != Address.StructureIndex)
			{
				CachedStructureIndex = Address.StructureIndex;
				const uint8 ArchetypeIndex = ArchetypeAssignments[CachedStructureIndex];
				CachedRecipe = Recipes.IsValidIndex(ArchetypeIndex) ? Recipes[ArchetypeIndex] : nullptr;
				CachedStructureTransform = ResolveStructureTransform(CachedStructureIndex, Seed);
			}
			const bool bTreeRecord = Address.PieceIndex == TreeSpoolPieceIndex;
			const bool bMountedTorchRecord = Address.PieceIndex >= MountedTorchSpoolPieceIndexBase
				&& Address.PieceIndex < TreeSpoolPieceIndex;
			const uint8 MountedTorchFixtureIndex = bMountedTorchRecord
				? static_cast<uint8>(Address.PieceIndex - MountedTorchSpoolPieceIndexBase)
				: 0;
			if (!CachedRecipe
				|| (!bTreeRecord && !bMountedTorchRecord
					&& !CachedRecipe->GetPieces().IsValidIndex(Address.PieceIndex))
				|| (bMountedTorchRecord
					&& !CachedRecipe->GetMountedTorchLocalTransforms().IsValidIndex(
						MountedTorchFixtureIndex)))
			{
				UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("临时流含越界配方部件索引。"));
				return 1;
			}
			const FCityBuildingPieceRecipe* Piece = bTreeRecord || bMountedTorchRecord
				? nullptr
				: &CachedRecipe->GetPieces()[Address.PieceIndex];
			const FTransform WorldTransform = bTreeRecord
				? ResolveSettlementTreeTransform(
					Address.StructureIndex,
					CachedRecipe->GetNominalFootprintCentimeters(),
					Seed)
				: (bMountedTorchRecord
					? CachedRecipe->GetMountedTorchLocalTransforms()[MountedTorchFixtureIndex]
						* CachedStructureTransform
					: Piece->LocalTransform * CachedStructureTransform);
			const FWorldChunkCoord ChunkCoord = FWorldChunkCoord::FromWorldLocation(WorldTransform.GetLocation());
			if (FWorldChunkPackCoord::FromChunk(ChunkCoord) != PackCoord)
			{
				UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("临时流 Pack 路由与重建 Transform 不一致。"));
				return 1;
			}
			FWorldChunkData& Chunk = Chunks.FindOrAdd(ChunkCoord);
			Chunk.Coord = ChunkCoord;
			Chunk.Revision = 1;
			FWorldPersistentEntityRecord& Record = Chunk.Records.AddDefaulted_GetRef();
			Record.EntityId = bTreeRecord
				? ResolveSettlementTreeEntityId(Address.StructureIndex, BuildingEntityCount)
				: (bMountedTorchRecord
					? ResolveSettlementMountedTorchEntityId(
						Address.StructureIndex,
						MountedTorchFixtureIndex,
						RecipeBuildingEntityCount)
					: FWorldEntityId(
						FirstEntityIdByStructure[Address.StructureIndex] + Address.PieceIndex));
			Record.Domain = bTreeRecord
				? EWorldEntityDomain::WorldObject
				: EWorldEntityDomain::Building;
			Record.DefinitionId = bTreeRecord
				? SettlementTreeDefinitionId
				: (bMountedTorchRecord
					? GetMountedTorchBuildingDefinitionId()
					: ResolvePieceDefinitionId(*Piece));
			Record.WorldTransform = WorldTransform;
			Record.StateRevision = 1;
			if (!Record.IsValid())
			{
				UE_LOG(LogMillionSettlementWorldCommandlet, Error,
					TEXT("生成了无效 Settlement 持久化记录。"));
				return 1;
			}
			if (bTreeRecord)
			{
				++WrittenTreeWorldObjectCount;
			}
			else
			{
				++WrittenBuildingEntityCount;
				if (bMountedTorchRecord)
				{
					++WrittenMountedTorchBuildingEntityCount;
				}
			}
		}

		TArray<FWorldChunkData> SortedChunkData;
		Chunks.GenerateValueArray(SortedChunkData);
		Algo::Sort(SortedChunkData, [](const FWorldChunkData& Left, const FWorldChunkData& Right)
		{
			return FWorldChunkPackCoord::GetLocalChunkIndex(Left.Coord)
				< FWorldChunkPackCoord::GetLocalChunkIndex(Right.Coord);
		});
		TArray<FWorldCompressedChunk> CompressedChunks;
		CompressedChunks.Reserve(SortedChunkData.Num());
		for (const FWorldChunkData& Chunk : SortedChunkData)
		{
			FWorldCompressedChunk& Compressed = CompressedChunks.AddDefaulted_GetRef();
			if (!FWorldChunkCodec::Compress(Chunk, Compressed, Error))
			{
				UE_LOG(LogMillionSettlementWorldCommandlet, Error,
					TEXT("压缩 Chunk (%d,%d,%d) 失败：%s"), Chunk.Coord.X, Chunk.Coord.Y, Chunk.Coord.Z, *Error);
				return 1;
			}
			TotalCompressedBytes += Compressed.Bytes.Num();
		}
		if (!ArchiveWriter.WritePack(PackCoord, CompressedChunks, Error))
		{
			UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("%s"), *Error);
			return 1;
		}
		IFileManager::Get().Delete(*Spool.Path, false, true);
		if ((PackIndex + 1) % 100 == 0 || PackIndex + 1 == PackCoords.Num())
		{
			UE_LOG(LogMillionSettlementWorldCommandlet, Display,
				TEXT("已写入 Pack：%d / %d，压缩 Chunk Payload %.2f MiB。"),
				PackIndex + 1, PackCoords.Num(), TotalCompressedBytes / 1048576.0);
		}
	}
	if (WrittenBuildingEntityCount != BuildingEntityCount
		|| WrittenMountedTorchBuildingEntityCount != MountedTorchBuildingEntityCount
		|| WrittenTreeWorldObjectCount != TreeWorldObjectCount)
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("Pack 写入实体计数不一致：Building=%lld/%lld，MountedTorch=%lld/%lld，Tree=%lld/%lld。"),
			WrittenBuildingEntityCount, BuildingEntityCount,
			WrittenMountedTorchBuildingEntityCount, MountedTorchBuildingEntityCount,
			WrittenTreeWorldObjectCount, TreeWorldObjectCount);
		return 1;
	}
	if (!ArchiveWriter.Commit(Error))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	IFileManager::Get().DeleteDirectory(*SpoolRoot, false, true);
	if (!IFileManager::Get().Move(*OutputRoot, *StagingRoot, true, true))
	{
		UE_LOG(LogMillionSettlementWorldCommandlet, Error,
			TEXT("无法发布已完成的种子目录：%s -> %s"), *StagingRoot, *OutputRoot);
		return 1;
	}
	StagingGuard.Release();
	UE_LOG(LogMillionSettlementWorldCommandlet, Display,
		TEXT("种子世界生成完成：%s；%d Structures；%lld Building；%lld Tree WorldObject；Chunk Payload %.2f MiB。"),
		*OutputRoot, StructureCount, BuildingEntityCount, TreeWorldObjectCount,
		TotalCompressedBytes / 1048576.0);
	return 0;
}
