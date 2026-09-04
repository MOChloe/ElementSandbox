#include "Commandlets/ValidateWorldSeedCommandlet.h"

#include "Chunk/WorldChunkTypes.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Storage/WorldChunkCodec.h"
#include "Storage/WorldStorageArchive.h"
#include "Torch/TorchDefinition.h"
#include "Tree/SettlementTreeTypes.h"
#include "WorldSeed/MillionSettlementSeedLayout.h"

DEFINE_LOG_CATEGORY_STATIC(LogValidateWorldSeedCommandlet, Log, All);

UValidateWorldSeedCommandlet::UValidateWorldSeedCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UValidateWorldSeedCommandlet::Main(const FString& Params)
{
	const FString WorldSeedsRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldSeeds")));
	FString InputArgument;
	FParse::Value(*Params, TEXT("Input="), InputArgument);
	FString InputRoot = InputArgument.IsEmpty()
		? FPaths::Combine(WorldSeedsRoot, TEXT("MillionSettlement"))
		: (FPaths::IsRelative(InputArgument)
			? FPaths::Combine(WorldSeedsRoot, InputArgument)
			: FPaths::ConvertRelativePathToFull(InputArgument));
	FPaths::NormalizeDirectoryName(InputRoot);
	if (!FPaths::IsUnderDirectory(InputRoot, WorldSeedsRoot)
		|| !IFileManager::Get().FileExists(*FPaths::Combine(InputRoot, TEXT("World.manifest"))))
	{
		UE_LOG(LogValidateWorldSeedCommandlet, Error,
			TEXT("输入必须是 Saved/WorldSeeds 下含 World.manifest 的种子目录：%s"), *InputRoot);
		return 1;
	}

	int64 ExpectedStructures = 1000000;
	int64 ExpectedBuildingEntities = UE::ElementSandbox::WorldSeed::ExpectedBuildingEntityCount;
	int64 ExpectedMountedTorchBuildingEntities =
		UE::ElementSandbox::WorldSeed::ExpectedMountedTorchBuildingEntityCount;
	int64 ExpectedWorldObjectEntities = 1000000;
	int64 MaximumCompressedMiB = 750;
	FParse::Value(*Params, TEXT("ExpectedStructures="), ExpectedStructures);
	FParse::Value(*Params, TEXT("ExpectedBuildingEntities="), ExpectedBuildingEntities);
	FParse::Value(
		*Params,
		TEXT("ExpectedMountedTorchBuildingEntities="),
		ExpectedMountedTorchBuildingEntities);
	FParse::Value(*Params, TEXT("ExpectedWorldObjectEntities="), ExpectedWorldObjectEntities);
	FParse::Value(*Params, TEXT("MaximumCompressedMiB="), MaximumCompressedMiB);
	if (ExpectedStructures < 0 || ExpectedBuildingEntities < 0
		|| ExpectedMountedTorchBuildingEntities < 0
		|| ExpectedMountedTorchBuildingEntities > ExpectedBuildingEntities
		|| ExpectedWorldObjectEntities < 0 || MaximumCompressedMiB <= 0)
	{
		UE_LOG(LogValidateWorldSeedCommandlet, Error, TEXT("期望计数或压缩大小上限无效。"));
		return 1;
	}

	FWorldStorageOpenOptions OpenOptions;
	OpenOptions.WritableRoot = InputRoot;
	OpenOptions.bCreateIfMissing = false;
	FWorldStorageArchive Archive;
	FString Error;
	if (!Archive.Open(OpenOptions, Error))
	{
		UE_LOG(LogValidateWorldSeedCommandlet, Error, TEXT("打开种子失败：%s"), *Error);
		return 1;
	}
	const FWorldStorageManifestInfo Manifest = Archive.GetManifestInfo();
	const FWorldStorageArchiveStats ArchiveStats = Archive.GetArchiveStats();
	if (Manifest.CompleteStructureCount != ExpectedStructures
		|| Manifest.BuildingEntityCount != ExpectedBuildingEntities
		|| Manifest.WorldObjectEntityCount != ExpectedWorldObjectEntities)
	{
		UE_LOG(LogValidateWorldSeedCommandlet, Error,
			TEXT("Manifest 计数不匹配：Structures=%lld/%lld，Building=%lld/%lld，WorldObject=%lld/%lld。"),
			Manifest.CompleteStructureCount, ExpectedStructures,
			Manifest.BuildingEntityCount, ExpectedBuildingEntities,
			Manifest.WorldObjectEntityCount, ExpectedWorldObjectEntities);
		return 1;
	}
	if (Manifest.NextEntityId
			!= static_cast<uint64>(Manifest.BuildingEntityCount + Manifest.WorldObjectEntityCount) + 1
		|| Manifest.NextEntityId > static_cast<uint64>(MAX_int32))
	{
		UE_LOG(LogValidateWorldSeedCommandlet, Error,
			TEXT("种子 NextEntityId 超出逐 ID 校验器范围：%llu。"), Manifest.NextEntityId);
		return 1;
	}

	TArray<FWorldChunkCoord> ChunkCoords;
	Archive.GetOccupiedChunkCoords(ChunkCoords);
	if (ChunkCoords.Num() != ArchiveStats.OccupiedChunkCount)
	{
		UE_LOG(LogValidateWorldSeedCommandlet, Error, TEXT("稀疏 Chunk 索引计数不一致。"));
		return 1;
	}
	TBitArray<> SeenEntityIds(false, static_cast<int32>(Manifest.NextEntityId));
	int64 TotalEntityCount = 0;
	int64 BuildingEntityCount = 0;
	int64 MountedTorchBuildingEntityCount = 0;
	int64 WorldObjectEntityCount = 0;
		int64 ElementEntityCount = 0;
		int64 TotalCompressedBytes = 0;
		int32 MaximumChunkCompressedBytes = 0;
		int32 MaximumChunkUncompressedBytes = 0;
		int32 MaximumChunkEntityCount = 0;
		FWorldChunkCoord MaximumChunkCoord;
	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCoords.Num(); ++ChunkIndex)
	{
		const FWorldChunkCoord Coord = ChunkCoords[ChunkIndex];
		FWorldCompressedChunk Compressed;
		FWorldChunkData Decoded;
		if (!Archive.ReadCompressedChunk(Coord, Compressed, Error)
			|| !FWorldChunkCodec::Decompress(Compressed, Decoded, Error))
		{
			UE_LOG(LogValidateWorldSeedCommandlet, Error,
				TEXT("Chunk (%d,%d,%d) 读取或解码失败：%s"), Coord.X, Coord.Y, Coord.Z, *Error);
			return 1;
		}
		if (Decoded.Coord != Coord || !Decoded.IsValid())
		{
			UE_LOG(LogValidateWorldSeedCommandlet, Error,
				TEXT("Chunk (%d,%d,%d) 解码后 HomeChunk 契约无效。"), Coord.X, Coord.Y, Coord.Z);
			return 1;
		}

		int32 ChunkBuildingCount = 0;
		int32 ChunkWorldObjectCount = 0;
		int32 ChunkElementCount = 0;
		for (const FWorldPersistentEntityRecord& Record : Decoded.Records)
		{
			const uint64 EntityValue = Record.EntityId.GetValue();
			if (EntityValue == 0 || EntityValue >= Manifest.NextEntityId
				|| SeenEntityIds[static_cast<int32>(EntityValue)])
			{
				UE_LOG(LogValidateWorldSeedCommandlet, Error,
					TEXT("Chunk (%d,%d,%d) 含越界或全局重复 EntityId=%llu。"),
					Coord.X, Coord.Y, Coord.Z, EntityValue);
				return 1;
			}
			SeenEntityIds[static_cast<int32>(EntityValue)] = true;
			switch (Record.Domain)
			{
			case EWorldEntityDomain::Building:
			{
				const bool bMountedTorch =
					Record.DefinitionId == GetMountedTorchBuildingDefinitionId();
				const uint64 FirstMountedTorchId = static_cast<uint64>(
					ExpectedBuildingEntities - ExpectedMountedTorchBuildingEntities) + 1ull;
				const bool bInMountedTorchIdRange =
					EntityValue >= FirstMountedTorchId
					&& EntityValue <= static_cast<uint64>(ExpectedBuildingEntities);
				if (bMountedTorch != bInMountedTorchIdRange)
				{
					UE_LOG(LogValidateWorldSeedCommandlet, Error,
						TEXT("Building Entity=%llu 的挂墙火把 Definition 与保留 ID 区间不一致。"),
						EntityValue);
					return 1;
				}
				if (bMountedTorch)
				{
					if (!Record.Payload.IsEmpty()
						|| !Record.WorldTransform.GetScale3D().Equals(FVector::OneVector, 0.001))
					{
						UE_LOG(LogValidateWorldSeedCommandlet, Error,
							TEXT("挂墙火把必须是无 Payload、单位比例的 Building 形态，Entity=%llu。"),
							EntityValue);
						return 1;
					}
					++MountedTorchBuildingEntityCount;
				}
				++ChunkBuildingCount;
				break;
			}
			case EWorldEntityDomain::WorldObject:
				if (Record.DefinitionId != SettlementTreeDefinitionId || !Record.Payload.IsEmpty())
				{
					UE_LOG(LogValidateWorldSeedCommandlet, Error,
						TEXT("正式 Settlement 种子的 WorldObject 必须是无 Payload 的 Settlement.Tree。"));
					return 1;
				}
				{
					using namespace UE::ElementSandbox::WorldSeed;
					const FVector Scale = Record.WorldTransform.GetScale3D();
					if (Scale.GetMin() < SettlementTreeMinimumUniformScale - 0.001
						|| Scale.GetMax() > SettlementTreeMaximumUniformScale + 0.001
						|| !FMath::IsNearlyEqual(Scale.X, Scale.Y, 0.001)
						|| !FMath::IsNearlyEqual(Scale.Y, Scale.Z, 0.001))
					{
						UE_LOG(LogValidateWorldSeedCommandlet, Error,
							TEXT("Settlement.Tree 必须使用三倍后的 Uniform Scale [%.2f, %.2f]，Entity=%llu 实际=(%.3f,%.3f,%.3f)。"),
							SettlementTreeMinimumUniformScale,
							SettlementTreeMaximumUniformScale,
							EntityValue, Scale.X, Scale.Y, Scale.Z);
						return 1;
					}
				}
				++ChunkWorldObjectCount;
				break;
			case EWorldEntityDomain::Element: ++ChunkElementCount; break;
			default:
				UE_LOG(LogValidateWorldSeedCommandlet, Error,
					TEXT("Chunk (%d,%d,%d) 含不允许写入世界 Chunk 的领域。"), Coord.X, Coord.Y, Coord.Z);
				return 1;
			}
		}
		if (ChunkBuildingCount != Compressed.BuildingEntityCount
			|| ChunkWorldObjectCount != Compressed.WorldObjectEntityCount
			|| ChunkElementCount != Compressed.ElementEntityCount)
		{
			UE_LOG(LogValidateWorldSeedCommandlet, Error,
				TEXT("Chunk (%d,%d,%d) TOC 领域计数与内容不一致。"), Coord.X, Coord.Y, Coord.Z);
			return 1;
		}
		TotalEntityCount += Decoded.Records.Num();
		BuildingEntityCount += ChunkBuildingCount;
		WorldObjectEntityCount += ChunkWorldObjectCount;
		ElementEntityCount += ChunkElementCount;
			TotalCompressedBytes += Compressed.Bytes.Num();
			if (Compressed.Bytes.Num() > MaximumChunkCompressedBytes)
			{
				MaximumChunkCompressedBytes = Compressed.Bytes.Num();
				MaximumChunkUncompressedBytes = Compressed.UncompressedSize;
				MaximumChunkEntityCount = Decoded.Records.Num();
				MaximumChunkCoord = Coord;
			}
		if ((ChunkIndex + 1) % 10000 == 0 || ChunkIndex + 1 == ChunkCoords.Num())
		{
			UE_LOG(LogValidateWorldSeedCommandlet, Display,
				TEXT("已校验 Chunk：%d / %d；Entity：%lld。"),
				ChunkIndex + 1, ChunkCoords.Num(), TotalEntityCount);
		}
	}

	const int64 ExpectedTotalEntityCount = static_cast<int64>(Manifest.NextEntityId - 1);
	const int64 MaximumCompressedBytes = MaximumCompressedMiB * 1024ll * 1024ll;
	if (TotalEntityCount != ExpectedTotalEntityCount
		|| SeenEntityIds.CountSetBits() != ExpectedTotalEntityCount
		|| BuildingEntityCount != Manifest.BuildingEntityCount
		|| MountedTorchBuildingEntityCount != ExpectedMountedTorchBuildingEntities
		|| WorldObjectEntityCount != Manifest.WorldObjectEntityCount
		|| TotalCompressedBytes > MaximumCompressedBytes)
	{
		UE_LOG(LogValidateWorldSeedCommandlet, Error,
			TEXT("全局验收失败：Entities=%lld/%lld，Seen=%d，Building=%lld/%lld，MountedTorch=%lld/%lld，WorldObject=%lld/%lld，Compressed=%.2f/%lld MiB。"),
			TotalEntityCount, ExpectedTotalEntityCount, SeenEntityIds.CountSetBits(),
			BuildingEntityCount, Manifest.BuildingEntityCount,
			MountedTorchBuildingEntityCount, ExpectedMountedTorchBuildingEntities,
			WorldObjectEntityCount, Manifest.WorldObjectEntityCount,
			TotalCompressedBytes / 1048576.0, MaximumCompressedMiB);
		return 1;
	}

		UE_LOG(LogValidateWorldSeedCommandlet, Display,
			TEXT("种子世界完整验收通过：World=%s，Packs=%d，Chunks=%d，Structures=%lld，Building=%lld，WorldObject=%lld，Element=%lld，Compressed=%.2f MiB。"),
		*Manifest.WorldId.ToString(EGuidFormats::Digits), ArchiveStats.PackCount,
		ArchiveStats.OccupiedChunkCount, Manifest.CompleteStructureCount,
			BuildingEntityCount, WorldObjectEntityCount, ElementEntityCount,
			TotalCompressedBytes / 1048576.0);
		UE_LOG(LogValidateWorldSeedCommandlet, Display,
			TEXT("最大 Chunk=(%d,%d,%d)：Entities=%d，Compressed=%.2f MiB，Uncompressed=%.2f MiB。"),
			MaximumChunkCoord.X, MaximumChunkCoord.Y, MaximumChunkCoord.Z,
			MaximumChunkEntityCount, MaximumChunkCompressedBytes / 1048576.0,
			MaximumChunkUncompressedBytes / 1048576.0);
	return 0;
}
