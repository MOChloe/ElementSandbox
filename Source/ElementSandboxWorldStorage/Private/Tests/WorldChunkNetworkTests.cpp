#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Network/WorldChunkActivationReadiness.h"
#include "Network/WorldChunkClientCache.h"
#include "Network/WorldChunkClientLiveDeltaQueue.h"
#include "Network/WorldChunkLiveDeltaFlowControl.h"
#include "Network/WorldChunkOfferFlowControl.h"
#include "Storage/WorldChunkCodec.h"

using namespace UE::ElementSandbox::WorldStorage::Private;

namespace
{
	class FScopedChunkNetworkTestDirectory final
	{
	public:
		FScopedChunkNetworkTestDirectory()
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("WorldChunkNetworkTests"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		}
		~FScopedChunkNetworkTestDirectory()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Root;
	};

	FWorldCompressedChunk MakeNetworkTestChunk()
	{
		FWorldChunkData Data;
		Data.Coord = FWorldChunkCoord(-3, 4, 5);
		Data.Revision = 17;
		for (uint64 Index = 1; Index <= 1600; ++Index)
		{
			FWorldPersistentEntityRecord& Record = Data.Records.AddDefaulted_GetRef();
			Record.EntityId = FWorldEntityId(Index);
			Record.Domain = EWorldEntityDomain::Building;
			Record.DefinitionId = FName(TEXT("Network.Cache.Wall"));
			Record.WorldTransform = FTransform(
				FRotator(0.0, static_cast<double>(Index % 360), 0.0),
				Data.Coord.GetWorldMinimum()
					+ FVector(
						static_cast<double>((Index * 37) % 9900),
						static_cast<double>((Index * 53) % 9900),
						static_cast<double>((Index * 71) % 9900)));
			Record.StateRevision = 1;
			Record.Payload.SetNumUninitialized(32);
			uint32 PayloadState = static_cast<uint32>(Index) * 747796405u + 2891336453u;
			for (int32 ByteIndex = 0; ByteIndex < Record.Payload.Num(); ++ByteIndex)
			{
				PayloadState ^= PayloadState << 13;
				PayloadState ^= PayloadState >> 17;
				PayloadState ^= PayloadState << 5;
				Record.Payload[ByteIndex] = static_cast<uint8>(PayloadState);
			}
		}
		FWorldCompressedChunk Chunk;
		FString Error;
		check(FWorldChunkCodec::Compress(Data, Chunk, Error));
		return Chunk;
	}

	FWorldChunkOffer MakeOffer(
		const FGuid& WorldId,
		const FWorldCompressedChunk& Chunk)
	{
		FWorldChunkOffer Offer;
		Offer.WorldId = WorldId;
		Offer.Coord = Chunk.Coord;
		Offer.Revision = Chunk.Revision;
		Offer.ContentHash = Chunk.ContentHash;
		Offer.CompressedSize = Chunk.Bytes.Num();
		Offer.UncompressedSize = Chunk.UncompressedSize;
		return Offer;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkActivationReadinessContractTest,
	"ElementSandbox.WorldStorage.Network.ActivationReadinessRejectsVacuousCore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkActivationReadinessContractTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("空 Core 的 0/0 不能放行"),
		IsActivationCoreGateComplete(true, true, 0, 0, 0));
	TestFalse(TEXT("WorldStorage 未就绪不能放行"),
		IsActivationCoreGateComplete(false, true, 1, 1, 1));
	TestFalse(TEXT("Client 端点握手前不能放行"),
		IsActivationCoreGateComplete(true, false, 1, 1, 1));
	TestFalse(TEXT("Client 尚未 ACK 全部 Core 不能放行"),
			IsActivationCoreGateComplete(true, true, 2, 1, 2));
	TestFalse(TEXT("Authority 尚未准备好全部 Core 不能放行"),
			IsActivationCoreGateComplete(true, true, 2, 2, 1));
	TestTrue(TEXT("存档、端点、ACK 与 Authority 基线全部就绪后才放行"),
			IsActivationCoreGateComplete(true, true, 2, 2, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkInitialActivationGateLatchTest,
	"ElementSandbox.WorldStorage.Network.InitialActivationGateIsOneWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkInitialActivationGateLatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestFalse(TEXT("首次 Core 未完成时保持启动门禁"),
		IsInitialActivationGateSatisfied(false, false));
	TestTrue(TEXT("首次 Core 完成后放行"),
		IsInitialActivationGateSatisfied(false, true));
	TestTrue(TEXT("兴趣中心迁移时新 Core 未完成也不会重新关闭门禁"),
		IsInitialActivationGateSatisfied(true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkDenseActivationCoreTest,
	"ElementSandbox.WorldStorage.Network.ActivationCoreIncludesEmptyAdjacentChunks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkDenseActivationCoreTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FWorldChunkCoord Center(-3, 4, -2);
	TArray<FWorldChunkCoord> Core;
	BuildDenseActivationCore(Center, Core);
	TestEqual(TEXT("3x3x3 Activation Core 总是包含 27 个 Chunk"), Core.Num(), 27);
	TestTrue(TEXT("遍历首坐标稳定"), Core[0] == FWorldChunkCoord(-4, 3, -3));
	TestTrue(TEXT("遍历末坐标稳定"), Core.Last() == FWorldChunkCoord(-2, 5, -1));
	TestTrue(TEXT("包含权威 Pawn 所在 Chunk"), Core.Contains(Center));
	TSet<FWorldChunkCoord> Unique;
	for (const FWorldChunkCoord& Coord : Core)
	{
		Unique.Add(Coord);
	}
	TestEqual(TEXT("基线不包含重复 Chunk"), Unique.Num(), Core.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkOfferFlowControlContractTest,
	"ElementSandbox.WorldStorage.Network.OfferApplicationWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkOfferFlowControlContractTest::RunTest(const FString& Parameters)
{
	FWorldChunkOfferFlowControl Flow;
	TestFalse(TEXT("未发布 Offer 不占窗口"), Flow.OccupiesPublishedWindow());
	Flow.MarkPublished();
	TestTrue(TEXT("发布后直到 Snapshot 完成持续占窗口"), Flow.OccupiesPublishedWindow());
	TestFalse(TEXT("响应超时前不重发"),
		Flow.ShouldRetryOffer(false, false, 0.5, 0.0, 1.0));
	TestTrue(TEXT("未收到 Have/Need 且超时后允许重发"),
		Flow.ShouldRetryOffer(false, false, 1.0, 0.0, 1.0));
	TestFalse(TEXT("已经排队的重发不会重复排队"),
		Flow.ShouldRetryOffer(false, true, 2.0, 0.0, 1.0));
	Flow.MarkClientResponseReceived();
	TestTrue(TEXT("Need 后仍占窗口直到 Snapshot ACK"), Flow.OccupiesPublishedWindow());
	TestFalse(TEXT("收到 Need 后不再重发 Offer"),
		Flow.ShouldRetryOffer(false, false, 2.0, 0.0, 1.0));
	Flow.ReleasePublishedWindow();
	TestFalse(TEXT("Snapshot ACK 后释放窗口"), Flow.OccupiesPublishedWindow());
	TestFalse(TEXT("窗口满时拒绝新 Offer"),
		FWorldChunkOfferFlowControl::CanPublish(false, 64, 64));
	TestTrue(TEXT("窗口满时仍允许已发布 Offer 超时重发"),
		FWorldChunkOfferFlowControl::CanPublish(true, 64, 64));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkLiveDeltaFlowControlContractTest,
	"ElementSandbox.WorldStorage.Network.LiveDeltaApplicationWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkLiveDeltaFlowControlContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWorldChunkLiveDeltaFlowControl Flow;
	TestTrue(TEXT("没有在途 Batch 时可以发布"), Flow.CanPublish());
	const uint64 First = Flow.BeginBatch();
	TestTrue(TEXT("首个 Batch 获得非零序号"), First != 0);
	TestTrue(TEXT("首批在途时仍允许填充第二个窗口"), Flow.CanPublish());
	const uint64 Second = Flow.BeginBatch();
	TestTrue(TEXT("第二个 Batch 使用递增序号"), Second > First);
	TestFalse(TEXT("两个应用窗口都占用后禁止继续发布"), Flow.CanPublish());
	TestEqual(TEXT("窗口满时 BeginBatch 不会旁路窗口"), Flow.BeginBatch(), uint64(0));
	TestFalse(TEXT("迟到或伪造 ACK 不释放窗口"), Flow.Acknowledge(Second + 1));
	TestTrue(TEXT("正确 ACK 释放一个窗口"), Flow.Acknowledge(First));
	TestTrue(TEXT("释放一个窗口后可继续发布"), Flow.CanPublish());
	TestTrue(TEXT("第二个在途 Batch 可独立 ACK"), Flow.Acknowledge(Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkClientLiveDeltaQueueContractTest,
	"ElementSandbox.WorldStorage.Network.LiveDeltaClientAppliesOneQueuedBatchAtATime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkClientLiveDeltaQueueContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWorldChunkClientLiveDeltaQueue Queue;
	auto MakeDeltas = []
	{
		TArray<FWorldChunkLiveDelta> Deltas;
		FWorldChunkLiveDelta& Delta = Deltas.AddDefaulted_GetRef();
		Delta.Kind = EWorldChunkLiveDeltaKind::GameplayTombstone;
		Delta.EntityId = FWorldEntityId(17);
		Delta.StateRevision = 2;
		return Deltas;
	};
	TestTrue(TEXT("第一批只入队、不在 RPC 内应用"),
		Queue.Enqueue(1, MakeDeltas()) == EWorldChunkLiveDeltaEnqueueResult::Queued);
	TestTrue(TEXT("双窗口第二批保持严格顺序"),
		Queue.Enqueue(2, MakeDeltas()) == EWorldChunkLiveDeltaEnqueueResult::Queued);
	TestEqual(TEXT("客户端最多持有服务器双窗口的两个批次"), Queue.Num(), 2);
	TestTrue(TEXT("第三批不能绕过满队列"),
		Queue.Enqueue(3, MakeDeltas()) == EWorldChunkLiveDeltaEnqueueResult::Full);
	const FQueuedWorldChunkLiveDeltaBatch* First = Queue.Peek();
	TestTrue(TEXT("每帧只暴露队首批次"), First && First->Sequence == 1);
	TestTrue(TEXT("完成首批后才推进 ACK 序号"), Queue.CompleteFront(1));
	TestEqual(TEXT("首批完成后只剩第二批"), Queue.Num(), 1);
	TestEqual(TEXT("已应用序号精确推进一次"), Queue.GetLastAppliedSequence(), uint64(1));
	const FQueuedWorldChunkLiveDeltaBatch* Second = Queue.Peek();
	TestTrue(TEXT("下一帧看到第二批"), Second && Second->Sequence == 2);
	TestTrue(TEXT("已应用重包只需幂等 ACK"),
		Queue.Enqueue(1, MakeDeltas()) == EWorldChunkLiveDeltaEnqueueResult::AlreadyApplied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkClientCacheContractTest,
	"ElementSandbox.WorldStorage.Network.ClientCacheValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkClientCacheContractTest::RunTest(const FString& Parameters)
{
	FScopedChunkNetworkTestDirectory Directory;
	const FGuid WorldId(0x11111111, 0x22222222, 0x33333333, 0x44444444);
	const FWorldCompressedChunk Source = MakeNetworkTestChunk();
	const FWorldChunkOffer Offer = MakeOffer(WorldId, Source);
	TestTrue(TEXT("完整 Chunk 可原子写入客户端缓存"),
		FWorldChunkClientCache::Save(Directory.Root, WorldId, Source));

	FWorldCompressedChunk Loaded;
	TestTrue(TEXT("Revision 与 Hash 命中时缓存可读"),
		FWorldChunkClientCache::Load(Directory.Root, Offer, Loaded));
	TestTrue(TEXT("缓存命中保持压缩 Payload 原样"), Loaded.Bytes == Source.Bytes);

	FWorldChunkOffer Stale = Offer;
	++Stale.Revision;
	TestFalse(TEXT("Revision 失配不能声称 Have"),
		FWorldChunkClientCache::Load(Directory.Root, Stale, Loaded));
	Stale = Offer;
	++Stale.ContentHash.Low;
	TestFalse(TEXT("ContentHash 失配不能声称 Have"),
		FWorldChunkClientCache::Load(Directory.Root, Stale, Loaded));

	const FString CachePath =
		FWorldChunkClientCache::MakeFilename(Directory.Root, Source.Coord);
	TArray<uint8> CorruptBytes;
	check(FFileHelper::LoadFileToArray(CorruptBytes, *CachePath));
	CorruptBytes.Last() ^= 0x5a;
	check(FFileHelper::SaveArrayToFile(CorruptBytes, *CachePath));
	TestFalse(TEXT("缓存文件 Payload 损坏会被 Hash 校验拒绝"),
		FWorldChunkClientCache::Load(Directory.Root, Offer, Loaded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldChunkSegmentAssemblyContractTest,
	"ElementSandbox.WorldStorage.Network.SegmentedPayloadReassembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkSegmentAssemblyContractTest::RunTest(const FString& Parameters)
{
	static constexpr int32 SegmentBytes = 24 * 1024;
	const FGuid WorldId(0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd);
	const FWorldCompressedChunk Source = MakeNetworkTestChunk();
	const FWorldChunkOffer Offer = MakeOffer(WorldId, Source);
	const int32 SegmentCount = FMath::DivideAndRoundUp(Source.Bytes.Num(), SegmentBytes);
	TestTrue(TEXT("测试 Payload 确实跨越多个 Segment"), SegmentCount > 1);

	TArray<FWorldChunkPayloadSegment> Segments;
	for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
	{
		FWorldChunkPayloadSegment& Segment = Segments.AddDefaulted_GetRef();
		Segment.Offer = Offer;
		Segment.SegmentIndex = SegmentIndex;
		Segment.SegmentCount = SegmentCount;
		const int32 Offset = SegmentIndex * SegmentBytes;
		Segment.Bytes.Append(
			Source.Bytes.GetData() + Offset,
			FMath::Min(SegmentBytes, Source.Bytes.Num() - Offset));
	}

	FWorldChunkSegmentAssembly Assembly;
	for (int32 Index = Segments.Num() - 1; Index >= 0; --Index)
	{
		const EWorldChunkSegmentAcceptResult Result =
			Assembly.Accept(Segments[Index], SegmentBytes);
		TestEqual(TEXT("乱序 Segment 在最后一段到齐前只返回 Accepted"),
			static_cast<uint8>(Result),
			static_cast<uint8>(Index == 0
				? EWorldChunkSegmentAcceptResult::Completed
				: EWorldChunkSegmentAcceptResult::Accepted));
	}
	TestEqual(TEXT("重复 Segment 不重复计入"),
		static_cast<uint8>(Assembly.Accept(Segments[0], SegmentBytes)),
		static_cast<uint8>(EWorldChunkSegmentAcceptResult::Duplicate));

	FWorldCompressedChunk Reassembled;
	TestTrue(TEXT("完整 Segment 可重组成已校验 Chunk"), Assembly.Build(Reassembled));
	TestTrue(TEXT("重组字节与原 Payload 完全一致"), Reassembled.Bytes == Source.Bytes);

	FWorldChunkSegmentAssembly Rejected;
	FWorldChunkPayloadSegment WrongSize = Segments.Last();
	WrongSize.Bytes.Pop(EAllowShrinking::No);
	TestEqual(TEXT("尾 Segment 长度不正确会立即拒绝"),
		static_cast<uint8>(Rejected.Accept(WrongSize, SegmentBytes)),
		static_cast<uint8>(EWorldChunkSegmentAcceptResult::Rejected));
	FWorldChunkPayloadSegment WrongOffer = Segments[0];
	++WrongOffer.Offer.Revision;
	TestEqual(TEXT("同一装配器拒绝不同 Snapshot Revision"),
		static_cast<uint8>(Assembly.Accept(WrongOffer, SegmentBytes)),
		static_cast<uint8>(EWorldChunkSegmentAcceptResult::Rejected));
	return true;
}

#endif
