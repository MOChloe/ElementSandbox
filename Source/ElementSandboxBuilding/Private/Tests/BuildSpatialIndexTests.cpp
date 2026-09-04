#if WITH_DEV_AUTOMATION_TESTS

#include "Entity/BuildEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Spatial/BuildAABBTree.h"
#include "Spatial/BuildDynamicAABBTree.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Spatial/BuildSpatialEntry.h"

#include <limits>

namespace
{
	FBox MakeBounds(const FVector& Center, const FVector& Extent = FVector(4.0))
	{
		return FBox(Center - Extent, Center + Extent);
	}

	TSet<FBuildEntityHandle> MakeEntitySet(const TArray<FBuildEntityHandle>& Entities)
	{
		TSet<FBuildEntityHandle> Result;
		Result.Reserve(Entities.Num());
		for (const FBuildEntityHandle Entity : Entities)
		{
			Result.Add(Entity);
		}
		return Result;
	}

	bool WaitForSnapshotWorkers(
		FBuildSpatialIndex& Index,
		const double TimeoutSeconds = 5.0)
	{
		const double EndTime = FPlatformTime::Seconds() + TimeoutSeconds;
		while (Index.GetAsyncSnapshotInFlightCount() > 0
			&& FPlatformTime::Seconds() < EndTime)
		{
			FPlatformProcess::SleepNoStats(0.001f);
		}
		return Index.GetAsyncSnapshotInFlightCount() == 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSpatialChunkCoordinateTest,
	"ElementSandbox.Building.Spatial.ChunkCoordinate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildSpatialChunkCoordinateTest::RunTest(const FString& Parameters)
{
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 100.0;

	FIntVector Chunk = FIntVector::ZeroValue;
	TestTrue(TEXT("有限世界位置可以映射到 Chunk"),
		Config.TryGetChunkCoordinate(FVector(100.0, -0.1, 250.0), Chunk));
	TestTrue(TEXT("Chunk 坐标使用向下取整并正确处理负数"),
		Chunk == FIntVector(1, -1, 2));

	const double InvalidCoordinate = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("拒绝非有限世界位置"),
		Config.TryGetChunkCoordinate(FVector(InvalidCoordinate, 0.0, 0.0), Chunk));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildStaticAABBTreeTest,
	"ElementSandbox.Building.Spatial.StaticAABBTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildStaticAABBTreeTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	TArray<FBuildSpatialEntry> Entries;
	Entries.Reserve(125);
	for (int32 X = 0; X < 5; ++X)
	{
		for (int32 Y = 0; Y < 5; ++Y)
		{
			for (int32 Z = 0; Z < 5; ++Z)
			{
				Entries.Add({
					Registry.CreateEntity(),
					MakeBounds(FVector(X * 40.0, Y * 40.0, Z * 40.0))});
			}
		}
	}

	FBuildAABBTree Tree;
	Tree.Build(Entries);
	TestEqual(TEXT("Snapshot 保存全部叶数据"), Tree.Num(), Entries.Num());
	TestTrue(TEXT("非空 Snapshot 建立层级节点"), Tree.GetNodeCount() > 1);

	const TArray<FBox> Queries = {
		FBox(FVector(-5.0), FVector(45.0)),
		FBox(FVector(70.0, 30.0, -10.0), FVector(130.0, 95.0, 50.0)),
		FBox(FVector(-1000.0), FVector(1000.0)),
		FBox(FVector(500.0), FVector(600.0))};

	for (int32 QueryIndex = 0; QueryIndex < Queries.Num(); ++QueryIndex)
	{
		TSet<FBuildEntityHandle> Expected;
		for (const FBuildSpatialEntry& Entry : Entries)
		{
			if (Entry.Bounds.Intersect(Queries[QueryIndex]))
			{
				Expected.Add(Entry.Entity);
			}
		}

		TArray<FBuildEntityHandle> ActualArray;
		Tree.Query(Queries[QueryIndex], ActualArray);
		const TSet<FBuildEntityHandle> Actual = MakeEntitySet(ActualArray);
		TestEqual(
			FString::Printf(TEXT("Query %d 与暴力查询数量一致"), QueryIndex),
			Actual.Num(),
			Expected.Num());
		for (const FBuildEntityHandle Entity : Expected)
		{
			TestTrue(
				FString::Printf(TEXT("Query %d 不漏掉相交叶"), QueryIndex),
				Actual.Contains(Entity));
		}
	}

	Tree.Reset();
	TArray<FBuildEntityHandle> EmptyResults;
	Tree.Query(FBox(FVector(-100.0), FVector(100.0)), EmptyResults);
	TestTrue(TEXT("Reset 后 Snapshot 为空"), EmptyResults.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDynamicAABBTreeTest,
	"ElementSandbox.Building.Spatial.DynamicAABBTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildDynamicAABBTreeTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildDynamicAABBTree Tree(20.0);
	TMap<FBuildEntityHandle, FBox> ExpectedBounds;
	TArray<FBuildEntityHandle> Handles;
	Handles.Reserve(96);

	for (int32 Index = 0; Index < 96; ++Index)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		const FBox Bounds = MakeBounds(FVector(
			(Index % 12) * 30.0,
			((Index / 12) % 4) * 35.0,
			(Index / 48) * 45.0));
		Handles.Add(Entity);
		ExpectedBounds.Add(Entity, Bounds);
		TestTrue(FString::Printf(TEXT("插入 Dynamic 叶 %d"), Index), Tree.Insert(Entity, Bounds));
	}

	TestEqual(TEXT("Dynamic Tree 记录全部叶"), Tree.Num(), Handles.Num());
	TestTrue(TEXT("初始 Dynamic Tree 层级有效"), Tree.Validate());
	TestFalse(TEXT("拒绝重复叶"), Tree.Insert(Handles[0], ExpectedBounds.FindChecked(Handles[0])));

	for (int32 Index = 0; Index < Handles.Num(); Index += 3)
	{
		const FBox NewBounds = MakeBounds(FVector(500.0 + Index * 2.0, -200.0, 25.0));
		TestTrue(TEXT("越过 Fat Bounds 的 Update 成功"), Tree.Update(Handles[Index], NewBounds));
		ExpectedBounds.FindChecked(Handles[Index]) = NewBounds;
	}

	for (int32 Index = 1; Index < Handles.Num(); Index += 5)
	{
		TestTrue(TEXT("Dynamic 叶删除成功"), Tree.Remove(Handles[Index]));
		ExpectedBounds.Remove(Handles[Index]);
	}
	TestTrue(TEXT("Update/Remove 后 Dynamic Tree 层级有效"), Tree.Validate());

	const TArray<FBox> Queries = {
		FBox(FVector(-10.0), FVector(100.0)),
		FBox(FVector(490.0, -220.0, 0.0), FVector(800.0, -180.0, 50.0)),
		FBox(FVector(-1000.0), FVector(1000.0)),
		FBox(FVector(2000.0), FVector(2100.0))};

	for (int32 QueryIndex = 0; QueryIndex < Queries.Num(); ++QueryIndex)
	{
		TSet<FBuildEntityHandle> Expected;
		for (const TPair<FBuildEntityHandle, FBox>& Pair : ExpectedBounds)
		{
			if (Pair.Value.Intersect(Queries[QueryIndex]))
			{
				Expected.Add(Pair.Key);
			}
		}

		TArray<FBuildEntityHandle> ActualArray;
		Tree.Query(Queries[QueryIndex], ActualArray);
		const TSet<FBuildEntityHandle> Actual = MakeEntitySet(ActualArray);
		TestEqual(
			FString::Printf(TEXT("Dynamic Query %d 与暴力查询数量一致"), QueryIndex),
			Actual.Num(),
			Expected.Num());
		for (const FBuildEntityHandle Entity : Expected)
		{
			TestTrue(
				FString::Printf(TEXT("Dynamic Query %d 不漏叶"), QueryIndex),
				Actual.Contains(Entity));
		}
	}

	// Query 会二次检查精确 Bounds，不能把 Fat Bounds 的宽松命中暴露给调用方。
	FBuildDynamicAABBTree PaddingTree(100.0);
	const FBuildEntityHandle PaddingEntity = Registry.CreateEntity();
	const uint64 PaddingRevisionBeforeInsert = PaddingTree.GetRevision();
	TestTrue(TEXT("插入 Padding 测试叶"),
		PaddingTree.Insert(PaddingEntity, MakeBounds(FVector::ZeroVector, FVector(1.0)), 7));
	TestEqual(TEXT("Dynamic Tree 插入沿父链累计 Cost"), PaddingTree.GetTotalCost(), 7);
	TestEqual(TEXT("Dynamic Tree 插入推进 Revision"),
		PaddingTree.GetRevision(), PaddingRevisionBeforeInsert + 1);
	const uint64 PaddingRevisionBeforeCostUpdate = PaddingTree.GetRevision();
	TestTrue(TEXT("Fat Bounds 内只更新 Cost 不重插"), PaddingTree.Update(
		PaddingEntity,
		MakeBounds(FVector::ZeroVector, FVector(1.0)),
		11));
	TestEqual(TEXT("Dynamic Tree Cost 更新沿父链生效"), PaddingTree.GetTotalCost(), 11);
	TestEqual(TEXT("Dynamic Tree Cost 更新推进 Revision"),
		PaddingTree.GetRevision(), PaddingRevisionBeforeCostUpdate + 1);
	TArray<FBuildEntityHandle> PaddingResults;
	PaddingTree.Query(FBox(FVector(50.0), FVector(51.0)), PaddingResults);
	TestTrue(TEXT("Fat Bounds 不产生公共查询假阳性"), PaddingResults.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildHybridSpatialIndexTest,
	"ElementSandbox.Building.Spatial.HybridChunkIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildHybridSpatialIndexTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 100.0;
	Config.DynamicBoundsPadding = 10.0;
	Config.MaxChunksPerEntity = 8;
	FBuildSpatialIndex Index(Config);

	const FBuildEntityHandle CrossingEntity = Registry.CreateEntity();
	const FBuildEntityHandle SameChunkEntity = Registry.CreateEntity();
	const FBuildEntityHandle RemoteEntity = Registry.CreateEntity();
	const FBox CrossingBounds(FVector(90.0, 10.0, 10.0), FVector(110.0, 20.0, 20.0));
	const FBox SameChunkBounds(FVector(40.0, 10.0, 10.0), FVector(50.0, 20.0, 20.0));
	const FBox RemoteBounds(FVector(1000.0, -200.0, 0.0), FVector(1010.0, -190.0, 10.0));

	TestTrue(TEXT("插入跨 Chunk Entity"), Index.Insert(
		CrossingEntity, CrossingBounds, EBuildSpatialMobility::Static));
	TestTrue(TEXT("插入同 Chunk Entity"), Index.Insert(
		SameChunkEntity, SameChunkBounds, EBuildSpatialMobility::Static));
	TestTrue(TEXT("插入远端 Entity"), Index.Insert(
		RemoteEntity, RemoteBounds, EBuildSpatialMobility::Static));
	TestFalse(TEXT("空间索引拒绝重复 Entity"), Index.Insert(
		CrossingEntity, CrossingBounds, EBuildSpatialMobility::Static));
	TestEqual(TEXT("空间索引拥有三个 Entity"), Index.GetEntityCount(), 3);

	TArray<FBuildEntityHandle> Results;
	FBuildSpatialQueryScratch Scratch;
	Index.QueryOverlaps(
		FBox(FVector(95.0, 0.0, 0.0), FVector(105.0, 30.0, 30.0)),
		Scratch,
		Results);
	TestEqual(TEXT("跨 Chunk Entity 查询结果会去重"), Results.Num(), 1);
	TestTrue(TEXT("跨 Chunk Query 命中正确 Entity"), Results.Contains(CrossingEntity));

	const int32 DirtyBeforeRebuild = Index.GetDirtyStaticChunkCount();
	TestTrue(TEXT("初次插入产生 Dirty Chunk"), DirtyBeforeRebuild > 0);
	TestEqual(TEXT("所有 Dirty Chunk 均完成 Snapshot 重建"),
		Index.RebuildDirtyStaticChunks(), DirtyBeforeRebuild);
	TestEqual(TEXT("重建后没有 Dirty Chunk"), Index.GetDirtyStaticChunkCount(), 0);

	const FBox SameChunkUpdatedBounds(
		FVector(45.0, 10.0, 10.0),
		FVector(55.0, 20.0, 20.0));
	TestTrue(TEXT("Snapshot Entity 可在同一 Chunk 内更新"),
		Index.Update(SameChunkEntity, SameChunkUpdatedBounds));
	Index.QueryOverlaps(
		FBox(FVector(39.0, 10.0, 10.0), FVector(42.0, 20.0, 20.0)),
		Scratch,
		Results);
	TestTrue(TEXT("同 Chunk 更新后精确查询不返回旧 Bounds"), Results.IsEmpty());
	Index.QueryOverlaps(
		FBox(FVector(52.0, 10.0, 10.0), FVector(58.0, 20.0, 20.0)),
		Scratch,
		Results);
	TestEqual(TEXT("同 Chunk 更新后命中新 Bounds"), Results.Num(), 1);
	TestTrue(TEXT("同 Chunk 更新返回正确 Entity"), Results.Contains(SameChunkEntity));

	// Snapshot 中的旧叶由 Tombstone 屏蔽，新位置进入 Dynamic Tree。
	const FBox UpdatedBounds(FVector(20.0, 10.0, 10.0), FVector(30.0, 20.0, 20.0));
	TestTrue(TEXT("Snapshot Entity 可更新到新 Bounds"),
		Index.Update(CrossingEntity, UpdatedBounds));
	Index.QueryOverlaps(
		FBox(FVector(95.0, 0.0, 0.0), FVector(105.0, 30.0, 30.0)),
		Scratch,
		Results);
	TestTrue(TEXT("Tombstone 屏蔽 Snapshot 旧位置"), Results.IsEmpty());
	Index.QueryOverlaps(
		FBox(FVector(15.0, 0.0, 0.0), FVector(35.0, 30.0, 30.0)),
		Scratch,
		Results);
	TestEqual(TEXT("Dynamic 增量区返回新位置一次"), Results.Num(), 1);
	TestTrue(TEXT("更新后命中新位置"), Results.Contains(CrossingEntity));

	FBox StoredBounds(ForceInit);
	TestTrue(TEXT("可读取当前权威 Bounds"), Index.TryGetBounds(CrossingEntity, StoredBounds));
	TestTrue(TEXT("权威 Bounds 已更新"), StoredBounds == UpdatedBounds);
	TestTrue(TEXT("移除更新后的 Entity"), Index.Remove(CrossingEntity));
	Index.QueryOverlaps(
		FBox(FVector(15.0, 0.0, 0.0), FVector(35.0, 30.0, 30.0)),
		Scratch,
		Results);
	TestTrue(TEXT("删除后 Snapshot/Delta 都不再返回 Entity"), Results.IsEmpty());

	Index.QueryOverlaps(
		FBox(FVector(-100000.0), FVector(100000.0)),
		Scratch,
		Results);
	TestEqual(TEXT("巨型 Query 扫描 Sparse Chunk 并返回剩余 Entity"), Results.Num(), 2);
	TestTrue(TEXT("巨型 Query 包含同 Chunk Entity"), Results.Contains(SameChunkEntity));
	TestTrue(TEXT("巨型 Query 包含远端 Entity"), Results.Contains(RemoteEntity));

	const FBuildEntityHandle OversizedEntity = Registry.CreateEntity();
	TestFalse(TEXT("拒绝覆盖超过上限 Chunk 的异常 Bounds"),
		Index.Insert(
			OversizedEntity,
			FBox(FVector(-1000.0), FVector(1000.0)),
			EBuildSpatialMobility::Static));
	TestFalse(TEXT("拒绝无效 Handle"),
		Index.Insert(
			FBuildEntityHandle(),
			MakeBounds(FVector::ZeroVector),
			EBuildSpatialMobility::Static));

	TestTrue(TEXT("销毁已移出空间索引的旧 Entity"),
		Registry.DestroyEntity(CrossingEntity));
	const FBuildEntityHandle RecycledEntity = Registry.CreateEntity();
	TestEqual(TEXT("空间测试复用相同 Entity Slot"),
		RecycledEntity.GetIndex(), CrossingEntity.GetIndex());
	TestTrue(TEXT("新 Generation 可以独立插入 Slot 数组"), Index.Insert(
		RecycledEntity,
		MakeBounds(FVector(300.0, 10.0, 10.0)),
		EBuildSpatialMobility::Static));
	TestFalse(TEXT("旧 Generation 不会误命中新空间 Record"),
		Index.Contains(CrossingEntity));
	TestTrue(TEXT("新 Generation 命中自己的空间 Record"),
		Index.Contains(RecycledEntity));

	Index.Reset();
	TestEqual(TEXT("Reset 清空空间 Entity"), Index.GetEntityCount(), 0);
	TestEqual(TEXT("Reset 清空 Sparse Chunk"), Index.GetChunkCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSpatialRayQueryTest,
	"ElementSandbox.Building.Spatial.RayQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildSpatialRayQueryTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 100.0;
	Config.DynamicBoundsPadding = 75.0;
	Config.MaxChunksPerEntity = 8;
	FBuildSpatialIndex Index(Config);

	const FBuildEntityHandle SnapshotEntity = Registry.CreateEntity();
	const FBuildEntityHandle DeltaEntity = Registry.CreateEntity();
	const FBuildEntityHandle DynamicEntity = Registry.CreateEntity();
	const FBuildEntityHandle FatBoundsMiss = Registry.CreateEntity();
	TestTrue(TEXT("插入跨 Chunk 的静态射线目标"), Index.Insert(
		SnapshotEntity,
		FBox(FVector(90.0, 10.0, 10.0), FVector(110.0, 20.0, 20.0)),
		EBuildSpatialMobility::Static));
	TestEqual(TEXT("静态目标进入 Snapshot"), Index.RebuildDirtyStaticChunks(), 2);
	TestTrue(TEXT("插入 Snapshot 后的静态 Delta 目标"), Index.Insert(
		DeltaEntity,
		FBox(FVector(160.0, 10.0, 10.0), FVector(180.0, 20.0, 20.0)),
		EBuildSpatialMobility::Static));
	TestTrue(TEXT("插入永久动态射线目标"), Index.Insert(
		DynamicEntity,
		FBox(FVector(250.0, 10.0, 10.0), FVector(270.0, 20.0, 20.0)),
		EBuildSpatialMobility::Dynamic));
	TestTrue(TEXT("插入只会被 Fat Bounds 覆盖的目标"), Index.Insert(
		FatBoundsMiss,
		FBox(FVector(140.0, 50.0, 10.0), FVector(150.0, 60.0, 20.0)),
		EBuildSpatialMobility::Dynamic));

	TArray<FBuildSpatialRayHit> Hits;
	FBuildSpatialQueryScratch Scratch;
	const FVector RayOrigin(0.0, 15.0, 15.0);
	Index.QueryRay(
		RayOrigin,
		FVector(2.0, 0.0, 0.0),
		300.0,
		Scratch,
		Hits);
	TestEqual(TEXT("Snapshot、Delta 与 Dynamic 各返回一次"), Hits.Num(), 3);
	if (Hits.Num() == 3)
	{
		TestTrue(TEXT("结果按最近 Bounds 距离排序"), Hits[0].Entity == SnapshotEntity);
		TestEqual(TEXT("最近命中距离正确"), Hits[0].Distance, 90.0);
		TestTrue(TEXT("第二项来自 Static Delta"), Hits[1].Entity == DeltaEntity);
		TestEqual(TEXT("Static Delta 距离正确"), Hits[1].Distance, 160.0);
		TestTrue(TEXT("第三项来自永久 Dynamic Tree"), Hits[2].Entity == DynamicEntity);
		TestEqual(TEXT("Dynamic 距离正确"), Hits[2].Distance, 250.0);
	}

	TestTrue(TEXT("Snapshot Entity 更新到射线外"), Index.Update(
		SnapshotEntity,
		FBox(FVector(90.0, 80.0, 10.0), FVector(110.0, 90.0, 20.0))));
	Index.QueryRay(RayOrigin, FVector::ForwardVector, 300.0, Scratch, Hits);
	TestEqual(TEXT("Tombstone 屏蔽旧 Snapshot 射线叶"), Hits.Num(), 2);
	TestFalse(TEXT("更新后的旧位置不再命中"),
		Hits.ContainsByPredicate(
			[SnapshotEntity](const FBuildSpatialRayHit& Hit)
			{
				return Hit.Entity == SnapshotEntity;
			}));

	Hits.Add({DynamicEntity, 1.0});
	Index.QueryRay(RayOrigin, FVector::ZeroVector, 300.0, Scratch, Hits);
	TestTrue(TEXT("无效方向会清空并拒绝查询"), Hits.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPersistentDynamicSpatialIndexTest,
	"ElementSandbox.Building.Spatial.PersistentDynamicIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPersistentDynamicSpatialIndexTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 100.0;
	Config.DynamicBoundsPadding = 20.0;
	Config.MaxChunksPerEntity = 8;
	FBuildSpatialIndex Index(Config);

	const FBuildEntityHandle StaticEntity = Registry.CreateEntity();
	const FBuildEntityHandle DynamicEntity = Registry.CreateEntity();
	const FBox StaticBounds(FVector(10.0), FVector(20.0));
	const FBox InitialDynamicBounds(FVector(30.0), FVector(40.0));
	TestTrue(TEXT("插入 Snapshot 候选 Entity"), Index.Insert(
		StaticEntity, StaticBounds, EBuildSpatialMobility::Static));
	TestTrue(TEXT("插入永久动态 Entity"), Index.Insert(
		DynamicEntity, InitialDynamicBounds, EBuildSpatialMobility::Dynamic));
	TestEqual(TEXT("永久动态插入不额外制造静态 Dirty Chunk"),
		Index.GetDirtyStaticChunkCount(), 1);
	TestEqual(TEXT("初次静态 Snapshot 重建一个 Chunk"),
		Index.RebuildDirtyStaticChunks(), 1);
	TestEqual(TEXT("初次重建后静态 Dirty 清零"),
		Index.GetDirtyStaticChunkCount(), 0);
	TArray<FBuildEntityHandle> Results;
	FBuildSpatialQueryScratch Scratch;
	Index.QueryOverlaps(FBox(FVector(25.0), FVector(45.0)), Scratch, Results);
	TestEqual(TEXT("同 Chunk 的永久动态 Entity 不会被重建清空"), Results.Num(), 1);
	TestTrue(TEXT("重建后返回正确永久动态 Entity"), Results.Contains(DynamicEntity));

	const FBox MovedDynamicBounds(
		FVector(130.0, 10.0, 10.0),
		FVector(140.0, 20.0, 20.0));
	TestTrue(TEXT("永久动态 Entity 可跨 Chunk 更新"),
		Index.Update(DynamicEntity, MovedDynamicBounds));
	TestEqual(TEXT("动态跨 Chunk 不触发静态 Snapshot 重建"),
		Index.GetDirtyStaticChunkCount(), 0);
	TestEqual(TEXT("没有静态 Dirty 时不会执行重建"),
		Index.RebuildDirtyStaticChunks(), 0);

	Index.QueryOverlaps(
		FBox(FVector(25.0), FVector(45.0)),
		Scratch,
		Results);
	TestFalse(TEXT("动态 Entity 不再出现在旧位置"), Results.Contains(DynamicEntity));
	Index.QueryOverlaps(
		FBox(FVector(125.0, 0.0, 0.0), FVector(145.0, 30.0, 30.0)),
		Scratch,
		Results);
	TestEqual(TEXT("动态 Entity 在新 Chunk 返回一次"), Results.Num(), 1);
	TestTrue(TEXT("新 Chunk 返回正确动态 Entity"), Results.Contains(DynamicEntity));

	EBuildSpatialMobility StoredMobility = EBuildSpatialMobility::Static;
	TestTrue(TEXT("可读取动态空间分类"),
		Index.TryGetMobility(DynamicEntity, StoredMobility));
	TestTrue(TEXT("动态空间分类保持不变"),
		StoredMobility == EBuildSpatialMobility::Dynamic);

	TestTrue(TEXT("静态 Entity 可提升为永久动态"),
		Index.SetMobility(StaticEntity, EBuildSpatialMobility::Dynamic));
	TestEqual(TEXT("提升会使旧 Snapshot 产生 Tombstone"),
		Index.GetDirtyStaticChunkCount(), 1);
	Index.QueryOverlaps(FBox(FVector(5.0), FVector(25.0)), Scratch, Results);
	TestEqual(TEXT("提升过程中 Entity 不会从查询消失"), Results.Num(), 1);
	TestTrue(TEXT("提升后仍返回原 Entity"), Results.Contains(StaticEntity));
	TestEqual(TEXT("提升后的静态 Snapshot 可完成清理"),
		Index.RebuildDirtyStaticChunks(), 1);
	Index.QueryOverlaps(FBox(FVector(5.0), FVector(25.0)), Scratch, Results);
	TestEqual(TEXT("Snapshot 重建不会吞掉永久动态 Entity"), Results.Num(), 1);
	TestTrue(TEXT("重建后永久动态 Entity 仍可查询"), Results.Contains(StaticEntity));

	const FBox PromotedMovedBounds(
		FVector(60.0, 10.0, 10.0),
		FVector(70.0, 20.0, 20.0));
	TestTrue(TEXT("提升后的 Entity 走动态更新路径"),
		Index.Update(StaticEntity, PromotedMovedBounds));
	TestEqual(TEXT("提升后的移动不污染静态 Dirty 状态"),
		Index.GetDirtyStaticChunkCount(), 0);

	TestTrue(TEXT("永久动态 Entity 可降回静态分类"),
		Index.SetMobility(DynamicEntity, EBuildSpatialMobility::Static));
	TestEqual(TEXT("降为静态后进入 Static Delta"),
		Index.GetDirtyStaticChunkCount(), 1);
	TestEqual(TEXT("降级后的 Static Delta 可进入 Snapshot"),
		Index.RebuildDirtyStaticChunks(), 1);
	Index.QueryOverlaps(
		FBox(FVector(125.0, 0.0, 0.0), FVector(145.0, 30.0, 30.0)),
		Scratch,
		Results);
	TestEqual(TEXT("降级重建后仍返回 Entity"), Results.Num(), 1);
	TestTrue(TEXT("降级重建后身份不变"), Results.Contains(DynamicEntity));

	const FBuildEntityHandle PendingStaticEntity = Registry.CreateEntity();
	const FBox PendingStaticBounds(
		FVector(220.0, 10.0, 10.0),
		FVector(230.0, 20.0, 20.0));
	TestTrue(TEXT("插入尚未进入 Snapshot 的静态 Entity"), Index.Insert(
		PendingStaticEntity,
		PendingStaticBounds,
		EBuildSpatialMobility::Static));
	TestEqual(TEXT("新静态 Entity 产生一个 Dirty Chunk"),
		Index.GetDirtyStaticChunkCount(), 1);
	TestTrue(TEXT("Snapshot 重建前也可以提升为动态"),
		Index.SetMobility(PendingStaticEntity, EBuildSpatialMobility::Dynamic));
	TestEqual(TEXT("已移出 Static Delta 时不留下空重建任务"),
		Index.GetDirtyStaticChunkCount(), 0);
	TestEqual(TEXT("没有静态差异时不执行空 Snapshot 重建"),
		Index.RebuildDirtyStaticChunks(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDynamicAABBTreeRandomizedTest,
	"ElementSandbox.Building.Spatial.DynamicAABBTreeRandomized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildDynamicAABBTreeRandomizedTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildDynamicAABBTree Tree(15.0);
	FRandomStream Random(0x5A17BEEF);
	TArray<FBuildEntityHandle> ActiveEntities;
	TMap<FBuildEntityHandle, FBox> ExpectedBounds;

	const auto MakeRandomBounds = [&Random]()
	{
		const FVector Center(
			Random.FRandRange(-2000.0f, 2000.0f),
			Random.FRandRange(-2000.0f, 2000.0f),
			Random.FRandRange(-500.0f, 500.0f));
		const FVector Extent(
			Random.FRandRange(1.0f, 40.0f),
			Random.FRandRange(1.0f, 40.0f),
			Random.FRandRange(1.0f, 40.0f));
		return MakeBounds(Center, Extent);
	};

	for (int32 Index = 0; Index < 192; ++Index)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		const FBox Bounds = MakeRandomBounds();
		if (!Tree.Insert(Entity, Bounds))
		{
			AddError(FString::Printf(TEXT("随机压力初始化插入失败：%d"), Index));
			return false;
		}
		ActiveEntities.Add(Entity);
		ExpectedBounds.Add(Entity, Bounds);
	}

	for (int32 Step = 0; Step < 1200; ++Step)
	{
		const int32 EntityIndex = Random.RandRange(0, ActiveEntities.Num() - 1);
		const FBuildEntityHandle Entity = ActiveEntities[EntityIndex];
		if ((Step % 7) == 0)
		{
			if (!Tree.Remove(Entity))
			{
				AddError(FString::Printf(TEXT("随机压力删除失败：Step %d"), Step));
				return false;
			}
			ExpectedBounds.Remove(Entity);

			const FBuildEntityHandle Replacement = Registry.CreateEntity();
			const FBox ReplacementBounds = MakeRandomBounds();
			if (!Tree.Insert(Replacement, ReplacementBounds))
			{
				AddError(FString::Printf(TEXT("随机压力替换插入失败：Step %d"), Step));
				return false;
			}
			ActiveEntities[EntityIndex] = Replacement;
			ExpectedBounds.Add(Replacement, ReplacementBounds);
		}
		else
		{
			const FBox NewBounds = MakeRandomBounds();
			if (!Tree.Update(Entity, NewBounds))
			{
				AddError(FString::Printf(TEXT("随机压力更新失败：Step %d"), Step));
				return false;
			}
			ExpectedBounds.FindChecked(Entity) = NewBounds;
		}

		if (!Tree.Validate())
		{
			AddError(FString::Printf(TEXT("Dynamic Tree 层级在 Step %d 损坏"), Step));
			return false;
		}

		if ((Step % 10) == 0)
		{
			const FVector QueryCenter(
				Random.FRandRange(-1800.0f, 1800.0f),
				Random.FRandRange(-1800.0f, 1800.0f),
				Random.FRandRange(-400.0f, 400.0f));
			const FBox QueryBounds = MakeBounds(QueryCenter, FVector(250.0, 250.0, 100.0));

			TSet<FBuildEntityHandle> Expected;
			for (const TPair<FBuildEntityHandle, FBox>& Pair : ExpectedBounds)
			{
				if (Pair.Value.Intersect(QueryBounds))
				{
					Expected.Add(Pair.Key);
				}
			}

			TArray<FBuildEntityHandle> ActualArray;
			Tree.Query(QueryBounds, ActualArray);
			const TSet<FBuildEntityHandle> Actual = MakeEntitySet(ActualArray);
			if (Actual.Num() != Expected.Num())
			{
				AddError(FString::Printf(
					TEXT("随机 Query 数量不一致：Step %d，Expected %d，Actual %d"),
					Step,
					Expected.Num(),
					Actual.Num()));
				return false;
			}
			for (const FBuildEntityHandle ExpectedEntity : Expected)
			{
				if (!Actual.Contains(ExpectedEntity))
				{
					AddError(FString::Printf(TEXT("随机 Query 漏叶：Step %d"), Step));
					return false;
				}
			}
		}
	}

	TestEqual(TEXT("随机压力后叶数量稳定"), Tree.Num(), ActiveEntities.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSpatialRayDDAAndScratchReuseTest,
	"ElementSandbox.Building.Spatial.RayDDAAndScratchReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildSpatialRayDDAAndScratchReuseTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 100.0;
	Config.MaxChunksPerEntity = 8;
	FBuildSpatialIndex Index(Config);

	TArray<FBuildEntityHandle> ExpectedDiagonalEntities;
	for (const double Coordinate : {-150.0, -50.0, 50.0, 150.0})
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		ExpectedDiagonalEntities.Add(Entity);
		TestTrue(TEXT("插入长斜射线目标"), Index.Insert(
			Entity,
			MakeBounds(FVector(Coordinate), FVector(4.0)),
			EBuildSpatialMobility::Static));
	}

	const FBuildEntityHandle CrossChunkEntity = Registry.CreateEntity();
	ExpectedDiagonalEntities.Add(CrossChunkEntity);
	TestTrue(TEXT("插入跨八个 Chunk 的边界目标"), Index.Insert(
		CrossChunkEntity,
		FBox(FVector(95.0), FVector(105.0)),
		EBuildSpatialMobility::Static));

	FBuildSpatialQueryScratch Scratch;
	TArray<FBuildSpatialRayHit> Hits;
	Index.QueryRay(
		FVector(-250.0),
		FVector(1.0),
		800.0,
		Scratch,
		Hits);
	TestEqual(TEXT("3D DDA 找到全部长斜射线目标"),
		Hits.Num(), ExpectedDiagonalEntities.Num());
	for (const FBuildEntityHandle Entity : ExpectedDiagonalEntities)
	{
		int32 MatchingHitCount = 0;
		for (const FBuildSpatialRayHit& Hit : Hits)
		{
			MatchingHitCount += Hit.Entity == Entity ? 1 : 0;
		}
		TestEqual(
			TEXT("跨 Chunk Entity 在长斜射线中只返回一次"),
			MatchingHitCount,
			1);
	}

	const int32 CandidateCapacity = Scratch.Candidates.Max();
	const SIZE_T UniqueAllocation = Scratch.UniqueEntities.GetAllocatedSize();
	const int32 HitCapacity = Hits.Max();
	Index.QueryRay(
		FVector(-250.0),
		FVector(1.0),
		800.0,
		Scratch,
		Hits);
	TestEqual(TEXT("预热后 Candidate 容量不再增长"),
		Scratch.Candidates.Max(), CandidateCapacity);
	TestEqual(TEXT("预热后去重集合容量不再增长"),
		Scratch.UniqueEntities.GetAllocatedSize(), UniqueAllocation);
	TestEqual(TEXT("预热后 Hit 容量不再增长"), Hits.Max(), HitCapacity);

	const FBuildEntityHandle NegativeBoundaryEntity = Registry.CreateEntity();
	TestTrue(TEXT("插入负坐标边界目标"), Index.Insert(
		NegativeBoundaryEntity,
		MakeBounds(FVector(-10.0, 0.0, 0.0), FVector(2.0)),
		EBuildSpatialMobility::Static));
	Index.QueryRay(
		FVector::ZeroVector,
		-FVector::ForwardVector,
		20.0,
		Scratch,
		Hits);
	TestTrue(TEXT("从 Chunk 边界向负方向立即进入负 Chunk"),
		Hits.ContainsByPredicate(
			[NegativeBoundaryEntity](const FBuildSpatialRayHit& Hit)
			{
				return Hit.Entity == NegativeBoundaryEntity;
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildAsyncStaticSnapshotVersioningTest,
	"ElementSandbox.Building.Spatial.AsyncSnapshotVersioning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildAsyncStaticSnapshotVersioningTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 1000.0;
	Config.AsyncSnapshotMinimumStaticEntries = 1;
	Config.AsyncSnapshotMinimumDeltaEntries = 1;
	Config.AsyncSnapshotMinimumTombstones = 1;
	Config.AsyncSnapshotIdleSeconds = 0.0;
	FBuildSpatialIndex Index(Config);

	const FBuildEntityHandle Entity = Registry.CreateEntity();
	const FBox InitialBounds = MakeBounds(FVector(10.0));
	const FBox UpdatedBounds = MakeBounds(FVector(200.0));
	TestTrue(TEXT("插入异步 Snapshot 静态叶"), Index.Insert(
		Entity,
		InitialBounds,
		EBuildSpatialMobility::Static));
	const FBuildSpatialSnapshotWorkStats CaptureStats =
		Index.ProcessAsyncSnapshotWork();
	TestEqual(TEXT("首轮只捕获一个 Chunk"), CaptureStats.CapturedChunks, 1);
	TestTrue(TEXT("捕获后修改 Static Version"), Index.Update(Entity, UpdatedBounds));
	TestTrue(TEXT("等待首个后台 BVH 完成"), WaitForSnapshotWorkers(Index));

	const FBuildSpatialSnapshotWorkStats StaleStats =
		Index.ProcessAsyncSnapshotWork();
	TestEqual(TEXT("版本不一致的结果被丢弃"),
		StaleStats.DiscardedStaleChunks, 1);
	TestEqual(TEXT("过期结果不会发布"), StaleStats.PublishedChunks, 0);
	TestEqual(TEXT("Idle 条件允许重新捕获最新版本"),
		StaleStats.CapturedChunks, 1);
	TestTrue(TEXT("等待重试 BVH 完成"), WaitForSnapshotWorkers(Index));

	const FBuildSpatialSnapshotWorkStats PublishStats =
		Index.ProcessAsyncSnapshotWork();
	TestEqual(TEXT("匹配版本只发布一个 Chunk"), PublishStats.PublishedChunks, 1);
	TestEqual(TEXT("发布后 Static Dirty 清零"),
		Index.GetDirtyStaticChunkCount(), 0);

	FBuildSpatialQueryScratch Scratch;
	TArray<FBuildEntityHandle> Results;
	Index.QueryOverlaps(UpdatedBounds, Scratch, Results);
	TestEqual(TEXT("异步发布结果与当前权威 Bounds 一致"), Results.Num(), 1);
	TestTrue(TEXT("异步 Snapshot 返回正确 Entity"), Results.Contains(Entity));
	Index.QueryOverlaps(InitialBounds, Scratch, Results);
	TestTrue(TEXT("异步 Snapshot 不复活过期 Bounds"), Results.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildAsyncStaticSnapshotChunkRecreationTest,
	"ElementSandbox.Building.Spatial.AsyncSnapshotChunkRecreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildAsyncStaticSnapshotChunkRecreationTest::RunTest(
	const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 1000.0;
	Config.AsyncSnapshotMinimumStaticEntries = 1;
	Config.AsyncSnapshotMinimumDeltaEntries = 1;
	Config.AsyncSnapshotMinimumTombstones = 1;
	Config.AsyncSnapshotIdleSeconds = 0.0;
	FBuildSpatialIndex Index(Config);

	const FBox OldBounds = MakeBounds(FVector(10.0));
	const FBox NewBounds = MakeBounds(FVector(200.0));
	const FBuildEntityHandle OldEntity = Registry.CreateEntity();
	TestTrue(TEXT("插入即将被销毁的静态 Entity"), Index.Insert(
		OldEntity,
		OldBounds,
		EBuildSpatialMobility::Static));
	TestEqual(TEXT("捕获旧 Chunk 版本"),
		Index.ProcessAsyncSnapshotWork().CapturedChunks, 1);

	TestTrue(TEXT("后台构建期间移除旧 Entity"), Index.Remove(OldEntity));
	TestTrue(TEXT("销毁旧 Entity 以复用 Slot"), Registry.DestroyEntity(OldEntity));
	const FBuildEntityHandle NewEntity = Registry.CreateEntity();
	TestEqual(TEXT("Registry 复用相同 Slot"),
		NewEntity.GetIndex(), OldEntity.GetIndex());
	TestTrue(TEXT("同一 Chunk 立即插入新一代 Entity"), Index.Insert(
		NewEntity,
		NewBounds,
		EBuildSpatialMobility::Static));
	TestTrue(TEXT("等待旧版本后台 BVH 完成"), WaitForSnapshotWorkers(Index));

	const FBuildSpatialSnapshotWorkStats StaleStats =
		Index.ProcessAsyncSnapshotWork();
	TestEqual(TEXT("Chunk 删除重建期间旧结果必须过期"),
		StaleStats.DiscardedStaleChunks, 1);
	TestEqual(TEXT("旧 Snapshot 不得发布到新一代 Chunk"),
		StaleStats.PublishedChunks, 0);
	TestEqual(TEXT("立即为当前 Chunk 重新捕获"),
		StaleStats.CapturedChunks, 1);
	TestTrue(TEXT("等待当前版本后台 BVH 完成"), WaitForSnapshotWorkers(Index));
	TestEqual(TEXT("当前版本成功发布"),
		Index.ProcessAsyncSnapshotWork().PublishedChunks, 1);

	FBuildSpatialQueryScratch Scratch;
	TArray<FBuildEntityHandle> Results;
	Index.QueryOverlaps(NewBounds, Scratch, Results);
	TestEqual(TEXT("重建后只返回新一代 Entity"), Results.Num(), 1);
	TestTrue(TEXT("重建后命中当前 Entity Handle"), Results.Contains(NewEntity));
	Index.QueryOverlaps(OldBounds, Scratch, Results);
	TestTrue(TEXT("旧 Snapshot 不复活已销毁 Entity"), Results.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildAsyncStaticSnapshotBudgetAndResetTest,
	"ElementSandbox.Building.Spatial.AsyncSnapshotBudgetAndReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildAsyncStaticSnapshotBudgetAndResetTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 100.0;
	Config.AsyncSnapshotMinimumStaticEntries = 1;
	Config.AsyncSnapshotMinimumDeltaEntries = 1;
	Config.AsyncSnapshotMinimumTombstones = 1;
	Config.AsyncSnapshotIdleSeconds = 0.0;
	Config.AsyncSnapshotMaxCapturesPerTick = 3;
	Config.AsyncSnapshotMaxConcurrentBuilds = 2;
	FBuildSpatialIndex Index(Config);

	for (int32 ChunkIndex = 0; ChunkIndex < 3; ++ChunkIndex)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		TestTrue(TEXT("插入独立异步 Chunk"), Index.Insert(
			Entity,
			MakeBounds(FVector(ChunkIndex * 100.0 + 10.0, 10.0, 10.0)),
			EBuildSpatialMobility::Static));
	}
	const FBuildSpatialSnapshotWorkStats Stats = Index.ProcessAsyncSnapshotWork();
	TestEqual(TEXT("并发预算限制同帧捕获数"), Stats.CapturedChunks, 2);
	TestTrue(TEXT("在途任务不超过并发上限"),
		Index.GetAsyncSnapshotInFlightCount() <= 2);

	Index.Reset();
	TestEqual(TEXT("Reset 立即清空新异步状态的在途计数"),
		Index.GetAsyncSnapshotInFlightCount(), 0);
	TestFalse(TEXT("Reset 后不保留旧任务的待处理状态"),
		Index.HasPendingAsyncSnapshotWork());
	FPlatformProcess::SleepNoStats(0.02f);
	TestEqual(TEXT("旧后台任务完成后不能回写已 Reset Index"),
		Index.GetEntityCount(), 0);

	const FBuildEntityHandle DynamicEntity = Registry.CreateEntity();
	TestTrue(TEXT("Reset 后插入永久 Dynamic Entity"), Index.Insert(
		DynamicEntity,
		MakeBounds(FVector(10.0)),
		EBuildSpatialMobility::Dynamic));
	TestFalse(TEXT("Dynamic Entity 不进入 Static Snapshot 调度"),
		Index.HasPendingAsyncSnapshotWork());
	TestEqual(TEXT("Dynamic Entity 不会被捕获"),
		Index.ProcessAsyncSnapshotWork().CapturedChunks, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildAsyncStaticSnapshotScheduleCandidateBudgetTest,
	"ElementSandbox.Building.Spatial.AsyncSnapshotScheduleCandidateBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildAsyncStaticSnapshotScheduleCandidateBudgetTest::RunTest(
	const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildSpatialIndexConfig Config;
	Config.ChunkSize = 100.0;
	Config.AsyncSnapshotMinimumStaticEntries = 1;
	Config.AsyncSnapshotMinimumDeltaEntries = 1;
	Config.AsyncSnapshotMinimumTombstones = 1;
	Config.AsyncSnapshotIdleSeconds = 1000.0;
	Config.AsyncSnapshotMaxCapturesPerTick = 1;
	Config.AsyncSnapshotMaxConcurrentBuilds = 1;
	Config.AsyncSnapshotMaxScheduleCandidatesPerTick = 64;
	FBuildSpatialIndex Index(Config);

	constexpr int32 ChunkCount = 400;
	TArray<FBuildEntityHandle> Entities;
	Entities.Reserve(ChunkCount);
	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		Entities.Add(Entity);
		TestTrue(TEXT("为 Snapshot 调度压力插入独立 Chunk"), Index.Insert(
			Entity,
			MakeBounds(FVector(ChunkIndex * 100.0 + 10.0, 10.0, 10.0)),
			EBuildSpatialMobility::Static));
	}

	// 保留 FIFO 尾部一个 Ready Chunk；前面的取消节点用于证明单 Tick
	// 调度检查不会因为同帧大批失效而扫完整队列。
	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount - 1; ++ChunkIndex)
	{
		TestTrue(TEXT("移除已进入 Ready FIFO 的 Chunk"),
			Index.Remove(Entities[ChunkIndex]));
	}
	TestEqual(TEXT("取消后只剩一个 Dirty Static Chunk"),
		Index.GetDirtyStaticChunkCount(), 1);
	TestTrue(TEXT("尾部有效 Ready Chunk 仍使异步工作可见"),
		Index.HasPendingAsyncSnapshotWork());

	const FBuildSpatialSnapshotWorkStats FirstStats =
		Index.ProcessAsyncSnapshotWork();
	TestEqual(TEXT("首 Tick 最多检查 64 个调度节点"),
		FirstStats.CheckedScheduleCandidates, 64);
	TestEqual(TEXT("预算耗尽前不会越过 64 个取消节点捕获尾部 Chunk"),
		FirstStats.CapturedChunks, 0);

	int32 CapturedChunks = 0;
	for (int32 TickIndex = 0; TickIndex < 8 && CapturedChunks == 0; ++TickIndex)
	{
		const FBuildSpatialSnapshotWorkStats Stats =
			Index.ProcessAsyncSnapshotWork();
		TestTrue(TEXT("后续 Tick 的候选检查仍不超过预算"),
			Stats.CheckedScheduleCandidates <= 64);
		CapturedChunks += Stats.CapturedChunks;
	}
	TestEqual(TEXT("增量跳过取消节点后最终捕获唯一有效 Chunk"),
		CapturedChunks, 1);
	TestTrue(TEXT("等待候选预算测试的后台构建完成"),
		WaitForSnapshotWorkers(Index));
	return true;
}

#endif
