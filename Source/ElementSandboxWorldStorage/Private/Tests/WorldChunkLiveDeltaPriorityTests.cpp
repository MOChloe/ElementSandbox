#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/WorldChunkServerLiveDeltaQueue.h"

using namespace UE::ElementSandbox::WorldStorage::Private;

namespace
{
FWorldChunkLiveDelta MakeDelta(const uint64 Id, const FWorldChunkCoord Coord,
	const EWorldChunkLiveDeltaKind Kind, const FVector& Location = FVector::ZeroVector)
{
	FWorldChunkLiveDelta Delta;
	Delta.EntityId = FWorldEntityId(Id);
	Delta.ChunkCoord = Coord;
	Delta.Kind = Kind;
	Delta.StateRevision = 1;
	if (Kind == EWorldChunkLiveDeltaKind::Upsert)
	{
		Delta.Record.EntityId = Delta.EntityId;
		Delta.Record.StateRevision = 1;
		Delta.Record.WorldTransform.SetLocation(Location);
	}
	return Delta;
}

bool HasBaseline(FWorldChunkCoord) { return true; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldChunkNearDeltaPriorityTest,
	"ElementSandbox.WorldStorage.Network.LiveDeltaNearObjectsBeforeFarDestruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkNearDeltaPriorityTest::RunTest(const FString&)
{
	FWorldChunkServerLiveDeltaQueue Queue;
	const FWorldChunkCoord Near(-1, 0, 0), Far(-40, 0, 0);
	const FVector Player(-100.0, 100.0, 100.0);
	for (uint64 Id = 1; Id <= 4096; ++Id)
		Queue.Enqueue(MakeDelta(Id, Far, EWorldChunkLiveDeltaKind::GameplayTombstone));
	Queue.Enqueue(MakeDelta(5000, Near, EWorldChunkLiveDeltaKind::Upsert, FVector(-9000, 100, 100)));
	Queue.Enqueue(MakeDelta(5001, Near, EWorldChunkLiveDeltaKind::Upsert, Player));
	TArray<FWorldChunkLiveDelta> Batch;
	TestTrue(TEXT("有远处删除积压时仍发送近处落地物件"), Queue.BuildBatch(Player, HasBaseline, 1, 1024, Batch));
	if (!TestEqual(TEXT("保持批次条数预算"), Batch.Num(), 1)) return false;
	TestTrue(TEXT("同 Chunk 内优先脚边的新物件，不按较小 ID 取远物件"), Batch[0].EntityId == FWorldEntityId(5001));
	TestEqual(TEXT("远处积压尚未耗尽，近处已可交互"), Queue.Num(), 4097);

	// 角色移动立即改变优先级，不能沿用上次发送或最初入队时的位置。
	Queue.BuildBatch(Far.GetWorldMinimum(), HasBaseline, 1, 1024, Batch);
	TestTrue(TEXT("传送后立即优先新位置的删除"), Batch.Num() == 1 && Batch[0].ChunkCoord == Far);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldChunkLiveDeltaFairnessTest,
	"ElementSandbox.WorldStorage.Network.LiveDeltaContinuousNearWorkKeepsBackgroundProgress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkLiveDeltaFairnessTest::RunTest(const FString&)
{
	FWorldChunkServerLiveDeltaQueue Queue;
	const FWorldChunkCoord Near(0, 0, 0), FarA(-30, 0, 0), FarB(30, 0, 0);
	Queue.Enqueue(MakeDelta(1, FarA, EWorldChunkLiveDeltaKind::GameplayTombstone));
	Queue.Enqueue(MakeDelta(2, FarB, EWorldChunkLiveDeltaKind::Upsert, FarB.GetWorldMinimum()));
	TSet<FWorldEntityId> Sent;
	TArray<FWorldChunkLiveDelta> Batch;
	int32 NearBatches = 0;
	for (uint64 Round = 0; Round < 24; ++Round)
	{
		Queue.Enqueue(MakeDelta(100 + Round, Near, EWorldChunkLiveDeltaKind::Upsert));
		if (!Queue.BuildBatch(FVector::ZeroVector, HasBaseline, 1, 1024, Batch)) return false;
		Sent.Add(Batch[0].EntityId);
		NearBatches += Batch[0].ChunkCoord == Near ? 1 : 0;
	}
	TestTrue(TEXT("持续新增近处物件时，远处删除也能发送"), Sent.Contains(FWorldEntityId(1)));
	TestTrue(TEXT("持续新增近处物件时，另一个远处 Chunk 也能前进"), Sent.Contains(FWorldEntityId(2)));
	TestTrue(TEXT("大部分批次仍服务近处交互"), NearBatches >= 21);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldChunkLiveDeltaCoalescingTest,
	"ElementSandbox.WorldStorage.Network.LiveDeltaMoveCoalescesBeforePriorityReorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkLiveDeltaCoalescingTest::RunTest(const FString&)
{
	FWorldChunkServerLiveDeltaQueue Queue;
	const FWorldChunkCoord Before(-1, 0, 0), After(1, 0, 0);
	Queue.Enqueue(MakeDelta(1, Before, EWorldChunkLiveDeltaKind::Upsert));
	FWorldChunkLiveDelta Move = MakeDelta(1, After, EWorldChunkLiveDeltaKind::Upsert, After.GetWorldMinimum());
	Move.StateRevision = Move.Record.StateRevision = 2;
	Queue.Enqueue(MoveTemp(Move));
	TestEqual(TEXT("跨 Chunk 移动仅保留一个最新状态"), Queue.Num(), 1);
	Queue.RemoveChunk(Before);
	TestEqual(TEXT("离开旧 Chunk 不会删除新位置的待发送状态"), Queue.Num(), 1);
	TArray<FWorldChunkLiveDelta> Batch;
	TestTrue(TEXT("新位置可发送"), Queue.BuildBatch(FVector::ZeroVector, HasBaseline, 256, 1024, Batch));
	if (!TestEqual(TEXT("同一身份不会在批内出现两次"), Batch.Num(), 1)) return false;
	TestTrue(TEXT("只投影最新 Chunk 和 Revision"), Batch[0].ChunkCoord == After && Batch[0].StateRevision == 2);
	TestTrue(TEXT("已发送状态离开队列"), Queue.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldChunkLiveDeltaBaselineBudgetTest,
	"ElementSandbox.WorldStorage.Network.LiveDeltaPriorityPreservesBaselineAndBatchBudgets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldChunkLiveDeltaBaselineBudgetTest::RunTest(const FString&)
{
	FWorldChunkServerLiveDeltaQueue Queue;
	const FWorldChunkCoord Near(0, 0, 0), Ready(1, 0, 0);
	Queue.Enqueue(MakeDelta(1, Near, EWorldChunkLiveDeltaKind::Upsert));
	Queue.Enqueue(MakeDelta(2, Ready, EWorldChunkLiveDeltaKind::Upsert, Ready.GetWorldMinimum()));
	Queue.Enqueue(MakeDelta(3, Ready, EWorldChunkLiveDeltaKind::GameplayTombstone));
	TArray<FWorldChunkLiveDelta> Batch;
	const auto HasReadyBaseline = [Ready](const FWorldChunkCoord Coord) { return Coord == Ready; };
	Queue.BuildBatch(FVector::ZeroVector, HasReadyBaseline, 256, 1024, Batch);
	TestTrue(TEXT("同一选中 Chunk 的删除仍先于新增，且批次同质"),
		Batch.Num() == 1 && Batch[0].EntityId == FWorldEntityId(3));
	Queue.BuildBatch(FVector::ZeroVector, HasReadyBaseline, 256, 1024, Batch);
	TestTrue(TEXT("最近 Chunk 无 ACK 时，仍只发送已有基线的 Chunk"),
		Batch.Num() == 1 && Batch[0].EntityId == FWorldEntityId(2));
	TestFalse(TEXT("未 ACK 的近场 Delta 保留等待，不越过 Snapshot"),
		Queue.BuildBatch(FVector::ZeroVector, HasReadyBaseline, 256, 1024, Batch));
	TestEqual(TEXT("等待基线的记录仍在队列"), Queue.Num(), 1);
	for (uint64 Id = 4; Id <= 5; ++Id)
	{
		auto Delta = MakeDelta(Id, Near, EWorldChunkLiveDeltaKind::Upsert);
		Delta.Record.Payload.SetNumZeroed(500);
		Queue.Enqueue(MoveTemp(Delta));
	}
	Queue.BuildBatch(FVector::ZeroVector, HasBaseline, 256, 1024, Batch);
	TestEqual(TEXT("字节预算不能因近场优先而超量"), Batch.Num(), 2);
	TestEqual(TEXT("放不下的记录留到下一批"), Queue.Num(), 1);
	return true;
}

#endif
