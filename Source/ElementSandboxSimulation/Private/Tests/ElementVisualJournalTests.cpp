#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Visual/ElementVisualJournal.h"

namespace ElementSandbox::Simulation::Visual::Tests
{
	FElementVisualDescriptor MakeDescriptor(
		const uint64 EntityId,
		const FElementVisualShardKey Shard,
		const uint64 Revision,
		const FVector& Location)
	{
		FElementVisualDescriptor Descriptor;
		Descriptor.Key = FElementVisualKey::MakePersistent(
			FWorldEntityId(EntityId), TEXT("Test.Flame"), 0);
		Descriptor.VisualDefinitionId = TEXT("Test.Visual.Cone");
		Descriptor.Shard = Shard;
		Descriptor.WorldTransform = FTransform(Location);
		Descriptor.WorldBounds = FBox::BuildAABB(Location, FVector(25.0));
		Descriptor.Intensity = 1.0f;
		Descriptor.Color = FLinearColor::Red;
		Descriptor.StartTimeMilliseconds = 100;
		Descriptor.EndTimeMilliseconds = 0;
		Descriptor.Revision = Revision;
		return Descriptor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementVisualJournalBoundedShardTest,
	"ElementSandbox.Simulation.Visual.Journal.BoundedShardAndDuplicateRevision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementVisualJournalBoundedShardTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Simulation::Visual::Tests;
	FElementVisualJournalConfig Config;
	Config.MaxRetainedBatchesPerShard = 2;
	FElementVisualJournal Journal(Config);
	const FElementVisualShardKey Shard{{1, -2, 3}};

	int32 WakeCount = 0;
	TArray<FElementVisualShardKey> LastWake;
	Journal.OnChangesAvailable().AddLambda(
		[&WakeCount, &LastWake](const TArray<FElementVisualShardKey>& Shards)
		{
			++WakeCount;
			LastWake = Shards;
		});

	FElementVisualDescriptor Descriptor = MakeDescriptor(101, Shard, 1, FVector(10050.0, -19950.0, 30050.0));
	TestTrue(TEXT("测试 Visual Descriptor 合法"), Descriptor.IsValid());
	TestEqual(TEXT("首次 Upsert 应用"),
		Journal.Upsert(Descriptor), EElementVisualMutationResult::Applied);
	TestEqual(TEXT("首次提交推进独立 Shard Sequence"), Journal.GetCurrentSequence(Shard), uint64(1));

	FElementVisualShardSnapshot Snapshot;
	TestTrue(TEXT("Snapshot 与 Cursor 原子复制"), Journal.CopyShardSnapshot(Shard, Snapshot));
	TestEqual(TEXT("Snapshot Cursor 对齐已发布序列"), Snapshot.Cursor, uint64(1));
	TestTrue(TEXT("Snapshot 使用 immutable 共享数组"), Snapshot.Descriptors.IsValid());
	TestEqual(TEXT("Snapshot 含一个 Visual"), Snapshot.GetDescriptors().Num(), 1);

	TestEqual(TEXT("相同 Revision 是幂等空操作"),
		Journal.Upsert(Descriptor), EElementVisualMutationResult::Unchanged);
	TestEqual(TEXT("重复 Revision 不推进 Sequence"), Journal.GetCurrentSequence(Shard), uint64(1));
	TestEqual(TEXT("重复 Revision 不唤醒消费者"), WakeCount, 1);

	for (uint64 Revision = 2; Revision <= 3; ++Revision)
	{
		Descriptor.Revision = Revision;
		Descriptor.Intensity = static_cast<float>(Revision);
		TestEqual(TEXT("更高 Revision 局部更新"),
			Journal.Upsert(Descriptor), EElementVisualMutationResult::Applied);
	}
	TestEqual(TEXT("三次提交后 Sequence 为 3"), Journal.GetCurrentSequence(Shard), uint64(3));

	TArray<FElementVisualChangeBatch> Batches;
	TestEqual(TEXT("落后有界历史只报告本 Shard Gap"),
		Journal.ReadChangesAfter(Shard, 0, Batches), EElementVisualJournalReadResult::Gap);
	TestEqual(TEXT("仍在历史内只读取增量"),
		Journal.ReadChangesAfter(Shard, 1, Batches), EElementVisualJournalReadResult::Changes);
	TestEqual(TEXT("保留两批"), Batches.Num(), 2);
	TestEqual(TEXT("最后增量 Revision"), Batches.Last().Changes[0].Descriptor.Revision, uint64(3));

	TestEqual(TEXT("RuntimeEvict 使用明确删除语义"),
		Journal.Remove(Shard, Descriptor.Key, EElementVisualChangeKind::RuntimeEvict, 4),
		EElementVisualMutationResult::Applied);
	TestTrue(TEXT("删除后仍可复制空 Snapshot"), Journal.CopyShardSnapshot(Shard, Snapshot));
	TestEqual(TEXT("删除后 Snapshot 为空"), Snapshot.GetDescriptors().Num(), 0);
	TestEqual(TEXT("唤醒仅包含变化 Shard"), LastWake, TArray<FElementVisualShardKey>{Shard});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementVisualJournalAtomicTransactionTest,
	"ElementSandbox.Simulation.Visual.Journal.AtomicMultiShardTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementVisualJournalAtomicTransactionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Simulation::Visual::Tests;
	FElementVisualJournal Journal;
	const FElementVisualShardKey First{{0, 0, 0}};
	const FElementVisualShardKey Second{{1, 0, 0}};
	int32 WakeCount = 0;
	TArray<FElementVisualShardKey> WokenShards;
	Journal.OnChangesAvailable().AddLambda(
		[&WakeCount, &WokenShards](const TArray<FElementVisualShardKey>& Shards)
		{
			++WakeCount;
			WokenShards = Shards;
		});

	TestTrue(TEXT("开始 Visual Journal 事务"), Journal.BeginTransaction());
	TestEqual(TEXT("事务内写第一个 Shard"),
		Journal.Upsert(MakeDescriptor(201, First, 1, FVector(50.0))),
		EElementVisualMutationResult::Applied);
	TestEqual(TEXT("事务内写第二个 Shard"),
		Journal.Upsert(MakeDescriptor(202, Second, 1, FVector(10050.0, 0.0, 0.0))),
		EElementVisualMutationResult::Applied);
	TestEqual(TEXT("提交前不推进第一个 Cursor"), Journal.GetCurrentSequence(First), uint64(0));
	TestEqual(TEXT("提交前不推进第二个 Cursor"), Journal.GetCurrentSequence(Second), uint64(0));
	TestEqual(TEXT("提交前不唤醒"), WakeCount, 0);
	TestTrue(TEXT("原子提交跨 Shard 事务"), Journal.CommitTransaction());
	TestEqual(TEXT("一次事务只唤醒一次"), WakeCount, 1);
	TestEqual(TEXT("唤醒包含两个排序后的 Shard"), WokenShards, TArray<FElementVisualShardKey>({First, Second}));
	TestEqual(TEXT("第一个 Shard 独立 Sequence"), Journal.GetCurrentSequence(First), uint64(1));
	TestEqual(TEXT("第二个 Shard 独立 Sequence"), Journal.GetCurrentSequence(Second), uint64(1));

	TestTrue(TEXT("可开始取消事务"), Journal.BeginTransaction());
	TestEqual(TEXT("取消事务内更新先暂存"),
		Journal.Remove(First, MakeDescriptor(201, First, 1, FVector(50.0)).Key,
			EElementVisualChangeKind::GameplayDestroy, 2),
		EElementVisualMutationResult::Applied);
	Journal.CancelTransaction();
	FElementVisualShardSnapshot Snapshot;
	TestTrue(TEXT("取消后 Snapshot 仍存在"), Journal.CopyShardSnapshot(First, Snapshot));
	TestEqual(TEXT("取消没有移除已提交 Visual"), Snapshot.GetDescriptors().Num(), 1);
	TestEqual(TEXT("取消不推进 Sequence"), Snapshot.Cursor, uint64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementVisualJournalCrossShardMoveTest,
	"ElementSandbox.Simulation.Visual.Journal.CrossShardMoveKeepsFinalLocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementVisualJournalCrossShardMoveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Simulation::Visual::Tests;
	FElementVisualJournal Journal;
	const FElementVisualShardKey OldShard{{2, 0, 0}};
	const FElementVisualShardKey NewShard{{-1, 0, 0}};
	FElementVisualDescriptor Descriptor = MakeDescriptor(301, OldShard, 1, FVector(20050.0, 50.0, 50.0));
	TestEqual(TEXT("先写入旧 Shard"), Journal.Upsert(Descriptor), EElementVisualMutationResult::Applied);

	TArray<FElementVisualShardKey> ChangedShards;
	Journal.OnChangesAvailable().AddLambda(
		[&ChangedShards](const TArray<FElementVisualShardKey>& Shards)
		{
			ChangedShards = Shards;
		});
	TestTrue(TEXT("开始跨 Shard 原子移动"), Journal.BeginTransaction());
	TestEqual(TEXT("旧 Shard 发布明确 LeaveInterest"),
		Journal.Remove(OldShard, Descriptor.Key, EElementVisualChangeKind::LeaveInterest, 2),
		EElementVisualMutationResult::Applied);
	Descriptor.Shard = NewShard;
	Descriptor.WorldTransform.SetLocation(FVector(-9950.0, 50.0, 50.0));
	Descriptor.WorldBounds = FBox::BuildAABB(FVector(-9950.0, 50.0, 50.0), FVector(25.0));
	Descriptor.Revision = 3;
	TestEqual(TEXT("新 Shard 同事务 Upsert"),
		Journal.Upsert(Descriptor), EElementVisualMutationResult::Applied);
	TestTrue(TEXT("提交跨 Shard 移动"), Journal.CommitTransaction());
	TestEqual(TEXT("变化 Shard 按 Key 排序且一次唤醒"),
		ChangedShards, TArray<FElementVisualShardKey>({NewShard, OldShard}));

	FElementVisualShardSnapshot OldSnapshot;
	FElementVisualShardSnapshot NewSnapshot;
	Journal.CopyShardSnapshot(OldShard, OldSnapshot);
	Journal.CopyShardSnapshot(NewShard, NewSnapshot);
	TestEqual(TEXT("旧 Shard 已移除"), OldSnapshot.GetDescriptors().Num(), 0);
	TestEqual(TEXT("新 Shard 有且只有移动实体"), NewSnapshot.GetDescriptors().Num(), 1);
	TestEqual(TEXT("新 Shard 保存移动 Revision"),
		NewSnapshot.GetDescriptors()[0].Revision, uint64(3));

	Descriptor.Revision = 4;
	Descriptor.Intensity = 4.0f;
	TestEqual(TEXT("移动后的全局 Location 指向新 Shard"),
		Journal.Upsert(Descriptor), EElementVisualMutationResult::Applied);
	Journal.CopyShardSnapshot(NewShard, NewSnapshot);
	TestEqual(TEXT("后续更新仍命中新 Shard"),
		NewSnapshot.GetDescriptors()[0].Revision, uint64(4));
	return true;
}

#endif
