#if WITH_DEV_AUTOMATION_TESTS

#include "Chunk/WorldChunkCoordinates.h"
#include "Chunk/WorldChunkTypes.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Storage/WorldChunkCodec.h"
#include "Storage/WorldStorageArchive.h"

namespace ElementSandbox::WorldStorage::Tests
{
	FWorldPersistentEntityRecord MakeRecord(
		const uint64 Id,
		const EWorldEntityDomain Domain,
		const FName DefinitionId,
		const FVector& Location,
		const uint32 Revision,
		const TArray<uint8>& Payload = {})
	{
		FWorldPersistentEntityRecord Record;
		Record.EntityId = FWorldEntityId(Id);
		Record.Domain = Domain;
		Record.DefinitionId = DefinitionId;
		Record.WorldTransform = FTransform(
			FRotator(11.0, 37.0, -4.0),
			Location,
			FVector(1.0, 1.25, 0.75));
		Record.StateRevision = Revision;
		Record.Payload = Payload;
		return Record;
	}

	FWorldCompressedChunk MakeChunk(
		const FWorldChunkCoord& Coord,
		const uint32 Revision,
		TArray<FWorldPersistentEntityRecord> Records)
	{
		FWorldChunkData Chunk;
		Chunk.Coord = Coord;
		Chunk.Revision = Revision;
		Chunk.Records = MoveTemp(Records);
		FWorldCompressedChunk Compressed;
		FString Error;
		check(FWorldChunkCodec::Compress(Chunk, Compressed, Error));
		return Compressed;
	}

	FWorldStorageManifestInfo MakeManifest(const uint64 Generation = 1)
	{
		FWorldStorageManifestInfo Info;
		Info.WorldId = FGuid(0x12345678, 0x90abcdef, 0x10203040, 0x50607080);
		Info.Generation = Generation;
		Info.NextEntityId = 1000;
		Info.WorldSimulationTimeMilliseconds = 54321;
		Info.LastCheckpointUtc = FDateTime(2026, 8, 25, 1, 2, 3);
		Info.CompleteStructureCount = 12;
		Info.BuildingEntityCount = 34;
		Info.WorldObjectEntityCount = 56;
		return Info;
	}

	class FScopedWorldStorageTestDirectory final
	{
	public:
		explicit FScopedWorldStorageTestDirectory(const FString& Suffix)
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("WorldStorageTests"),
				Suffix + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			check(Root.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())));
		}

		~FScopedWorldStorageTestDirectory()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Child(const TCHAR* Name) const { return FPaths::Combine(Root, Name); }

	private:
		FString Root;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkCoordinateContractTest,
	"ElementSandbox.WorldStorage.Chunk.Coordinates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkCoordinateContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	TestTrue(TEXT("原点属于 0 Chunk"), FWorldChunkCoord::FromWorldLocation(FVector::ZeroVector) == FWorldChunkCoord(0, 0, 0));
	TestTrue(TEXT("100m 边界进入下一个 Chunk"),
		FWorldChunkCoord::FromWorldLocation(FVector(10000.0, 9999.99, -0.01)) == FWorldChunkCoord(1, 0, -1));
	TestTrue(TEXT("负 100m 边界和边界前一侧使用数学向下取整"),
		FWorldChunkCoord::FromWorldLocation(FVector(-10000.0, -10000.01, -9999.99)) == FWorldChunkCoord(-1, -2, -1));

	TestTrue(TEXT("正坐标 10³ Chunk 映射 Pack"),
		FWorldChunkPackCoord::FromChunk(FWorldChunkCoord(9, 10, 19)) == FWorldChunkPackCoord(0, 1, 1));
	TestTrue(TEXT("负坐标 Pack 也使用数学向下取整"),
		FWorldChunkPackCoord::FromChunk(FWorldChunkCoord(-1, -10, -11)) == FWorldChunkPackCoord(-1, -1, -2));
	TestEqual(TEXT("-1 Chunk 在负 Pack 中的局部索引为 999"),
		FWorldChunkPackCoord::GetLocalChunkIndex(FWorldChunkCoord(-1, -1, -1)), 999);

	const FWorldChunkBox EvenBox = FWorldChunkBox::Centered(FWorldChunkCoord(0, 0, 0), 128);
	TestTrue(TEXT("128 Box 是半开且恰好覆盖 -64..63"),
		EvenBox.Minimum == FWorldChunkCoord(-64, -64, -64)
		&& EvenBox.MaximumExclusive == FWorldChunkCoord(64, 64, 64)
		&& EvenBox.Contains(FWorldChunkCoord(63, 63, 63))
		&& !EvenBox.Contains(FWorldChunkCoord(64, 0, 0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkCodecContractTest,
	"ElementSandbox.WorldStorage.Codec.RoundTripAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkCodecContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FWorldChunkData Source;
	Source.Coord = FWorldChunkCoord(-2, 3, -4);
	Source.Revision = 17;
	const FVector Minimum = Source.Coord.GetWorldMinimum();
	Source.Records.Add(MakeRecord(100, EWorldEntityDomain::WorldObject, TEXT("Stick"), Minimum + FVector(1.0, 5000.0, 9999.0), 4, {7, 8}));
	Source.Records.Add(MakeRecord(5, EWorldEntityDomain::Building, TEXT("Wall"), Minimum + FVector(123.45, 678.9, 4321.0), 2, {1, 2, 3}));
	Source.Records.Add(MakeRecord(900, EWorldEntityDomain::Element, TEXT("Fire"), Minimum + FVector(9999.0, 0.25, 50.0), 11));
	Source.Records.Add(MakeRecord(901, EWorldEntityDomain::Building, TEXT("Edge"), Minimum + FVector(9999.999, 9999.999, 9999.999), 1));

	TArray<uint8> Encoded;
	FString Error;
	TestTrue(TEXT("有效 Chunk 可以编码"), FWorldChunkCodec::Encode(Source, Encoded, Error));
	FWorldChunkData Decoded;
	TestTrue(TEXT("编码结果可以完整解码"), FWorldChunkCodec::Decode(Encoded, Decoded, Error));
	TestEqual(TEXT("记录数往返一致"), Decoded.Records.Num(), Source.Records.Num());
	TestEqual(TEXT("Chunk Revision 往返一致"), Decoded.Revision, Source.Revision);
	for (const FWorldPersistentEntityRecord& Original : Source.Records)
	{
		const FWorldPersistentEntityRecord* RoundTrip = Decoded.Records.FindByPredicate(
			[Id = Original.EntityId](const FWorldPersistentEntityRecord& Candidate) { return Candidate.EntityId == Id; });
		TestNotNull(TEXT("每个 ID 都可恢复"), RoundTrip);
		if (RoundTrip)
		{
			TestEqual(TEXT("领域一致"), static_cast<uint8>(RoundTrip->Domain), static_cast<uint8>(Original.Domain));
			TestEqual(TEXT("Definition 一致"), RoundTrip->DefinitionId, Original.DefinitionId);
			TestEqual(TEXT("Revision 一致"), RoundTrip->StateRevision, Original.StateRevision);
			TestTrue(TEXT("量化位置误差不超过 0.2cm"),
				RoundTrip->WorldTransform.GetLocation().Equals(Original.WorldTransform.GetLocation(), 0.2));
			TestTrue(TEXT("非默认 Scale 往返"),
				RoundTrip->WorldTransform.GetScale3D().Equals(Original.WorldTransform.GetScale3D(), UE_KINDA_SMALL_NUMBER));
			TestTrue(TEXT("预定终态与实际 Chunk 编解码完全一致"),
				RoundTrip->WorldTransform.Equals(FWorldChunkCodec::QuantizeTransform(Original.WorldTransform), 1e-8));
			TestTrue(TEXT("解码旋转归一化，矩阵转换不引入缩放"),
				FMath::IsNearlyEqual(RoundTrip->WorldTransform.GetRotation().SizeSquared(), 1.0, 1e-12));
			TestTrue(TEXT("领域 Payload 往返"), RoundTrip->Payload == Original.Payload);
		}
	}

	FWorldCompressedChunk Compressed;
	TestTrue(TEXT("Chunk 可以压缩"), FWorldChunkCodec::Compress(Source, Compressed, Error));
	FWorldChunkData Uncompressed;
	TestTrue(TEXT("压缩 Chunk 可以校验解压"), FWorldChunkCodec::Decompress(Compressed, Uncompressed, Error));
	Compressed.Bytes[Compressed.Bytes.Num() / 2] ^= 0x5a;
	TestFalse(TEXT("ContentHash 损坏被拒绝"), FWorldChunkCodec::Decompress(Compressed, Uncompressed, Error));

	TArray<uint8> WrongVersion = Encoded;
	check(WrongVersion.Num() > 5);
	WrongVersion[4] = 0xff;
	WrongVersion[5] = 0xff;
	TestFalse(TEXT("格式版本不匹配明确拒绝"), FWorldChunkCodec::Decode(WrongVersion, Uncompressed, Error));
	TArray<uint8> WrongCodec = Encoded;
	check(WrongCodec.Num() > 7);
	WrongCodec[6] = 0xff;
	WrongCodec[7] = 0xff;
	TestFalse(TEXT("Codec 版本不匹配明确拒绝"), FWorldChunkCodec::Decode(WrongCodec, Uncompressed, Error));

	Source.Records.Add(MakeRecord(5, EWorldEntityDomain::WorldObject, TEXT("Duplicate"), Minimum + FVector(50.0), 1));
	TestFalse(TEXT("跨领域重复 WorldEntityId 不允许编码"), FWorldChunkCodec::Encode(Source, Encoded, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageArchiveContractTest,
	"ElementSandbox.WorldStorage.Archive.SparseSeekAndCOW",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageArchiveContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedWorldStorageTestDirectory Directory(TEXT("Archive"));
	const FString SeedRoot = Directory.Child(TEXT("Seed"));
	const FString WritableRoot = Directory.Child(TEXT("Writable"));
	const FWorldChunkCoord NegativeCoord(-1, -1, -1);
	const FWorldChunkCoord OriginCoord(0, 0, 0);
	const FWorldChunkCoord OtherPackCoord(10, 0, 0);

	TArray<FWorldCompressedChunk> SeedChunks;
	SeedChunks.Add(MakeChunk(NegativeCoord, 1, {
		MakeRecord(1, EWorldEntityDomain::Building, TEXT("Wall"), NegativeCoord.GetWorldMinimum() + FVector(20.0), 1)}));
	SeedChunks.Add(MakeChunk(OriginCoord, 1, {
		MakeRecord(2, EWorldEntityDomain::WorldObject, TEXT("Rock"), FVector(100.0, 200.0, 300.0), 1)}));
	SeedChunks.Add(MakeChunk(OtherPackCoord, 3, {
		MakeRecord(3, EWorldEntityDomain::Element, TEXT("Fire"), OtherPackCoord.GetWorldMinimum() + FVector(500.0), 2)}));

	FString Error;
	const FWorldStorageManifestInfo SeedManifest = MakeManifest(1);
	TestTrue(TEXT("可以写入小型确定性种子 Archive"),
		FWorldStorageArchive::WriteSeedArchive(SeedRoot, SeedManifest, SeedChunks, Error));

	FWorldStorageArchive Archive;
	FWorldStorageOpenOptions Options;
	Options.WritableRoot = WritableRoot;
	Options.SeedRoot = SeedRoot;
	Options.bCreateIfMissing = false;
	TestTrue(TEXT("Writable Manifest 可以引用只读 Seed Pack"), Archive.Open(Options, Error));
	TestEqual(TEXT("Manifest v3 往返保存 WorldObjectEntityCount"),
		Archive.GetManifestInfo().WorldObjectEntityCount, 56ll);

	TArray<FWorldChunkCoord> Occupied;
	Archive.QueryOccupiedChunks(FWorldChunkBox { FWorldChunkCoord(-2, -2, -2), FWorldChunkCoord(1, 1, 1) }, Occupied);
	TestEqual(TEXT("稀疏查询只返回范围内真实 Chunk"), Occupied.Num(), 2);
	TestTrue(TEXT("负坐标 Chunk 被索引"), Occupied.Contains(NegativeCoord));
	TestTrue(TEXT("原点 Chunk 被索引"), Occupied.Contains(OriginCoord));
	Archive.GetOccupiedChunkCoords(Occupied);
	TestEqual(TEXT("离线校验可枚举全部非空 Chunk"), Occupied.Num(), 3);
	TestTrue(TEXT("全量枚举按坐标排序"),
		Occupied[0] == NegativeCoord && Occupied[1] == OriginCoord && Occupied[2] == OtherPackCoord);
	FWorldChunkCoord MostPopulatedChunk;
	TestTrue(TEXT("Archive 缓存地图代表 Chunk，无需触发时扫描全量索引"),
		Archive.TryGetMostPopulatedChunk(MostPopulatedChunk));
	TestTrue(TEXT("同人口 Chunk 稳定选择更靠近地图占用范围中心者"),
		MostPopulatedChunk == OriginCoord);

	FWorldCompressedChunk ReadBack;
	TestTrue(TEXT("可以跨 Pack 单独 Seek 一个 Chunk"), Archive.ReadCompressedChunk(OtherPackCoord, ReadBack, Error));
	TestEqual(TEXT("Seek 不改变 Chunk Revision"), ReadBack.Revision, 3u);
	FWorldChunkData ReadData;
	TestTrue(TEXT("Seek 结果可独立解压"), FWorldChunkCodec::Decompress(ReadBack, ReadData, Error));
	TestEqual(TEXT("单 Chunk 只含自己的记录"), ReadData.Records.Num(), 1);

	FWorldCompressedChunk UpdatedOrigin = MakeChunk(OriginCoord, 5, {
		MakeRecord(2, EWorldEntityDomain::WorldObject, TEXT("Rock"), FVector(101.0, 202.0, 303.0), 5),
		MakeRecord(4, EWorldEntityDomain::Building, TEXT("Floor"), FVector(500.0, 600.0, 700.0), 1)});
	TArray<FWorldChunkCheckpointChange> Changes;
	Changes.Add({OriginCoord, UpdatedOrigin});
	Changes.Add({NegativeCoord, {}});
	FWorldStorageManifestInfo CheckpointManifest = SeedManifest;
	CheckpointManifest.Generation = 2;
	CheckpointManifest.NextEntityId = 1001;
	CheckpointManifest.LastCheckpointUtc = FDateTime(2026, 8, 25, 2, 0, 0);
	TestTrue(TEXT("Checkpoint 以 COW 更新一个 Pack 并删除 Chunk"),
		Archive.WriteCheckpoint(Changes, CheckpointManifest, Error));
	TestFalse(TEXT("删除后的 Chunk 不会从旧 Seed 复活"), Archive.ReadCompressedChunk(NegativeCoord, ReadBack, Error));
	TestTrue(TEXT("未修改的其他 Pack 继续可读"), Archive.ReadCompressedChunk(OtherPackCoord, ReadBack, Error));
	TestTrue(TEXT("更新 Chunk 使用新 Revision"), Archive.ReadCompressedChunk(OriginCoord, ReadBack, Error));
	TestEqual(TEXT("新 Revision 已发布"), ReadBack.Revision, 5u);

	FWorldStorageArchive Reopened;
	TestTrue(TEXT("重开只相信已原子提交的 Writable Manifest"), Reopened.Open(Options, Error));
	TestFalse(TEXT("重开后 Tombstone 语义仍阻止旧 Seed Chunk 复活"),
		Reopened.ReadCompressedChunk(NegativeCoord, ReadBack, Error));
	TestEqual(TEXT("重开保留 Checkpoint Generation"), Reopened.GetManifestInfo().Generation, 2ull);
	TestTrue(TEXT("Checkpoint 重载索引后重新发布最密集 Chunk"),
		Reopened.TryGetMostPopulatedChunk(MostPopulatedChunk));
	TestTrue(TEXT("包含 Building 与 WorldObject 的更新 Chunk 成为唯一最密集 Chunk"),
		MostPopulatedChunk == OriginCoord);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageLegacyManifestRejectedTest,
	"ElementSandbox.WorldStorage.Archive.ManifestV1V2Rejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageLegacyManifestRejectedTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedWorldStorageTestDirectory Directory(TEXT("LegacyManifest"));
	const FString Root = Directory.Child(TEXT("Seed"));
	FString Error;
	TArray<FWorldCompressedChunk> Chunks;
	Chunks.Add(MakeChunk(FWorldChunkCoord(0, 0, 0), 1, {
		MakeRecord(1, EWorldEntityDomain::WorldObject, TEXT("Settlement.Tree"), FVector::ZeroVector, 1)}));
	TestTrue(TEXT("先写出当前 v3 Manifest"),
		FWorldStorageArchive::WriteSeedArchive(Root, MakeManifest(), Chunks, Error));
	const FString ManifestPath = FPaths::Combine(Root, TEXT("World.manifest"));
	TArray<uint8> Bytes;
	TestTrue(TEXT("可读取 Manifest 原始字节"), FFileHelper::LoadFileToArray(Bytes, *ManifestPath));
	if (Bytes.Num() < 6)
	{
		AddError(TEXT("Manifest Header 长度不足。"));
		return false;
	}
	FWorldStorageOpenOptions Options;
	Options.WritableRoot = Root;
	Options.bCreateIfMissing = false;
	for (const uint16 LegacyVersion : {uint16(1), uint16(2)})
	{
		Bytes[4] = static_cast<uint8>(LegacyVersion & 0xff);
		Bytes[5] = static_cast<uint8>(LegacyVersion >> 8);
		TestTrue(FString::Printf(TEXT("把版本字段改写为 v%u"), LegacyVersion),
			FFileHelper::SaveArrayToFile(Bytes, *ManifestPath));
		Error.Reset();
		FWorldStorageArchive Archive;
		TestFalse(FString::Printf(TEXT("v%u Manifest 被明确拒绝且不走热状态迁移"),
			LegacyVersion), Archive.Open(Options, Error));
		TestTrue(TEXT("拒绝原因明确要求重新生成"),
			Error.Contains(TEXT("格式版本不匹配")));
	}
	return true;
}

#endif
