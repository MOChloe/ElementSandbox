#if WITH_DEV_AUTOMATION_TESTS

#include "Definition/WorldObjectDefinition.h"
#include "Collision/WorldObjectCollisionWorldSubsystem.h"
#include "Chunk/WorldChunkTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "Tests/WorldObjectTestFailingDefinition.h"
#include "Tests/WorldObjectTestPhysicsDefinition.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldStorageSubsystem.h"

namespace ElementSandbox::WorldObjects::Tests
{
FBox MakeBounds(const FVector& Center, const FVector& Extent = FVector(5.0))
{
	return FBox(Center - Extent, Center + Extent);
}

void AdvancePostActorFrame(UWorld& World, const float DeltaSeconds = 1.0f / 60.0f)
{
	// RunTest 同步执行于一个引擎帧；显式推进帧号来模拟多次真实 Post-Actor Tick。
	++GFrameCounter;
	FWorldDelegates::OnWorldPostActorTick.Broadcast(&World, LEVELTICK_All, DeltaSeconds);
}

struct FTestWorld final
{
	explicit FTestWorld(const FName Name)
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, Name, nullptr, true);
		check(World);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		Subsystem = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	}

	~FTestWorld()
	{
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	}

	UWorld* World = nullptr;
	UWorldObjectWorldSubsystem* Subsystem = nullptr;
};

AActor* MakeProxyActor(UWorld& World, UWorldObjectProxyComponent*& OutProxy)
{
	AActor* Actor = World.SpawnActor<AActor>();
	USphereComponent* Root = NewObject<USphereComponent>(Actor, TEXT("TestRoot"));
	Actor->AddInstanceComponent(Root);
	Actor->SetRootComponent(Root);
	Root->SetCastShadow(false);
	Root->RegisterComponent();

	OutProxy = NewObject<UWorldObjectProxyComponent>(Actor, TEXT("TestProxy"));
	Actor->AddInstanceComponent(OutProxy);
	OutProxy->RegisterComponent();
	return Actor;
}
} // namespace ElementSandbox::WorldObjects::Tests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectEntityRegistryTest,
								 "ElementSandbox.WorldObjects.Entity.HandleAndFragmentPools",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectEntityRegistryTest::RunTest(const FString& Parameters)
{
	FWorldObjectEntityRegistry Registry;
	const FWorldObjectEntityHandle First = Registry.CreateEntity();
	const FWorldObjectEntityHandle Second = Registry.CreateEntity();
	FWorldObjectTransformFragment FirstTransform;
	FirstTransform.WorldTransform.SetLocation(FVector(10.0, 20.0, 30.0));
	FWorldObjectTransformFragment SecondTransform;
	SecondTransform.WorldTransform.SetLocation(FVector(40.0, 50.0, 60.0));

	TestTrue(TEXT("新 Handle 带 Registry、Slot 和 Generation"), First.IsSet());
	TestTrue(TEXT("连续 Pool 接受第一个 Fragment"), Registry.AddFragment(First, FirstTransform));
	TestTrue(TEXT("连续 Pool 接受第二个 Fragment"), Registry.AddFragment(Second, SecondTransform));
	TestFalse(TEXT("同一 Entity 拒绝重复 Fragment"), Registry.AddFragment(First, FirstTransform));
	TestEqual(TEXT("Pool 记录两行"), Registry.GetFragmentCount<FWorldObjectTransformFragment>(), 2);
	const TWorldObjectFragmentPoolView<FWorldObjectTransformFragment> Batch =
		Registry.GetFragmentPoolView<FWorldObjectTransformFragment>();
	TestTrue(TEXT("批量视图保持 Entity 与 Transform 行一一对应"), Batch.IsValid() && Batch.Num() == 2);
	TWorldObjectMutableFragmentPoolView<FWorldObjectTransformFragment> MutableBatch =
		Registry.GetMutableFragmentPoolView<FWorldObjectTransformFragment>();
	TestTrue(TEXT("受生命周期约束的可写 Pool View 与 Owner 行一致"), MutableBatch.IsValid() && MutableBatch.Num() == 2);
	MutableBatch.Fragments[0].WorldTransform.SetLocation(FVector(70.0, 80.0, 90.0));
	TestTrue(TEXT("Mutable Pool View 原位写回连续 Fragment"),
			 Registry.FindFragment<FWorldObjectTransformFragment>(MutableBatch.Entities[0])
				 ->WorldTransform.GetLocation()
				 .Equals(FVector(70.0, 80.0, 90.0)));

	TestTrue(TEXT("销毁中间记录会级联移除 Fragment"), Registry.DestroyEntity(First));
	TestNull(TEXT("旧 Handle 不能命中 Swap-Remove 后的行"),
			 Registry.FindFragment<FWorldObjectTransformFragment>(First));
	const FWorldObjectTransformFragment* Remaining = Registry.FindFragment<FWorldObjectTransformFragment>(Second);
	TestTrue(TEXT("Swap-Remove 后另一 Entity 的值不串行"),
			 Remaining && Remaining->WorldTransform.GetLocation().Equals(FVector(40.0, 50.0, 60.0)));

	const FWorldObjectEntityHandle Reused = Registry.CreateEntity();
	TestEqual(TEXT("空闲 Slot 被复用"), Reused.GetSlot(), First.GetSlot());
	TestNotEqual(TEXT("复用 Slot 推进 Generation"), Reused.GetGeneration(), First.GetGeneration());
	TestFalse(TEXT("旧 Generation 永久失效"), Registry.IsAlive(First));

	FWorldObjectEntityRegistry OtherRegistry;
	TestFalse(TEXT("跨 Registry Handle 被拒绝"), Registry.IsAlive(OtherRegistry.CreateEntity()));
	const FWorldObjectEntityHandle BeforeReset = Reused;
	const uint32 BeforeResetRegistryId = Registry.GetRegistryId();
	Registry.Reset();
	const FWorldObjectEntityHandle AfterReset = Registry.CreateEntity();
	TestNotEqual(TEXT("Reset 重新分配 RegistryId"), Registry.GetRegistryId(), BeforeResetRegistryId);
	TestFalse(TEXT("Reset 前 Handle 永久失效"), Registry.IsAlive(BeforeReset));
	TestTrue(TEXT("Reset 后可以重新创建 Entity"), Registry.IsAlive(AfterReset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectPagedSparseFragmentIndexTest,
								 "ElementSandbox.WorldObjects.Entity.PagedSparseFragmentIndex",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPagedSparseFragmentIndexTest::RunTest(const FString& Parameters)
{
	FWorldObjectEntityRegistry Registry;
	FWorldObjectEntityHandle First;
	FWorldObjectEntityHandle HighSlot;
	for (int32 Index = 0; Index <= 65536; ++Index)
	{
		const FWorldObjectEntityHandle Entity = Registry.CreateEntity();
		if (Index == 0)
		{
			First = Entity;
		}
		HighSlot = Entity;
	}

	FWorldObjectTransformFragment Transform;
	Transform.WorldTransform.SetLocation(FVector(10.0, 20.0, 30.0));
	TestTrue(TEXT("高编号 Slot 可以添加稀有 Fragment"), Registry.AddFragment(HighSlot, Transform));
	FWorldObjectEntityRegistryStorageStats Stats = Registry.GetStorageStats();
	TestEqual(TEXT("稀有 Fragment 只创建一个 Pool"), Stats.FragmentPoolCount, 1);
	TestEqual(TEXT("高编号 Slot 只分配一个 256-Slot 索引页"), Stats.SparseIndexPageCount, 1);
	TestTrue(TEXT("Dense Fragment 容量只由实际行数决定"),
			 Stats.DenseFragmentCapacity >= 1 && Stats.DenseFragmentCapacity < 16);

	TestTrue(TEXT("同一 Pool 可在低编号 Slot 分配第二页"), Registry.AddFragment(First, Transform));
	Stats = Registry.GetStorageStats();
	TestEqual(TEXT("相距很远的两个 Slot 使用两个索引页"), Stats.SparseIndexPageCount, 2);

	TestTrue(TEXT("移除高编号 Fragment"), Registry.RemoveFragment<FWorldObjectTransformFragment>(HighSlot));
	Stats = Registry.GetStorageStats();
	TestEqual(TEXT("空高编号页立即释放并裁剪"), Stats.SparseIndexPageCount, 1);
	TestTrue(TEXT("低编号 Fragment 仍可解析"), Registry.FindFragment<FWorldObjectTransformFragment>(First) != nullptr);

	TestTrue(TEXT("销毁最后一个拥有者只访问其实际 Pool"), Registry.DestroyEntity(First));
	Stats = Registry.GetStorageStats();
	TestEqual(TEXT("Pool 为空后释放最后一个索引页"), Stats.SparseIndexPageCount, 0);
	TestEqual(TEXT("稳定 PoolId 在 Reset 前仍保留空 Pool"), Stats.FragmentPoolCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectSpatialIndexTest,
								 "ElementSandbox.WorldObjects.Spatial.StaticAndPortableTrees",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectSpatialIndexTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	FWorldObjectEntityRegistry Registry;
	FWorldObjectSpatialConfig Config;
	Config.DynamicBoundsPadding = 20.0;
	FWorldObjectSpatialIndex Index(Config);
	TArray<FWorldObjectEntityHandle> StaticEntities;
	for (int32 Number = 0; Number < 32; ++Number)
	{
		const FWorldObjectEntityHandle Entity = Registry.CreateEntity();
		StaticEntities.Add(Entity);
		TestTrue(TEXT("批量插入 PermanentStatic"), Index.Insert(Entity, MakeBounds(FVector(Number * 30.0, 0.0, 0.0)),
																EWorldObjectSpatialClass::PermanentStatic));
	}
	TestTrue(TEXT("静态成员变化只标 Dirty"), Index.IsStaticDirty());
	TestTrue(TEXT("32 条以内的 Chunk 提交为连续数组"), Index.RebuildStaticIfDirty());
	TestEqual(TEXT("连续数组不构建 BVH"), Index.GetStaticBuildCount(), uint64(0));
	TestEqual(TEXT("当前只有一个线性 Chunk"), Index.GetStaticLinearChunkCount(), 1);
	const FWorldObjectEntityHandle DenseStatic = Registry.CreateEntity();
	StaticEntities.Add(DenseStatic);
	TestTrue(
		TEXT("第 33 条记录仍只标记当前 Chunk Dirty"),
		Index.Insert(DenseStatic, MakeBounds(FVector(960.0, 0.0, 0.0)), EWorldObjectSpatialClass::PermanentStatic));
	FWorldObjectSpatialQueryScratch DirtyQueryScratch;
	TArray<FWorldObjectEntityHandle> DirtyQueryHits;
	Index.QueryOverlaps(FBox(FVector(940.0, -20.0, -20.0), FVector(980.0, 20.0, 20.0)), DirtyQueryScratch,
						DirtyQueryHits);
	TestTrue(TEXT("Dirty Chunk 查询使用连续扫描仍返回命中"), DirtyQueryHits.Contains(DenseStatic));
	TestEqual(TEXT("查询不得同步触发 BVH 构建"), Index.GetStaticBuildCount(), uint64(0));
	TestTrue(TEXT("查询后 Dirty 状态仍由批量提交点拥有"), Index.IsStaticDirty());
	TestTrue(TEXT("显式提交只为受影响的密集 Chunk 构建 BVH"), Index.RebuildStaticIfDirty());
	TestEqual(TEXT("33 条只合并为一次小 BVH 构建"), Index.GetStaticBuildCount(), uint64(1));
	TestEqual(TEXT("当前只有一个 BVH Chunk"), Index.GetStaticBVHChunkCount(), 1);
	TestFalse(TEXT("无新变化不会重复构建"), Index.RebuildStaticIfDirty());
	TestFalse(TEXT("PermanentStatic 拒绝 Transform 更新"),
			  Index.UpdatePortable(StaticEntities[0], MakeBounds(FVector(500.0, 0.0, 0.0))));

	const FWorldObjectEntityHandle Portable = Registry.CreateEntity();
	TestTrue(TEXT("插入 Portable Dynamic Tree"),
			 Index.Insert(Portable, MakeBounds(FVector(100.0, 0.0, 0.0)), EWorldObjectSpatialClass::Portable));
	TestTrue(TEXT("初始 Dynamic Tree 有效"), Index.ValidateDynamicTree());
	const uint64 InitialReinsertCount = Index.GetDynamicReinsertCount();
	TestTrue(TEXT("Fat Bounds 内精确 Bounds 可更新"),
			 Index.UpdatePortable(Portable, MakeBounds(FVector(105.0, 0.0, 0.0))));
	TestEqual(TEXT("Fat Bounds 内移动不重插"), Index.GetDynamicReinsertCount(), InitialReinsertCount);
	TestTrue(TEXT("越出 Fat Bounds 的移动会重插"),
			 Index.UpdatePortable(Portable, MakeBounds(FVector(300.0, 0.0, 0.0))));
	TestEqual(TEXT("只记录一次实际重插"), Index.GetDynamicReinsertCount(), InitialReinsertCount + 1);
	TestTrue(TEXT("旋转平衡后的 Dynamic Tree 有效"), Index.ValidateDynamicTree());

	FWorldObjectSpatialQueryScratch Scratch;
	TArray<FWorldObjectEntityHandle> Overlaps;
	Index.QueryOverlaps(FBox(FVector(290.0, -10.0, -10.0), FVector(310.0, 10.0, 10.0)), Scratch, Overlaps);
	TestTrue(TEXT("Dormant Portable 留在 Dynamic Tree 且可查询"), Overlaps.Contains(Portable));
	Index.QueryOverlaps(FBox(FVector(80.0, -10.0, -10.0), FVector(120.0, 10.0, 10.0)), Scratch, Overlaps);
	TestFalse(TEXT("查询用精确 Bounds 复核，不返回旧 Fat Bounds 假阳性"), Overlaps.Contains(Portable));
	Index.QueryPortableOverlaps(FBox(FVector(-10.0), FVector(10.0)), Scratch, Overlaps);
	TestEqual(TEXT("Portable Overlap 不访问 Static BVH"), Scratch.LastVisitedStaticNodes, 0);
	TestTrue(TEXT("Portable Overlap 不返回静态对象"), Overlaps.IsEmpty());

	TArray<FWorldObjectSpatialRayHit> RayHits;
	Index.QueryRay(FVector(-20.0, 0.0, 0.0), FVector::ForwardVector, 1000.0, Scratch, RayHits);
	TestFalse(TEXT("Ray 同时查询两棵树"), RayHits.IsEmpty());
	for (int32 IndexInHits = 1; IndexInHits < RayHits.Num(); ++IndexInHits)
	{
		TestTrue(TEXT("Ray 结果按距离后 Handle 确定排序"),
				 RayHits[IndexInHits - 1].Distance < RayHits[IndexInHits].Distance ||
					 (RayHits[IndexInHits - 1].Distance == RayHits[IndexInHits].Distance &&
					  RayHits[IndexInHits - 1].Entity < RayHits[IndexInHits].Entity));
	}
	Index.QueryPortableRay(FVector(-20.0, 0.0, 0.0), FVector::ForwardVector, 1000.0, Scratch, RayHits);
	TestEqual(TEXT("Portable 热路径不遍历 Static BVH"), Scratch.LastVisitedStaticNodes, 0);
	TestTrue(TEXT("Portable 热路径只返回 Dynamic Tree 命中"), RayHits.Num() == 1 && RayHits[0].Entity == Portable);

	TestTrue(TEXT("删除 PermanentStatic 标记所属 Chunk"), Index.Remove(DenseStatic));
	TestTrue(TEXT("回落到 32 条时释放该 Chunk 的 BVH"), Index.RebuildStaticIfDirty());
	TestEqual(TEXT("回落线性扫描不增加 BVH 构建数"), Index.GetStaticBuildCount(), uint64(1));
	TestEqual(TEXT("回落后恢复一个线性 Chunk"), Index.GetStaticLinearChunkCount(), 1);
	TestTrue(TEXT("删除 Portable"), Index.Remove(Portable));
	TestTrue(TEXT("删除后 Dynamic Tree 仍有效"), Index.ValidateDynamicTree());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectPersistentBatchLifecycleTest,
								 "ElementSandbox.WorldObjects.Persistence.AtomicBatchLifecycle",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPersistentBatchLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	FTestWorld Harness(TEXT("WorldObjectPersistentBatchLifecycle"));
	UWorldObjectDefinition* Definition = NewObject<UWorldObjectDefinition>(Harness.World);
	Definition->DefinitionId = TEXT("Test.PersistentBatch");
	Definition->SpatialClass = EWorldObjectSpatialClass::PermanentStatic;
	Definition->InteractionLocalBounds = MakeBounds(FVector::ZeroVector, FVector(20.0));
	UWorldObjectTestFailingDefinition* Failing = NewObject<UWorldObjectTestFailingDefinition>(Harness.World);
	TestTrue(TEXT("批量测试 Definition 注册"),
			 Harness.Subsystem->RegisterDefinition(*Definition) && Harness.Subsystem->RegisterDefinition(*Failing));

	auto MakePersistentRecord = [](const uint64 Id, const FName DefinitionId, const FVector& Location)
	{
		FWorldPersistentEntityRecord Record;
		Record.EntityId = FWorldEntityId(Id);
		Record.Domain = EWorldEntityDomain::WorldObject;
		Record.DefinitionId = DefinitionId;
		Record.WorldTransform.SetLocation(Location);
		Record.StateRevision = 1;
		return Record;
	};
	const FWorldChunkCoord HomeChunk(0, 0, 0);
	FWorldPersistentEntityRecord First =
		MakePersistentRecord(5001, Definition->DefinitionId, FVector(100.0, 100.0, 0.0));
	FWorldPersistentEntityRecord Second =
		MakePersistentRecord(5002, Definition->DefinitionId, FVector(200.0, 100.0, 0.0));
	FWorldPersistentEntityRecord FailingRecord =
		MakePersistentRecord(5003, Failing->DefinitionId, FVector(300.0, 100.0, 0.0));
	int32 UpsertBatchCount = 0;
	int32 LastUpsertSize = 0;
	int32 EvictBatchCount = 0;
	int32 LastEvictSize = 0;
	const FDelegateHandle UpsertHandle = Harness.Subsystem->OnEntitiesUpserted().AddLambda(
		[&](const TConstArrayView<FWorldObjectLifecycleRecord> Records)
		{
			++UpsertBatchCount;
			LastUpsertSize = Records.Num();
		});
	const FDelegateHandle EvictHandle = Harness.Subsystem->OnEntitiesRuntimeEvicted().AddLambda(
		[&](const TConstArrayView<FWorldObjectLifecycleRecord> Records)
		{
			++EvictBatchCount;
			LastEvictSize = Records.Num();
		});

	FString Error;
	TArray<FWorldPersistentEntityRecord> AtomicFailure{First, FailingRecord};
	TestFalse(TEXT("任一 Definition 装配失败会拒绝整批 Restore"),
			  Harness.Subsystem->RestorePersistentBatchForTesting(HomeChunk, AtomicFailure, Error));
	TestEqual(TEXT("失败 Restore 不残留先前成功记录"), Harness.Subsystem->GetRuntimeStats().EntityCount, 0);
	TestEqual(TEXT("失败事务不发布生命周期事件"), UpsertBatchCount, 0);

	TArray<FWorldPersistentEntityRecord> RestoreRecords{First, Second};
	TestTrue(TEXT("两条 PermanentStatic 一次 Restore 成功"),
			 Harness.Subsystem->RestorePersistentBatchForTesting(HomeChunk, RestoreRecords, Error));
	TestEqual(TEXT("整批 Restore 只发布一次通知"), UpsertBatchCount, 1);
	TestEqual(TEXT("一次通知携带整批记录"), LastUpsertSize, 2);
	TestEqual(TEXT("两条记录均可由稳定 ID 解析"), Harness.Subsystem->GetRuntimeStats().EntityCount, 2);

	TArray<FWorldPersistentEntityRecord> DuplicateRecords{First, First};
	TestFalse(TEXT("同批重复 ID 在任何写入前拒绝"),
			  Harness.Subsystem->RestorePersistentBatchForTesting(HomeChunk, DuplicateRecords, Error));
	TestEqual(TEXT("重复注入拒绝不发布新事件"), UpsertBatchCount, 1);
	First.StateRevision = 2;
	First.WorldTransform.SetLocation(FVector(150.0, 100.0, 0.0));
	TestTrue(TEXT("同 ID 新 Revision 通过同一批量入口更新"),
			 Harness.Subsystem->RestorePersistentBatchForTesting(HomeChunk, MakeArrayView(&First, 1), Error));
	TestEqual(TEXT("单条更新仍发布一个单元素 Batch"), UpsertBatchCount, 2);
	TestEqual(TEXT("单条 Batch 大小为一"), LastUpsertSize, 1);

	TArray<FWorldEntityId> EvictIds{First.EntityId, Second.EntityId};
	TestTrue(TEXT("RuntimeEvict 整批成功"),
			 Harness.Subsystem->RuntimeEvictPersistentBatchForTesting(HomeChunk, EvictIds, Error));
	TestEqual(TEXT("RuntimeEvict 只发布一次通知"), EvictBatchCount, 1);
	TestEqual(TEXT("RuntimeEvict 通知携带整批记录"), LastEvictSize, 2);
	TestEqual(TEXT("RuntimeEvict 后无 Resident WorldObject"), Harness.Subsystem->GetRuntimeStats().EntityCount, 0);
	Harness.Subsystem->OnEntitiesUpserted().Remove(UpsertHandle);
	Harness.Subsystem->OnEntitiesRuntimeEvicted().Remove(EvictHandle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectSubsystemLifecycleTest,
								 "ElementSandbox.WorldObjects.WorldSubsystem.LifecycleAndActiveArray",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectSubsystemLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	FTestWorld Harness(TEXT("WorldObjectSubsystemLifecycle"));
	TestNotNull(TEXT("Game World 自动创建 WorldObject Subsystem"), Harness.Subsystem);
	if (!Harness.Subsystem)
	{
		return false;
	}

	UWorldObjectDefinition* PortableDefinition = NewObject<UWorldObjectDefinition>(Harness.World);
	PortableDefinition->DefinitionId = TEXT("TestPortable");
	PortableDefinition->SpatialClass = EWorldObjectSpatialClass::Portable;
	PortableDefinition->InteractionLocalBounds = MakeBounds(FVector::ZeroVector);
	FWorldObjectCreateDesc InvalidActiveDesc;
	InvalidActiveDesc.Definition = PortableDefinition;
	InvalidActiveDesc.MotionState = EWorldObjectMotionState::Attached;
	TestFalse(TEXT("Active 创建缺少 Proxy 时原子失败"), Harness.Subsystem->CreateEntity(InvalidActiveDesc).IsSet());
	TestEqual(TEXT("失败创建不残留 Registry 行"), Harness.Subsystem->GetRuntimeStats().EntityCount, 0);
	UWorldObjectTestFailingDefinition* FailingDefinition = NewObject<UWorldObjectTestFailingDefinition>(Harness.World);
	FWorldObjectCreateDesc FailingConfigureDesc;
	FailingConfigureDesc.Definition = FailingDefinition;
	TestFalse(TEXT("Definition 自定义配置失败时原子拒绝创建"),
			  Harness.Subsystem->CreateEntity(FailingConfigureDesc).IsSet());
	TestEqual(TEXT("配置失败回滚核心 Fragment、空间与 Entity 行"), Harness.Subsystem->GetRuntimeStats().EntityCount, 0);

	UWorldObjectProxyComponent* NonAuthorityProxy = nullptr;
	AActor* NonAuthorityProxyActor = MakeProxyActor(*Harness.World, NonAuthorityProxy);
	NonAuthorityProxyActor->SetRole(ROLE_SimulatedProxy);
	FWorldObjectCreateDesc RejectedProxyDesc;
	RejectedProxyDesc.Definition = PortableDefinition;
	RejectedProxyDesc.Proxy = NonAuthorityProxy;
	TestFalse(TEXT("非 Authority Proxy 拒绝分配 WorldEntityId"),
			  Harness.Subsystem->CreateEntity(RejectedProxyDesc).IsSet());
	const FWorldObjectRuntimeStats RejectedProxyStats = Harness.Subsystem->GetRuntimeStats();
	TestEqual(TEXT("Proxy 绑定失败不残留 Registry 行"), RejectedProxyStats.EntityCount, 0);
	TestEqual(TEXT("Proxy 绑定失败不残留 WorldStorage 所有权"), RejectedProxyStats.WorldStorageOwnedEntityCount, 0);
	NonAuthorityProxyActor->Destroy();

	UWorldObjectProxyComponent* Proxy = nullptr;
	AActor* ProxyActor = MakeProxyActor(*Harness.World, Proxy);
	ProxyActor->SetActorLocation(FVector(10.0, 0.0, 0.0));
	FWorldObjectCreateDesc ActiveDesc;
	ActiveDesc.Definition = PortableDefinition;
	ActiveDesc.WorldTransform = ProxyActor->GetActorTransform();
	ActiveDesc.MotionState = EWorldObjectMotionState::Attached;
	ActiveDesc.Proxy = Proxy;
	const FWorldObjectEntityHandle Entity = Harness.Subsystem->CreateEntity(ActiveDesc);
	TestTrue(TEXT("Attached Portable 原子创建"), Entity.IsSet());
	TestTrue(TEXT("WorldEntityId 可解析回同一 Local Handle"),
			 Harness.Subsystem->FindEntity(Harness.Subsystem->GetWorldEntityId(Entity)) == Entity);
	TestEqual(TEXT("Attached 加入 Active Array"), Harness.Subsystem->GetRuntimeStats().ActiveCount, 1);

	ProxyActor->SetActorLocation(FVector(75.0, 0.0, 0.0));
	AdvancePostActorFrame(*Harness.World);
	Harness.Subsystem->EnsurePostActorStateCurrent();
	const FWorldObjectTransformFragment* SyncedTransform =
		Harness.Subsystem->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Entity);
	TestTrue(TEXT("Post-Actor-Tick 只采样 Active Actor 最终 Transform"),
			 SyncedTransform && SyncedTransform->WorldTransform.GetLocation().Equals(FVector(75.0, 0.0, 0.0)));
	TestEqual(TEXT("统计记录一次 Active 访问"), Harness.Subsystem->GetRuntimeStats().LastSampledActiveCount, 1);

	const FWorldEntityId WorldEntityId = Harness.Subsystem->GetWorldEntityId(Entity);
	Harness.Subsystem->QueueProxyMotionState(WorldEntityId, EWorldObjectMotionState::Dormant);
	Harness.Subsystem->QueueProxyMotionState(WorldEntityId, EWorldObjectMotionState::Dormant);
	AdvancePostActorFrame(*Harness.World);
	TestEqual(TEXT("重复 Sleep 幂等并 Swap-Remove Active"), Harness.Subsystem->GetRuntimeStats().ActiveCount, 0);
	TestTrue(TEXT("休眠不移出 Dynamic Tree"), Harness.Subsystem->GetSpatialIndex().Contains(Entity));

	TestTrue(TEXT("Dormant 自定义 Proxy 已关闭碰撞"),
			 Proxy && !Proxy->IsPhysicsProjectionActive() && Proxy->GetPhysicsPrimitive() &&
				 Proxy->GetPhysicsPrimitive()->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestTrue(TEXT("显式激活自定义 Proxy"),
			 Harness.Subsystem->ActivatePhysics(Entity, FVector(100.0, 0.0, 0.0)));
	TestEqual(TEXT("显式激活只加入 Active 一次"), Harness.Subsystem->GetRuntimeStats().ActiveCount, 1);
	TestTrue(TEXT("Attached、Dormant、Physics 全程保持 Handle"),
			 Harness.Subsystem->FindEntity(WorldEntityId) == Entity);

	bool bObservedPreDestroy = false;
	const FDelegateHandle Veto = Harness.Subsystem->OnEntityPreDestroy().AddLambda(
		[&bObservedPreDestroy, Entity](const FWorldObjectEntityHandle Candidate, bool& bCanDestroy)
		{
			bObservedPreDestroy = Candidate == Entity;
			bCanDestroy = false;
		});
	TestFalse(TEXT("Pre-Destroy 可以否决销毁"), Harness.Subsystem->DestroyEntity(Entity));
	TestTrue(TEXT("Pre-Destroy 收到完整 Handle"), bObservedPreDestroy);
	TestTrue(TEXT("否决后 Entity 保持存活"), Harness.Subsystem->IsEntityAlive(Entity));
	Harness.Subsystem->OnEntityPreDestroy().Remove(Veto);
	TestTrue(TEXT("移除否决后销毁成功"), Harness.Subsystem->DestroyEntity(Entity));
	TestFalse(TEXT("销毁后 WorldEntityId 不再解析"), Harness.Subsystem->FindEntity(WorldEntityId).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectPhysicsConfigurationTest,
								 "ElementSandbox.WorldObjects.Physics.ConfigurationValidation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPhysicsConfigurationTest::RunTest(const FString& Parameters)
{
	FWorldObjectPhysicsBodyInit Init;
	Init.MassKg = 12.0f;
	Init.LinearVelocity = FVector(10.0, 20.0, -30.0);
	Init.AngularVelocityDegrees = FVector(0.0, 90.0, 0.0);
	TestTrue(TEXT("有限质量和初速度构成有效 Physics 创建参数"), Init.IsValid());
	Init.CollisionPolicy = static_cast<EWorldObjectPhysicsCollisionPolicy>(255);
	TestFalse(TEXT("未知碰撞策略被创建参数拒绝"), Init.IsValid());
	Init.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::Standard;
	Init.MassKg = 0.0f;
	TestFalse(TEXT("非正质量被拒绝"), Init.IsValid());

	FWorldObjectPhysicsBodyNetState NetState;
	TestFalse(TEXT("未配置的复制值无效"), NetState.IsValid());
	NetState.bConfigured = true;
	NetState.LocalCenter = FVector(2.0, -1.0, 3.0);
	NetState.LocalExtent = FVector(10.0, 20.0, 30.0);
	NetState.MassKg = 12.0f;
	NetState.ActivationRevision = 1;
	TestTrue(TEXT("形状和质量完整时复制值有效"), NetState.IsValid());
	NetState.CollisionPolicy = static_cast<EWorldObjectPhysicsCollisionPolicy>(255);
	TestFalse(TEXT("未知碰撞策略被复制值拒绝"), NetState.IsValid());
	NetState.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::Standard;
	NetState.LocalExtent.Z = 0.0f;
	TestFalse(TEXT("退化 Box 被拒绝"), NetState.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectAuthorityMutationCaptureTest,
	"ElementSandbox.WorldObjects.Network.AuthorityMutationCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectAuthorityMutationCaptureTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	(void)Parameters;

	struct FCapture final
	{
		FWorldStorageEntityMutation Mutation;
		FWorldPersistentEntityRecord Record;
		FString Error;
		bool bSucceeded = false;
	};

	FWorldPersistentEntityRecord DormantRecord;
	{
		FTestWorld Authority(TEXT("WorldObjectAuthorityMutationCapture"));
		UWorldStorageSubsystem* Storage = Authority.World->GetSubsystem<UWorldStorageSubsystem>();
		UWorldObjectTestPhysicsDefinition* Definition =
			NewObject<UWorldObjectTestPhysicsDefinition>(Authority.World);
		Definition->DefinitionId = TEXT("Test.AuthorityMutationCapture");

		TArray<FCapture> Captures;
		const FDelegateHandle CaptureHandle = Storage->OnAuthorityMutation().AddLambda(
			[Storage, &Captures](const FWorldStorageEntityMutation& Mutation)
			{
				if (Mutation.Kind == EWorldStorageMutationKind::GameplayTombstone)
				{
					return;
				}
				FCapture& Capture = Captures.AddDefaulted_GetRef();
				Capture.Mutation = Mutation;
				Capture.bSucceeded = Storage->CaptureResidentRecord(
					Mutation.EntityId, Capture.Record, Capture.Error);
			});

		TArray<FWorldObjectCreateDesc> Descs;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FWorldObjectCreateDesc& Desc = Descs.AddDefaulted_GetRef();
			Desc.Definition = Definition;
			Desc.WorldTransform.SetLocation(FVector(Index * 40.0, 0.0, 120.0));
			Desc.MotionState = EWorldObjectMotionState::Physics;
			Desc.InstanceInteractionBounds = Definition->InteractionLocalBounds;
			Desc.PhysicsBody = FWorldObjectPhysicsBodyInit{};
		}
		FWorldObjectStagedCreateBatch Batch;
		TArray<FWorldObjectEntityHandle> Entities;
		TestTrue(TEXT("Physics 产品整批 Stage 成功"), Authority.Subsystem->StageCreateEntities(Descs, Batch));
		TestTrue(TEXT("Physics 产品整批 Commit 成功"), Authority.Subsystem->CommitStagedCreateEntities(Batch, Entities));
		TestEqual(TEXT("初始创建的每条 Authority Mutation 都能同步捕获"), Captures.Num(), Descs.Num());
		for (const FCapture& Capture : Captures)
		{
			TestTrue(TEXT("初始 Upsert 回调能捕获完整 Resident Record"), Capture.bSucceeded && Capture.Record.IsValid());
			TestEqual(TEXT("初始 Live Delta 内外 Revision 一致"), Capture.Record.StateRevision, Capture.Mutation.StateRevision);
		}

		if (Entities.IsEmpty())
		{
			Storage->OnAuthorityMutation().Remove(CaptureHandle);
			return false;
		}
		const FWorldEntityId SleepingId = Authority.Subsystem->GetWorldEntityId(Entities[0]);
		Authority.Subsystem->QueueProxyMotionState(SleepingId, EWorldObjectMotionState::Dormant);
		AdvancePostActorFrame(*Authority.World);
		const FCapture* SleepCapture = Captures.FindByPredicate(
			[SleepingId](const FCapture& Capture)
			{
				return Capture.Mutation.EntityId == SleepingId && Capture.Mutation.StateRevision > 1;
			});
		TestTrue(TEXT("Physics -> Dormant Mutation 同步捕获成功"),
			SleepCapture && SleepCapture->bSucceeded && SleepCapture->Record.IsValid());
		if (SleepCapture)
		{
			TestEqual(TEXT("Dormant Live Delta 内外 Revision 一致"),
				SleepCapture->Record.StateRevision, SleepCapture->Mutation.StateRevision);
			DormantRecord = SleepCapture->Record;
		}
		Storage->OnAuthorityMutation().Remove(CaptureHandle);
	}

	if (!DormantRecord.IsValid())
	{
		return false;
	}
	FTestWorld Restored(TEXT("WorldObjectAuthorityMutationCaptureRestored"));
	UWorldObjectTestPhysicsDefinition* RestoredDefinition =
		NewObject<UWorldObjectTestPhysicsDefinition>(Restored.World);
	RestoredDefinition->DefinitionId = DormantRecord.DefinitionId;
	TestTrue(TEXT("恢复端注册同一 Physics Definition"), Restored.Subsystem->RegisterDefinition(*RestoredDefinition));
	FString RestoreError;
	const FWorldChunkCoord HomeChunk =
		FWorldChunkCoord::FromWorldLocation(DormantRecord.WorldTransform.GetLocation());
	TestTrue(TEXT("Dormant Mutation 捕获结果可直接 Restore"),
		Restored.Subsystem->RestorePersistentBatchForTesting(
			HomeChunk, MakeArrayView(&DormantRecord, 1), RestoreError));
	const FWorldObjectEntityHandle RestoredEntity =
		Restored.Subsystem->FindEntity(DormantRecord.EntityId);
	const FWorldObjectMotionFragment* RestoredMotion =
		Restored.Subsystem->GetRegistry().FindFragment<FWorldObjectMotionFragment>(RestoredEntity);
	TestTrue(TEXT("同步捕获的 Payload 已经是 Dormant，而不是上一帧 Physics"),
		RestoredMotion && RestoredMotion->State == EWorldObjectMotionState::Dormant &&
			Restored.Subsystem->GetProxy(RestoredEntity) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectPhysicsLifecycleTest, "ElementSandbox.WorldObjects.Physics.ProxyLifecycle",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPhysicsLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	FTestWorld Harness(TEXT("WorldObjectPhysicsLifecycle"));
	UWorldObjectDefinition* Ordinary = NewObject<UWorldObjectDefinition>(Harness.World);
	Ordinary->DefinitionId = TEXT("Test.OrdinaryNoPhysicsBody");
	Ordinary->SpatialClass = EWorldObjectSpatialClass::Portable;
	Ordinary->InteractionLocalBounds = MakeBounds(FVector::ZeroVector);
	FWorldObjectCreateDesc OrdinaryDesc;
	OrdinaryDesc.Definition = Ordinary;
	OrdinaryDesc.MotionState = EWorldObjectMotionState::Physics;
	OrdinaryDesc.InstanceInteractionBounds = Ordinary->InteractionLocalBounds;
	OrdinaryDesc.PhysicsBody = FWorldObjectPhysicsBodyInit{};
	TestFalse(TEXT("没有 Physics Fragment 的 Definition 原子拒绝自动刚体"),
			  Harness.Subsystem->CreateEntity(OrdinaryDesc).IsSet());
	TestEqual(TEXT("错误形态不残留 Entity"), Harness.Subsystem->GetRuntimeStats().EntityCount, 0);

	UWorldObjectTestPhysicsDefinition* Physics = NewObject<UWorldObjectTestPhysicsDefinition>(Harness.World);
	Physics->DefinitionId = TEXT("Test.PhysicsLifecycle");
	FWorldObjectCreateDesc DormantDesc;
	DormantDesc.Definition = Physics;
	DormantDesc.WorldTransform.SetLocation(FVector(0.0, 0.0, 20.0));
	DormantDesc.MotionState = EWorldObjectMotionState::Dormant;
	const FWorldObjectEntityHandle DormantEntity = Harness.Subsystem->CreateEntity(DormantDesc);
	const FWorldObjectPhysicsBodyFragment* DormantBody =
		Harness.Subsystem->GetRegistry().FindFragment<FWorldObjectPhysicsBodyFragment>(DormantEntity);
	TestTrue(TEXT("带 Physics 能力的 Definition 可在 Dormant 态创建且不要求实例刚体参数"),
		DormantEntity.IsSet() && DormantBody && !Harness.Subsystem->GetProxy(DormantEntity));
	TestTrue(TEXT("Dormant 能力实体可正常销毁"), Harness.Subsystem->DestroyEntity(DormantEntity));
	FWorldObjectCreateDesc Desc;
	Desc.Definition = Physics;
	Desc.WorldTransform.SetLocation(FVector(0.0, 0.0, 100.0));
	Desc.MotionState = EWorldObjectMotionState::Physics;
	Desc.InstanceInteractionBounds = FBox(FVector(-12.0, -8.0, -20.0), FVector(12.0, 8.0, 20.0));
	FWorldObjectPhysicsBodyInit BodyInit;
	BodyInit.MassKg = 12.0f;
	BodyInit.AngularVelocityDegrees = FVector(0.0, 0.0, 90.0);
	Desc.PhysicsBody = BodyInit;
	const FWorldObjectEntityHandle Entity = Harness.Subsystem->CreateEntity(Desc);
	TestTrue(TEXT("PhysicsBody 自动创建正式 Entity 与 Proxy"), Entity.IsSet());
	const FWorldObjectPhysicsBodyFragment* Body =
		Harness.Subsystem->GetRegistry().FindFragment<FWorldObjectPhysicsBodyFragment>(Entity);
	TestTrue(TEXT("Definition 选择性装配 Physics Fragment 并写入实例质量"),
			 Body && FMath::IsNearlyEqual(Body->MassKg, 12.0f) &&
				 Body->CollisionPolicy == EWorldObjectPhysicsCollisionPolicy::Standard);
	FWorldObjectRuntimeStats Stats = Harness.Subsystem->GetRuntimeStats();
	TestEqual(TEXT("Physics Proxy 进入 Actor Active Array"), Stats.ActorActiveCount, 1);
	TestEqual(TEXT("Physics 生命周期只绑定一个临时 Proxy"), Stats.BoundProxyCount, 1);
	UWorldObjectProxyComponent* Proxy = Harness.Subsystem->GetProxy(Entity);
	AWorldObjectPhysicsProxyActor* ProxyActor =
		Proxy ? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
	TestNotNull(TEXT("自动 Proxy 使用专用 Physics Actor"), ProxyActor);
	UBoxComponent* PhysicsBox = ProxyActor ? ProxyActor->GetPhysicsBox() : nullptr;
	TestTrue(TEXT("不可见 Box 已完整配置，但在复制预留窗口内尚未释放 Chaos"),
			 PhysicsBox && ProxyActor && !ProxyActor->IsPhysicsReleased() && !PhysicsBox->IsSimulatingPhysics() &&
				 PhysicsBox->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics &&
				 ProxyActor->GetConfiguredCollisionPolicy() == EWorldObjectPhysicsCollisionPolicy::Standard);
	ACharacter* WalkabilityProbe = Harness.World->SpawnActor<ACharacter>();
	FHitResult HorizontalTopHit;
	HorizontalTopHit.bBlockingHit = true;
	HorizontalTopHit.Time = 0.5f;
	HorizontalTopHit.Component = PhysicsBox;
	HorizontalTopHit.Normal = FVector::UpVector;
	HorizontalTopHit.ImpactNormal = FVector::UpVector;
	TestTrue(TEXT("低矮物理 WorldObject 允许角色 Step Up"), PhysicsBox && PhysicsBox->CanCharacterStepUpOn == ECB_Yes);
	TestTrue(TEXT("物理 WorldObject 的水平顶面可作为正常落脚面"),
			 PhysicsBox && PhysicsBox->GetWalkableSlopeOverride().GetWalkableSlopeBehavior() == WalkableSlope_Default &&
				 WalkabilityProbe && WalkabilityProbe->GetCharacterMovement() &&
				 WalkabilityProbe->GetCharacterMovement()->IsWalkable(HorizontalTopHit));
	if (WalkabilityProbe)
	{
		WalkabilityProbe->Destroy();
	}
	TestTrue(TEXT("Box Bounds 与实例 Bounds 一致"),
			 PhysicsBox && PhysicsBox->GetUnscaledBoxExtent().Equals(FVector(12.0, 8.0, 20.0)));
	TestTrue(TEXT("Chaos Body 配置使用 ECS 分配质量"),
			 ProxyActor && FMath::IsNearlyEqual(ProxyActor->GetConfiguredMassKg(), 12.0f, 0.01f));
	TestTrue(TEXT("专用 Proxy 启用 Actor Movement Replication"), ProxyActor && ProxyActor->IsReplicatingMovement());

	const FWorldEntityId WorldEntityId = Harness.Subsystem->GetWorldEntityId(Entity);
	Harness.Subsystem->QueueProxyMotionState(WorldEntityId, EWorldObjectMotionState::Dormant);
	AdvancePostActorFrame(*Harness.World);
	Stats = Harness.Subsystem->GetRuntimeStats();
	TestEqual(TEXT("Sleep 从 Actor Active Array O(1) 移除"), Stats.ActorActiveCount, 0);
	TestTrue(TEXT("Sleep 保留 Entity 并回收自动 Proxy 与 Chaos Body"),
				 Harness.Subsystem->IsEntityAlive(Entity) && Harness.Subsystem->GetProxy(Entity) == nullptr &&
				 (!IsValid(ProxyActor) || ProxyActor->IsActorBeingDestroyed()));
	TestEqual(TEXT("Dormant 自动物理对象不保留绑定 Proxy"), Stats.BoundProxyCount, 0);
	FWorldObjectSpatialQueryScratch Scratch;
	TArray<FWorldObjectEntityHandle> Hits;
	Harness.Subsystem->QueryPortableOverlap(FBox(FVector(-20.0, -20.0, 70.0), FVector(20.0, 20.0, 120.0)), Scratch,
											Hits);
	TestTrue(TEXT("Dormant Physics 仍保留 Portable Dynamic Tree 叶"), Hits.Contains(Entity));
	TestTrue(TEXT("显式激活按同一 Entity 重建自动 Physics Proxy"),
			 Harness.Subsystem->ActivatePhysics(Entity, FVector(100.0, 0.0, 50.0)));
	Stats = Harness.Subsystem->GetRuntimeStats();
	UWorldObjectProxyComponent* ReactivatedProxy = Harness.Subsystem->GetProxy(Entity);
	AWorldObjectPhysicsProxyActor* ReactivatedActor =
		ReactivatedProxy ? Cast<AWorldObjectPhysicsProxyActor>(ReactivatedProxy->GetOwner()) : nullptr;
	TestEqual(TEXT("重新激活进入 Actor Active Array"), Stats.ActorActiveCount, 1);
	TestEqual(TEXT("重新激活只创建一个绑定 Proxy"), Stats.BoundProxyCount, 1);
	TestTrue(TEXT("Dormant 重建 Proxy 恢复碰撞与 Movement Replication，并立即释放 Physics"),
				 ReactivatedActor && ReactivatedActor != ProxyActor && ReactivatedActor->IsReplicatingMovement() &&
					 ReactivatedActor->IsPhysicsReleased() && ReactivatedActor->GetPhysicsBox()->IsSimulatingPhysics() &&
					 ReactivatedActor->GetPhysicsBox()->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics);
	TestTrue(TEXT("降级/重建保持同一 Handle 和 WorldEntityId"),
				 Harness.Subsystem->FindEntity(WorldEntityId) == Entity);
	TestTrue(TEXT("销毁 Entity 同步销毁自动 Proxy Actor"), Harness.Subsystem->DestroyEntity(Entity));
	TestTrue(TEXT("重建 Proxy Actor 已进入销毁流程"),
			 ReactivatedActor && (!IsValid(ReactivatedActor) || ReactivatedActor->IsActorBeingDestroyed()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectPhysicsDemotionScaleTest,
	"ElementSandbox.WorldObjects.Physics.DormantProxyScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPhysicsDemotionScaleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	FTestWorld Harness(TEXT("WorldObjectPhysicsDemotionScale"));
	UWorldObjectTestPhysicsDefinition* Definition = NewObject<UWorldObjectTestPhysicsDefinition>(Harness.World);
	Definition->DefinitionId = TEXT("Test.PhysicsDemotionScale");
	constexpr int32 EntityCount = 256;
	TArray<FWorldObjectEntityHandle> Entities;
	Entities.Reserve(EntityCount);
	for (int32 Index = 0; Index < EntityCount; ++Index)
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Definition;
		Desc.WorldTransform.SetLocation(FVector(Index * 50.0, 0.0, 100.0));
		Desc.MotionState = EWorldObjectMotionState::Physics;
		Desc.InstanceInteractionBounds = Definition->InteractionLocalBounds;
		Desc.PhysicsBody = FWorldObjectPhysicsBodyInit{};
		const FWorldObjectEntityHandle Entity = Harness.Subsystem->CreateEntity(Desc);
		if (!Entity.IsSet())
		{
			AddError(FString::Printf(TEXT("第 %d 个规模样本创建失败"), Index));
			return false;
		}
		Entities.Add(Entity);
		Harness.Subsystem->QueueProxyMotionState(
			Harness.Subsystem->GetWorldEntityId(Entity), EWorldObjectMotionState::Dormant);
	}
	TestEqual(TEXT("激活阶段每个样本只有一个临时 Proxy"),
		Harness.Subsystem->GetRuntimeStats().BoundProxyCount, EntityCount);
	AdvancePostActorFrame(*Harness.World);
	const FWorldObjectRuntimeStats Stats = Harness.Subsystem->GetRuntimeStats();
	TestEqual(TEXT("批量 Sleep 后全部 Entity 仍在"), Stats.EntityCount, EntityCount);
	TestEqual(TEXT("批量 Sleep 后 Active Array 清空"), Stats.ActorActiveCount, 0);
	TestEqual(TEXT("批量 Sleep 后不残留任何自动 Physics Proxy"), Stats.BoundProxyCount, 0);
	for (const FWorldObjectEntityHandle Entity : Entities)
	{
		if (!Harness.Subsystem->IsEntityAlive(Entity) || Harness.Subsystem->GetProxy(Entity) != nullptr)
		{
			AddError(TEXT("Dormant 规模样本未保持纯 ECS 状态。"));
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectLooseDebrisPersistenceTest,
								 "ElementSandbox.WorldObjects.Physics.LooseDebrisCollisionAndPersistence",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectLooseDebrisPersistenceTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	TArray<FWorldPersistentEntityRecord> Records;
	FWorldEntityId EntityId;
	FString Error;
	{
		FTestWorld Authority(TEXT("WorldObjectLooseDebrisAuthority"));
		UWorldObjectTestPhysicsDefinition* Definition = NewObject<UWorldObjectTestPhysicsDefinition>(Authority.World);
		Definition->DefinitionId = TEXT("Test.LooseDebrisPersistence");
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Definition;
		Desc.WorldTransform.SetLocation(FVector(0.0, 0.0, 120.0));
		Desc.MotionState = EWorldObjectMotionState::Physics;
		Desc.InstanceInteractionBounds = Definition->InteractionLocalBounds;
		FWorldObjectPhysicsBodyInit Init;
		Init.MassKg = 4.0f;
		Init.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
		Desc.PhysicsBody = Init;
		const FWorldObjectEntityHandle Entity = Authority.Subsystem->CreateEntity(Desc);
		EntityId = Authority.Subsystem->GetWorldEntityId(Entity);
		TestTrue(TEXT("LooseDebris Authority Entity 创建"), Entity.IsSet() && EntityId.IsSet());
		TestTrue(TEXT("生产 WorldObject Adapter 捕获 LooseDebris Physics Payload"),
				 Authority.Subsystem->CapturePersistentBatchForTesting(MakeArrayView(&EntityId, 1), Records, Error));
		TestEqual(TEXT("捕获一条 LooseDebris 记录"), Records.Num(), 1);
	}
	if (Records.Num() != 1)
	{
		AddError(Error);
		return false;
	}

	FTestWorld Restored(TEXT("WorldObjectLooseDebrisRestored"));
	UWorldObjectTestPhysicsDefinition* RestoredDefinition =
		NewObject<UWorldObjectTestPhysicsDefinition>(Restored.World);
	RestoredDefinition->DefinitionId = TEXT("Test.LooseDebrisPersistence");
	TestTrue(TEXT("Restore World 注册同一 Definition"), Restored.Subsystem->RegisterDefinition(*RestoredDefinition));
	const FWorldChunkCoord HomeChunk = FWorldChunkCoord::FromWorldLocation(Records[0].WorldTransform.GetLocation());
	TestTrue(TEXT("LooseDebris Payload 通过生产 Restore 路径恢复"),
			 Restored.Subsystem->RestorePersistentBatchForTesting(HomeChunk, Records, Error));
	const FWorldObjectEntityHandle RestoredEntity = Restored.Subsystem->FindEntity(EntityId);
	const FWorldObjectPhysicsBodyFragment* Body =
		Restored.Subsystem->GetRegistry().FindFragment<FWorldObjectPhysicsBodyFragment>(RestoredEntity);
	UWorldObjectProxyComponent* Proxy = Restored.Subsystem->GetProxy(RestoredEntity);
	AWorldObjectPhysicsProxyActor* Actor = Proxy ? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
	UBoxComponent* Box = Actor ? Actor->GetPhysicsBox() : nullptr;
	TestTrue(TEXT("CollisionPolicy 经 Chunk Payload、ECS Fragment 与复制配置完整往返"),
			 Body && Actor && Body->CollisionPolicy == EWorldObjectPhysicsCollisionPolicy::LooseDebris &&
				 Actor->GetConfiguredCollisionPolicy() == EWorldObjectPhysicsCollisionPolicy::LooseDebris);
		TestTrue(TEXT("LooseDebris 不可 StepUp、不可作为移动地面，并阻挡角色、环境和其他动态残骸"),
					 Box && Box->CanCharacterStepUpOn == ECB_No
						 && Box->GetWalkableSlopeOverride().GetWalkableSlopeBehavior() == WalkableSlope_Unwalkable
						 && Box->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block &&
					 Box->GetCollisionResponseToChannel(ECC_PhysicsBody) == ECR_Block &&
					 Box->GetCollisionResponseToChannel(ECC_WorldStatic) == ECR_Block &&
					 Box->GetCollisionResponseToChannel(ECC_WorldDynamic) == ECR_Block);
	TestTrue(TEXT("LooseDebris 保留完整碰撞盒并限制初始解穿透速度"),
		Box
			&& Box->GetUnscaledBoxExtent().Equals(
				Actor->GetConfiguredLocalExtent(),
				UE_KINDA_SMALL_NUMBER)
			&& Box->BodyInstance.GetOverrideMaxDepenetrationVelocity()
			&& FMath::IsNearlyEqual(Box->BodyInstance.GetMaxDepenetrationVelocity(), 60.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectPhysicsEnvironmentTest,
								 "ElementSandbox.WorldObjects.Physics.ChaosEnvironmentAndSleep",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPhysicsEnvironmentTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	const UWorld::InitializationValues Values = UWorld::InitializationValues()
													.CreatePhysicsScene(true)
													.ShouldSimulatePhysics(true)
													.EnableTraceCollision(true)
													.CreateNavigation(false)
													.CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("WorldObjectPhysicsEnvironment"), nullptr, true,
										ERHIFeatureLevel::Num, &Values, true);
	TestNotNull(TEXT("环境测试 World 创建"), World);
	if (!World)
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitWorld(Values);
	World->UpdateWorldComponents(true, false);
	UWorldObjectWorldSubsystem* Subsystem = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	UWorldStorageSubsystem* Storage = World->GetSubsystem<UWorldStorageSubsystem>();
	const FWorldResidencySourceHandle ResidencySource =
		Storage ? Storage->RegisterResidencySource(FVector::ZeroVector) : FWorldResidencySourceHandle();
	AActor* Floor = World->SpawnActor<AActor>();
	UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor, TEXT("PhysicsFloor"));
	Floor->AddInstanceComponent(FloorBox);
	Floor->SetRootComponent(FloorBox);
	FloorBox->SetBoxExtent(FVector(500.0, 500.0, 5.0));
	FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	FloorBox->SetCollisionObjectType(ECC_WorldStatic);
	FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
	FloorBox->SetCastShadow(false);
	FloorBox->RegisterComponent();
	Floor->SetActorLocation(FVector(0.0, 0.0, -5.0));

	UWorldObjectTestPhysicsDefinition* Definition = NewObject<UWorldObjectTestPhysicsDefinition>(World);
	Definition->DefinitionId = TEXT("Test.PhysicsEnvironment");
	FWorldObjectCreateDesc Desc;
	Desc.Definition = Definition;
	Desc.WorldTransform.SetLocation(FVector(0.0, 0.0, 120.0));
	Desc.MotionState = EWorldObjectMotionState::Physics;
	Desc.InstanceInteractionBounds = Definition->InteractionLocalBounds;
	FWorldObjectPhysicsBodyInit Init;
	Init.MassKg = 10.0f;
	Init.LinearVelocity = FVector(0.0, 0.0, -300.0);
	Desc.PhysicsBody = Init;
	const FWorldObjectEntityHandle Entity = Subsystem->CreateEntity(Desc);
	TestTrue(TEXT("环境物理对象创建"), Entity.IsSet());
	UWorldObjectProxyComponent* InitialProxy = Subsystem->GetProxy(Entity);
	AWorldObjectPhysicsProxyActor* InitialProxyActor =
		InitialProxy ? Cast<AWorldObjectPhysicsProxyActor>(InitialProxy->GetOwner()) : nullptr;
	TestTrue(TEXT("Physics 创建提交后先复制初始位置，不在同帧释放刚体"),
		InitialProxyActor && !InitialProxyActor->IsPhysicsReleased()
			&& !InitialProxyActor->GetPhysicsBox()->IsSimulatingPhysics());
	World->BeginPlay();
	bool bObservedReleasedPhysics = false;
	for (int32 Frame = 0; Frame < 360; ++Frame)
	{
		++GFrameCounter;
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		bObservedReleasedPhysics |= IsValid(InitialProxyActor)
			&& InitialProxyActor->IsPhysicsReleased()
			&& InitialProxyActor->GetPhysicsBox()->IsSimulatingPhysics();
	}
	TestTrue(TEXT("复制预留窗口后 Authority 确实释放并模拟刚体"), bObservedReleasedPhysics);
	const FWorldObjectMotionFragment* Motion =
		Subsystem->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Entity);
	const FWorldObjectTransformFragment* Transform =
		Subsystem->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Entity);
	TestTrue(TEXT("Chaos 环境接触后刚体最终休眠"), Motion && Motion->State == EWorldObjectMotionState::Dormant);
	TestTrue(TEXT("真实 Chaos Sleep 后自动 Proxy 已回收"),
			 Subsystem->GetProxy(Entity) == nullptr &&
			 (!IsValid(InitialProxyActor) || InitialProxyActor->IsActorBeingDestroyed()));
	TestEqual(TEXT("环境休眠后没有残留绑定 Proxy"), Subsystem->GetRuntimeStats().BoundProxyCount, 0);
	TestTrue(TEXT("休眠物理对象停在环境表面上方而非穿透"),
			 Transform && Transform->WorldTransform.GetLocation().Z >= 9.0);
	if (Storage && ResidencySource.IsSet())
	{
		Storage->UnregisterResidencySource(ResidencySource);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectDormantNearFieldCollisionTest,
	"ElementSandbox.WorldObjects.Collision.DormantNearFieldProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectDormantNearFieldCollisionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	(void)Parameters;
	FTestWorld Harness(TEXT("WorldObjectDormantNearFieldCollision"));
	UWorldObjectCollisionWorldSubsystem* Collision =
		Harness.World->GetSubsystem<UWorldObjectCollisionWorldSubsystem>();
	if (!TestTrue(TEXT("WorldObject 与近场碰撞 Subsystem 可用"), Harness.Subsystem && Collision))
	{
		return false;
	}

	UWorldObjectTestPhysicsDefinition* Definition =
		NewObject<UWorldObjectTestPhysicsDefinition>(Harness.World);
	Definition->DefinitionId = TEXT("Test.DormantNearFieldCollision");
	FWorldObjectCreateDesc Desc;
	Desc.Definition = Definition;
	Desc.WorldTransform = FTransform(FVector(150.0, 0.0, 80.0));
	Desc.MotionState = EWorldObjectMotionState::Dormant;
	const FWorldObjectEntityHandle Entity = Harness.Subsystem->CreateEntity(Desc);
	TestTrue(TEXT("创建带 Physics 能力的 Dormant WorldObject"), Entity.IsSet());
	FWorldObjectPhysicsBodyFragment* PhysicsBody =
		Harness.Subsystem->GetRegistry().FindMutableFragment<FWorldObjectPhysicsBodyFragment>(Entity);
	if (PhysicsBody)
	{
		PhysicsBody->MassKg = 4.0f;
		PhysicsBody->CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
	}
	TestNotNull(TEXT("Dormant WorldObject 保留可配置质量与 LooseDebris 策略"), PhysicsBody);

	FWorldObjectCollisionSource Source;
	Source.SubjectLocation = FVector::ZeroVector;
	Source.ViewLocation = FVector::ZeroVector;
	Source.ViewDirection = FVector::ForwardVector;
	Source.PawnContactBounds = FBox(FVector(-50.0), FVector(50.0));
	Source.ImmediateBounds = FBox(FVector(-400.0), FVector(400.0));
	Source.PrefetchBounds = Source.ImmediateBounds;
	Source.RetentionBounds = Source.ImmediateBounds.ExpandBy(300.0);
	Source.Revision = 1;
	const FWorldObjectCollisionSourceHandle SourceHandle = Collision->RegisterSource(Source);
	TestTrue(TEXT("注册角色近场碰撞 Source"), SourceHandle.IsSet());
	Collision->FlushImmediateCollisionChanges();
	TestEqual(TEXT("四米内 Dormant WorldObject 同步建立碰撞实例"),
		Collision->GetStats().CollisionInstanceCount, 1);
	TestEqual(TEXT("Dirty Source 只查询一次 WorldObject 空间索引"),
		Collision->GetStats().SpatialQueryCount, 1ll);

	FHitResult Hit;
	const bool bPawnTraceBlocked = Harness.World->LineTraceSingleByChannel(
		Hit,
		FVector(50.0, 0.0, 80.0),
		FVector(250.0, 0.0, 80.0),
		ECC_Pawn);
	TestTrue(TEXT("近场实例真实阻挡 Pawn Channel"), bPawnTraceBlocked);
	const UPrimitiveComponent* DormantCollisionComponent = Hit.GetComponent();
	TestTrue(TEXT("LooseDebris 近场实例不可 StepUp、不可成为角色移动基底"),
		DormantCollisionComponent
		&& DormantCollisionComponent->CanCharacterStepUpOn == ECB_No
		&& DormantCollisionComponent->GetWalkableSlopeOverride().GetWalkableSlopeBehavior()
			== WalkableSlope_Unwalkable);

	Source.Velocity = FVector(600.0, 0.0, 0.0);
	Source.PawnContactBounds = FBox(FVector(100.0, -50.0, 30.0), FVector(200.0, 50.0, 130.0));
	Source.Revision = 2;
	TestTrue(TEXT("角色接触走廊更新 Source"), Collision->UpdateSource(SourceHandle, Source));
	Collision->FlushImmediateCollisionChanges();
	TestEqual(TEXT("胶囊到达前按接触走廊激活，不等待阻挡回调"),
		Collision->GetStats().CollisionInstanceCount, 0);
	TestEqual(TEXT("Physics Proxy 接管时同步撤掉 Dormant 近场碰撞"),
		Collision->GetStats().CollisionInstanceCount, 0);
	UWorldObjectProxyComponent* ContactProxy = Harness.Subsystem->GetProxy(Entity);
	AWorldObjectPhysicsProxyActor* ContactPhysicsActor = ContactProxy
		? Cast<AWorldObjectPhysicsProxyActor>(ContactProxy->GetOwner()) : nullptr;
	TestTrue(TEXT("接触激活沿用 Fragment 的质量与 LooseDebris 策略"),
		ContactPhysicsActor
		&& FMath::IsNearlyEqual(ContactPhysicsActor->GetConfiguredMassKg(), 4.0f, 0.01f)
		&& ContactPhysicsActor->GetConfiguredCollisionPolicy()
			== EWorldObjectPhysicsCollisionPolicy::LooseDebris);
	TestTrue(TEXT("Dormant 接触唤醒不等待新生成物的复制预留窗口"),
		ContactPhysicsActor && ContactPhysicsActor->IsPhysicsReleased()
		&& ContactPhysicsActor->GetPhysicsBox()->IsSimulatingPhysics());
	TestTrue(TEXT("接触走廊只做零速度预唤醒，不在角色碰到前慢慢推走"),
		ContactPhysicsActor
		&& ContactPhysicsActor->GetPhysicsBox()->GetPhysicsLinearVelocity().IsNearlyZero(1.0));
	TestTrue(TEXT("Physics 状态改回 Dormant"),
		Harness.Subsystem->SetMotionState(Entity, EWorldObjectMotionState::Dormant));
	TestEqual(TEXT("Dormant 封口时在自动 Proxy 回收前恢复近场碰撞"),
		Collision->GetStats().CollisionInstanceCount, 1);
	TestTrue(TEXT("GameplayDestroy 成功"), Harness.Subsystem->DestroyEntity(Entity));
	TestEqual(TEXT("GameplayDestroy 同步移除近场碰撞"),
		Collision->GetStats().CollisionInstanceCount, 0);
	TestTrue(TEXT("注销角色 Source"), Collision->UnregisterSource(SourceHandle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectCollisionGraceDeadlineOrderTest,
	"ElementSandbox.WorldObjects.Collision.GraceExpiresOldestFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectCollisionGraceDeadlineOrderTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::Tests;
	(void)Parameters;
	FTestWorld Harness(TEXT("WorldObjectCollisionGraceDeadlineOrder"));
	auto* Collision = Harness.World->GetSubsystem<UWorldObjectCollisionWorldSubsystem>();
	if (!TestTrue(TEXT("碰撞回收测试环境就绪"), Harness.Subsystem && Collision)) return false;
	auto* Definition = NewObject<UWorldObjectTestPhysicsDefinition>(Harness.World);
	Definition->DefinitionId = TEXT("Test.CollisionGraceDeadlineOrder");
	TArray<FWorldObjectCollisionSourceHandle> Sources;
	TArray<FWorldObjectEntityHandle> Entities;
	for (const double X : {0.0, 1000.0})
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Definition;
		Desc.WorldTransform.SetLocation(FVector(X, 0.0, 80.0));
		Entities.Add(Harness.Subsystem->CreateEntity(Desc));
		FWorldObjectCollisionSource Source;
		Source.SubjectLocation = Desc.WorldTransform.GetLocation();
		Source.ViewLocation = Source.SubjectLocation;
		Source.PawnContactBounds = FBox::BuildAABB(Source.SubjectLocation, FVector(50.0));
		Source.ImmediateBounds = FBox::BuildAABB(Source.SubjectLocation, FVector(300.0));
		Source.PrefetchBounds = Source.RetentionBounds = Source.ImmediateBounds;
		Source.Revision = 1;
		Sources.Add(Collision->RegisterSource(Source));
	}
	Collision->FlushImmediateCollisionChanges();
	if (!TestEqual(TEXT("两个互不覆盖的近场分别建立碰撞"), Collision->GetStats().CollisionInstanceCount, 2)) return false;

	// 故意让队列同时含有已到期项和未来项；未来项不能把较早的碰撞代理卡在队列里。
	TestTrue(TEXT("第一个 Source 离开"), Collision->UnregisterSource(Sources[0]));
	FPlatformProcess::Sleep(0.75f);
	TestTrue(TEXT("第二个 Source 稍后离开"), Collision->UnregisterSource(Sources[1]));
	FPlatformProcess::Sleep(static_cast<float>(Collision->GetActivationConfig().GraceSeconds - 0.5));
	Collision->Tick(0.0f);
	TestEqual(TEXT("只回收已过 Grace 的第一个碰撞实例"), Collision->GetStats().CollisionInstanceCount, 1);
	FHitResult Hit;
	TestFalse(TEXT("已到期位置不再留下隐藏阻挡"), Harness.World->LineTraceSingleByChannel(
		Hit, FVector(-200.0, 0.0, 80.0), FVector(200.0, 0.0, 80.0), ECC_Pawn));
	TestTrue(TEXT("尚在 Grace 内的位置仍保留碰撞"), Harness.World->LineTraceSingleByChannel(
		Hit, FVector(800.0, 0.0, 80.0), FVector(1200.0, 0.0, 80.0), ECC_Pawn));
	FPlatformProcess::Sleep(0.75f);
	Collision->Tick(0.0f);
	TestEqual(TEXT("第二个实例到期后也完成回收"), Collision->GetStats().CollisionInstanceCount, 0);
	TestTrue(TEXT("碰撞回收不销毁 Gameplay 实体"), Harness.Subsystem->IsEntityAlive(Entities[0])
		&& Harness.Subsystem->IsEntityAlive(Entities[1]));
	return true;
}

#endif
