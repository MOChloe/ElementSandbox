#if WITH_DEV_AUTOMATION_TESTS

#include "BuildingWorldSubsystem.h"

#include "Async/TaskGraphInterfaces.h"
#include "Chunk/WorldChunkTypes.h"
#include "Collision/BuildCollisionHost.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInterface.h"
#include "MeshPoolRenderHost.h"
#include "Misc/AutomationTest.h"
#include "PresentationWorldSubsystem.h"
#include "Rendering/BuildPresentationSettings.h"
#include "Rendering/BuildRenderTypes.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Tests/BuildEntityTestTypes.h"

namespace ElementSandbox::Building::Tests
{
	struct FBuildingSubsystemTestWorld
	{
		explicit FBuildingSubsystemTestWorld(
			const EWorldType::Type WorldType = EWorldType::Game,
			const FName WorldName = NAME_None)
		{
			World = UWorld::CreateWorld(
				WorldType,
				false,
				WorldName,
				nullptr,
				true);
			check(World);
			GEngine->CreateNewWorldContext(WorldType).SetCurrentWorld(World);
			Subsystem = World->GetSubsystem<UBuildingWorldSubsystem>();
		}

		~FBuildingSubsystemTestWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Subsystem = nullptr;
	};

	UBuildTestDefinition* MakeSubsystemTestDefinition(
		UObject* Outer,
		UStaticMesh* Mesh)
	{
		check(Mesh);
		UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>(Outer);
		FBuildMeshPartDefinition Part;
		Part.Mesh = Mesh;
		Definition->MeshParts.Add(Part);
		return Definition;
	}

	struct FScopedIncrementalPresentationSettings final
	{
		FScopedIncrementalPresentationSettings()
			: Settings(GetMutableDefault<UBuildPresentationSettings>()),
			  PreviousMinimumLocalRadius(Settings->MinimumLocalRadius),
			  PreviousLocalTarget(Settings->LocalResidentTargetMeshParts),
			  PreviousStableTarget(Settings->StableResidentTargetMeshParts),
			  PreviousTransitionReserve(Settings->TransitionReserveMeshParts),
			  PreviousHardWatermark(Settings->ResidentHardWatermarkMeshParts),
			  PreviousEmergencyOverflow(Settings->EmergencyOverflowMeshParts),
			  PreviousInitialWorkBudget(Settings->InitialMeshPoolWorkBudgetParts),
			  PreviousMinimumWorkBudget(Settings->MinimumMeshPoolWorkBudgetParts),
			  PreviousMaximumWorkBudget(Settings->MaximumMeshPoolWorkBudgetParts),
			  PreviousPublishBudget(Settings->LocalTransitionPublishBudgetEntitiesPerCycle),
			  PreviousHotRadius(Settings->HotPromotionRadius)
		{
			Settings->MinimumLocalRadius = 1000.0;
			Settings->LocalResidentTargetMeshParts = 4;
			Settings->StableResidentTargetMeshParts = 4;
			Settings->TransitionReserveMeshParts = 4;
			Settings->ResidentHardWatermarkMeshParts = 8;
			Settings->EmergencyOverflowMeshParts = 0;
			Settings->InitialMeshPoolWorkBudgetParts = 1;
			Settings->MinimumMeshPoolWorkBudgetParts = 1;
			Settings->MaximumMeshPoolWorkBudgetParts = 1;
			Settings->LocalTransitionPublishBudgetEntitiesPerCycle = 1;
			Settings->HotPromotionRadius = 1000.0;
		}

		~FScopedIncrementalPresentationSettings()
		{
			Settings->MinimumLocalRadius = PreviousMinimumLocalRadius;
			Settings->LocalResidentTargetMeshParts = PreviousLocalTarget;
			Settings->StableResidentTargetMeshParts = PreviousStableTarget;
			Settings->TransitionReserveMeshParts = PreviousTransitionReserve;
			Settings->ResidentHardWatermarkMeshParts = PreviousHardWatermark;
			Settings->EmergencyOverflowMeshParts = PreviousEmergencyOverflow;
			Settings->InitialMeshPoolWorkBudgetParts = PreviousInitialWorkBudget;
			Settings->MinimumMeshPoolWorkBudgetParts = PreviousMinimumWorkBudget;
			Settings->MaximumMeshPoolWorkBudgetParts = PreviousMaximumWorkBudget;
			Settings->LocalTransitionPublishBudgetEntitiesPerCycle = PreviousPublishBudget;
			Settings->HotPromotionRadius = PreviousHotRadius;
		}

		UBuildPresentationSettings* Settings = nullptr;
		double PreviousMinimumLocalRadius = 0.0;
		int32 PreviousLocalTarget = 0;
		int32 PreviousStableTarget = 0;
		int32 PreviousTransitionReserve = 0;
		int32 PreviousHardWatermark = 0;
		int32 PreviousEmergencyOverflow = 0;
		int32 PreviousInitialWorkBudget = 0;
		int32 PreviousMinimumWorkBudget = 0;
		int32 PreviousMaximumWorkBudget = 0;
		int32 PreviousPublishBudget = 0;
		double PreviousHotRadius = 0.0;
	};
}

#if WITH_EDITOR

namespace ElementSandbox::Building::Tests
{
	struct FBuildingClientProjectionWorld final
	{
		FBuildingClientProjectionWorld()
		{
			UWorld::InitializationValues InitializationValues;
			InitializationValues
				.CreatePhysicsScene(true)
				.ShouldSimulatePhysics(false)
				.EnableTraceCollision(true)
				.CreateNavigation(false)
				.CreateAISystem(false);
			World = UWorld::CreateWorld(
				EWorldType::PIE,
				false,
				TEXT("BuildingClientProjection"),
				nullptr,
				true,
				ERHIFeatureLevel::Num,
				&InitializationValues,
				true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(NM_Client);
			World->InitWorld(InitializationValues);
			World->UpdateWorldComponents(true, false);
			Subsystem = World->GetSubsystem<UBuildingWorldSubsystem>();
		}

		~FBuildingClientProjectionWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Subsystem = nullptr;
	};
}

#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingDefinitionRegistryRulesTest,
	"ElementSandbox.Building.Network.DefinitionRegistryRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingDefinitionRegistryRulesTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildingSubsystemTestWorld Harness(EWorldType::Game, TEXT("BuildingDefinitionRegistry"));
	if (!Harness.Subsystem)
	{
		return false;
	}

	UBuildTestDefinition* MissingId = NewObject<UBuildTestDefinition>(Harness.World);
	MissingId->DefinitionId = NAME_None;
	TestFalse(TEXT("缺失 DefinitionId 的配置被拒绝"),
		Harness.Subsystem->RegisterDefinition(*MissingId));
	TestNull(TEXT("未知 DefinitionId 查询为空"),
		Harness.Subsystem->FindDefinition(TEXT("Building.Test.Unknown")));

	UBuildTestDefinition* First = NewObject<UBuildTestDefinition>(Harness.World);
	First->DefinitionId = TEXT("Building.Test.RegistryUnique");
	UBuildTestDefinition* Duplicate = NewObject<UBuildTestDefinition>(Harness.World);
	Duplicate->DefinitionId = First->DefinitionId;
	TestTrue(TEXT("首次注册稳定 Definition"),
		Harness.Subsystem->RegisterDefinition(*First));
	TestTrue(TEXT("同一对象重复注册幂等"),
		Harness.Subsystem->RegisterDefinition(*First));
	TestFalse(TEXT("同 ID 的不同对象被拒绝"),
		Harness.Subsystem->RegisterDefinition(*Duplicate));
	TestEqual(TEXT("ID 始终解析到首次注册对象"),
		Harness.Subsystem->FindDefinition(First->DefinitionId),
		static_cast<UBuildingDefinition*>(First));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingWorldEntityIdLifetimeTest,
	"ElementSandbox.Building.Network.WorldEntityIdLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingWorldEntityIdLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildingSubsystemTestWorld FirstWorld(EWorldType::Game, TEXT("BuildingWorldEntityIdFirst"));
	FBuildingSubsystemTestWorld SecondWorld(EWorldType::Game, TEXT("BuildingWorldEntityIdSecond"));
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!FirstWorld.Subsystem || !SecondWorld.Subsystem || !CubeMesh)
	{
		return false;
	}
	UBuildTestDefinition* FirstDefinition = MakeSubsystemTestDefinition(
		FirstWorld.World, CubeMesh);
	FirstDefinition->DefinitionId = TEXT("Building.Test.WorldEntityId");
	UBuildTestDefinition* SecondDefinition = MakeSubsystemTestDefinition(
		SecondWorld.World, CubeMesh);
	SecondDefinition->DefinitionId = FirstDefinition->DefinitionId;

	const FBuildEntityHandle FirstEntity = FirstWorld.Subsystem->CreateEntity(
		*FirstDefinition, FTransform::Identity);
	const FWorldEntityId FirstId = FirstWorld.Subsystem->GetWorldEntityId(FirstEntity);
	TestTrue(TEXT("第一个 World 分配有效 WorldEntityId"), FirstId.IsSet());
	TestTrue(TEXT("销毁第一个 Entity"), FirstWorld.Subsystem->DestroyEntity(FirstEntity));
	const FBuildEntityHandle Replacement = FirstWorld.Subsystem->CreateEntity(
		*FirstDefinition, FTransform(FVector(200.0, 0.0, 0.0)));
	const FWorldEntityId ReplacementId = FirstWorld.Subsystem->GetWorldEntityId(Replacement);
	TestTrue(TEXT("销毁后新 WorldEntityId 单调增加"),
		ReplacementId.GetValue() > FirstId.GetValue());
	TestTrue(TEXT("销毁后不复用 WorldEntityId"), ReplacementId != FirstId);

	const FBuildEntityHandle OtherWorldEntity = SecondWorld.Subsystem->CreateEntity(
		*SecondDefinition, FTransform::Identity);
	const FWorldEntityId OtherWorldId = SecondWorld.Subsystem->GetWorldEntityId(OtherWorldEntity);
	TestEqual(TEXT("不同 World 的分配器彼此隔离"), OtherWorldId.GetValue(), 1ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingOldPayloadVersionRejectedTest,
	"ElementSandbox.Building.Persistence.OldPayloadVersionRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingOldPayloadVersionRejectedTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildingSubsystemTestWorld Harness(EWorldType::Game, TEXT("BuildingOldPayloadVersion"));
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !CubeMesh)
	{
		return false;
	}

	UBuildTestDefinition* Definition = MakeSubsystemTestDefinition(Harness.World, CubeMesh);
	Definition->DefinitionId = TEXT("Building.Test.OldPayloadVersion");
	const FTransform Transform(FVector(-12500.0, 2500.0, 300.0));
	const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
		*Definition, Transform, EBuildSpatialMobility::Dynamic);
	const FWorldEntityId EntityId = Harness.Subsystem->GetWorldEntityId(Entity);
	if (!TestTrue(TEXT("创建待捕获 Building"), Entity.IsSet() && EntityId.IsSet()))
	{
		return false;
	}

	TArray<FWorldPersistentEntityRecord> Records;
	FString Error;
	TestTrue(TEXT("生产 Adapter 捕获单一新格式 Payload"),
		Harness.Subsystem->CapturePersistentBatchForTesting(
			MakeArrayView(&EntityId, 1), Records, Error));
	if (!TestEqual(TEXT("捕获一条 Building 记录"), Records.Num(), 1))
	{
		return false;
	}
	if (!TestTrue(TEXT("Building Payload 包含 Magic 与 Version 头"),
		Records[0].Payload.Num() >= 6))
	{
		return false;
	}

	const FWorldChunkCoord HomeChunk =
		FWorldChunkCoord::FromWorldLocation(Records[0].WorldTransform.GetLocation());
	TestTrue(TEXT("RuntimeEvict 移除当前运行投影"),
		Harness.Subsystem->RuntimeEvictPersistentBatchForTesting(
			HomeChunk, MakeArrayView(&EntityId, 1), Error));
	TestFalse(TEXT("被 Evict 的 Entity 不再可解析"),
		Harness.Subsystem->FindEntity(EntityId).IsSet());

	TArray<FWorldPersistentEntityRecord> OldVersionRecords = Records;
	// Building Payload 头为 uint32 Magic + uint16 Version；模拟已删除的旧版格式。
	OldVersionRecords[0].Payload[4] = 1;
	OldVersionRecords[0].Payload[5] = 0;
	TestFalse(TEXT("旧 Building Payload 版本直接拒绝"),
		Harness.Subsystem->RestorePersistentBatchForTesting(
			HomeChunk, OldVersionRecords, Error));
	TestFalse(TEXT("版本拒绝不留半恢复 Entity"),
		Harness.Subsystem->FindEntity(EntityId).IsSet());
	TestTrue(TEXT("同一批正式新格式仍可原子恢复"),
		Harness.Subsystem->RestorePersistentBatchForTesting(HomeChunk, Records, Error));
	TestTrue(TEXT("新格式恢复同一持久身份"),
		Harness.Subsystem->FindEntity(EntityId).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingBatchRemovalPreflightAndCommitTest,
	"ElementSandbox.Building.Persistence.BatchRemovalPreflightAndCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingBatchRemovalPreflightAndCommitTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildingSubsystemTestWorld Harness(EWorldType::Game, TEXT("BuildingBatchRemovalPreflight"));
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UBuildTestDefinition* Definition = Harness.Subsystem && CubeMesh
		? MakeSubsystemTestDefinition(Harness.World, CubeMesh) : nullptr;
	if (!TestNotNull(TEXT("创建批量移除测试 Definition"), Definition))
	{
		return false;
	}
	Definition->DefinitionId = TEXT("Building.Test.BatchRemoval");

	constexpr int32 FastBatchSize = 64;
	TArray<FWorldEntityId> EntityIds;
	EntityIds.Reserve(FastBatchSize);
	for (int32 Index = 0; Index < FastBatchSize; ++Index)
	{
		const FVector Location(100.0 + Index * 20.0, 100.0, 100.0);
		const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(*Definition, FTransform(Location));
		const FWorldEntityId EntityId = Harness.Subsystem->GetWorldEntityId(Entity);
		if (!Entity.IsSet() || !EntityId.IsSet())
		{
			AddError(TEXT("无法创建批量移除测试 Entity。"));
			return false;
		}
		EntityIds.Add(EntityId);
	}
	const FWorldChunkCoord HomeChunk = FWorldChunkCoord::FromWorldLocation(FVector(100.0, 100.0, 100.0));
	FWorldChunkCoord WrongHomeChunk = HomeChunk;
	++WrongHomeChunk.X;
	int32 RemovalBatchCount = 0;
	int32 RemovalChangeCount = 0;
	const FDelegateHandle QueryListener = Harness.Subsystem->OnQuerySnapshotBatchCommitted().AddLambda(
		[&RemovalBatchCount, &RemovalChangeCount](const FBuildQuerySnapshotBatchRef Batch)
		{
			++RemovalBatchCount;
			RemovalChangeCount += Batch->Changes.Num();
		});

	FString Error;
	TestFalse(TEXT("错误 HomeChunk 在首个 Runtime 写入前拒绝整批"),
		Harness.Subsystem->RuntimeEvictPersistentBatchForTesting(WrongHomeChunk, EntityIds, Error));
	for (const FWorldEntityId EntityId : EntityIds)
	{
		TestTrue(TEXT("预检失败后全部 Entity 仍存活"), Harness.Subsystem->FindEntity(EntityId).IsSet());
	}
	TestEqual(TEXT("预检失败不发布 Query Snapshot"), RemovalBatchCount, 0);

	TArray<FWorldPersistentEntityRecord> FastBatchRecords;
	Error.Reset();
	if (!TestTrue(TEXT("捕获快速移除后的测试恢复记录"),
			Harness.Subsystem->CapturePersistentBatchForTesting(EntityIds, FastBatchRecords, Error)))
	{
		AddError(Error);
		return false;
	}
	Error.Reset();
	TestTrue(TEXT("无生命周期监听时整批通过预检后一次提交"),
		Harness.Subsystem->RuntimeEvictPersistentBatchForTesting(HomeChunk, EntityIds, Error));
	for (const FWorldEntityId EntityId : EntityIds)
	{
		TestFalse(TEXT("快速批量移除后 Entity 不再可解析"), Harness.Subsystem->FindEntity(EntityId).IsSet());
	}
	TestEqual(TEXT("快速批量移除只发布一个 Query Snapshot Batch"), RemovalBatchCount, 1);
	TestEqual(TEXT("快速批量移除发布全部 Shape 变化"), RemovalChangeCount, FastBatchSize);
	Harness.Subsystem->OnQuerySnapshotBatchCommitted().Remove(QueryListener);
	Error.Reset();
	if (!TestTrue(TEXT("恢复快速移除实体以保持测试 Resident Directory 一致"),
			Harness.Subsystem->RestorePersistentBatchForTesting(HomeChunk, FastBatchRecords, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<FWorldEntityId> ObservedEntityIds;
	constexpr int32 ObservedBatchSize = 4;
	for (int32 Index = 0; Index < ObservedBatchSize; ++Index)
	{
		const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
			*Definition, FTransform(FVector(2000.0 + Index * 20.0, 100.0, 100.0)));
		ObservedEntityIds.Add(Harness.Subsystem->GetWorldEntityId(Entity));
	}
	TArray<FWorldPersistentEntityRecord> ObservedBatchRecords;
	Error.Reset();
	if (!TestTrue(TEXT("捕获监听回退路径的测试恢复记录"),
			Harness.Subsystem->CapturePersistentBatchForTesting(
				ObservedEntityIds, ObservedBatchRecords, Error)))
	{
		AddError(Error);
		return false;
	}
	int32 LocalRemovalCount = 0;
	const FDelegateHandle LocalRemovalListener = Harness.Subsystem->OnEntityLocalRemoved().AddLambda(
		[&LocalRemovalCount](const FBuildEntityHandle) { ++LocalRemovalCount; });
	Error.Reset();
	TestTrue(TEXT("存在生命周期监听时保留可回滚移除路径"),
		Harness.Subsystem->RuntimeEvictPersistentBatchForTesting(HomeChunk, ObservedEntityIds, Error));
	TestEqual(TEXT("回滚兼容路径仍逐实体广播本地移除"), LocalRemovalCount, ObservedBatchSize);
	Harness.Subsystem->OnEntityLocalRemoved().Remove(LocalRemovalListener);
	Error.Reset();
	if (!TestTrue(TEXT("恢复监听回退路径实体以保持测试 Resident Directory 一致"),
			Harness.Subsystem->RestorePersistentBatchForTesting(HomeChunk, ObservedBatchRecords, Error)))
	{
		AddError(Error);
		return false;
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingRenderCellSeparationTest,
	"ElementSandbox.Building.Rendering.StaticRenderCellSeparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingRenderCellSeparationTest::RunTest(const FString& Parameters)
{
	FBuildSpatialIndexConfig SpatialConfig;
	FBuildRenderClusterConfig RenderConfig;
	FIntVector SpatialA;
	FIntVector SpatialB;
	FIntVector RenderA;
	FIntVector RenderB;
	TestEqual(TEXT("Gameplay Spatial Chunk 保持 50m"), SpatialConfig.ChunkSize, 5000.0);
	TestEqual(TEXT("Static baseline Cell 使用 1km"), RenderConfig.StaticCellSize, 100000.0);
	TestTrue(TEXT("解析第一个 Gameplay Chunk"),
		SpatialConfig.TryGetChunkCoordinate(FVector(1000.0, 0.0, 0.0), SpatialA));
	TestTrue(TEXT("解析第二个 Gameplay Chunk"),
		SpatialConfig.TryGetChunkCoordinate(FVector(6000.0, 0.0, 0.0), SpatialB));
	TestTrue(TEXT("解析第一个 Render Cell"),
		RenderConfig.TryGetCellCoordinate(FVector(1000.0, 0.0, 0.0), RenderA));
	TestTrue(TEXT("解析第二个 Render Cell"),
		RenderConfig.TryGetCellCoordinate(FVector(6000.0, 0.0, 0.0), RenderB));
	TestTrue(TEXT("两个位置属于不同 Gameplay Chunk"), SpatialA != SpatialB);
	TestEqual(TEXT("两个位置仍合并进同一个 1km Render Cell"), RenderA, RenderB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingWorldSubsystemInitializesAndFlushesTest,
	"ElementSandbox.Building.WorldSubsystem.InitializesAndFlushes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingWorldSubsystemInitializesAndFlushesTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildingSubsystemTestWorld Harness(EWorldType::Game, TEXT("BuildingSubsystemFlush"));
	TestNotNull(TEXT("Game World 自动创建 Building Subsystem"), Harness.Subsystem);
	if (!Harness.Subsystem)
	{
		return false;
	}

	UPresentationWorldSubsystem* Presentation =
		Harness.World->GetSubsystem<UPresentationWorldSubsystem>();
	TestNotNull(TEXT("非 Dedicated Server 初始化通用 Presentation"), Presentation);
	TestNotNull(TEXT("Game World 初始化时自动创建 Collision Host"),
		Harness.Subsystem->GetCollisionHost());
	TestFalse(TEXT("空 DirtySet 不请求 Tick"), Harness.Subsystem->IsTickable());
	if (!Presentation)
	{
		return false;
	}
	FPresentationViewSource View;
		View.ViewLocation = FVector::ZeroVector;
		View.SubjectLocation = FVector::ZeroVector;
	View.Revision = 1;
	const FPresentationSourceHandle Source = Presentation->RegisterSource(View);
	TestTrue(TEXT("注册测试相机 Source"), Source.IsSet());

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	TestNotNull(TEXT("加载 Subsystem 测试 Mesh"), CubeMesh);
	if (!CubeMesh)
	{
		return false;
	}

	UBuildTestDefinition* Definition = MakeSubsystemTestDefinition(
		Harness.World,
		CubeMesh);
	const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
		*Definition,
		FTransform(FVector(125.0, 0.0, 0.0)));
	TestTrue(TEXT("Subsystem 原子创建测试 Entity"), Entity.IsSet());
	TestEqual(TEXT("创建同时写入 Registry"),
		Harness.Subsystem->GetRegistry().GetEntityCount(), 1);
	TestEqual(TEXT("创建同时注册空间 Bounds"),
		Harness.Subsystem->GetSpatialIndex().GetEntityCount(), 1);
	TestFalse(TEXT("表现提交不依赖 Tickable Subsystem"), Harness.Subsystem->IsTickable());

	TestTrue(TEXT("显式同步场景可以手动 Flush"),
		Harness.Subsystem->FlushRenderChanges());
	TestEqual(TEXT("MeshPool 收到一个驻留 Instance"),
		Harness.Subsystem->GetRenderedInstanceCount(), 1);

	FBuildPartTransformFragment PartTransforms;
	PartTransforms.LocalTransforms.Add(FTransform(FVector(75.0, 0.0, 0.0)));
	TestTrue(TEXT("为动态表现添加每实例 Part Transform"),
		Harness.Subsystem->GetRegistry().AddFragment(Entity, PartTransforms));
	const int32 ChangedPartIds[] = {0};
	TestTrue(TEXT("提交 Part Transform 同时刷新空间与表现派生层"),
		Harness.Subsystem->CommitPartTransformChange(Entity, ChangedPartIds));
	FBox UpdatedBounds(ForceInit);
	TestTrue(TEXT("提交后空间索引仍可读取 Bounds"),
		Harness.Subsystem->GetSpatialIndex().TryGetBounds(Entity, UpdatedBounds));
	TestTrue(TEXT("空间 Bounds 跟随 Part 局部移动"),
		UpdatedBounds.GetCenter().Equals(FVector(200.0, 0.0, 0.0)));
	Harness.Subsystem->FlushRenderChanges();
	TestEqual(TEXT("Part Transform 表现批次已消费"),
		Harness.Subsystem->GetRenderedInstanceCount(), 1);

	TestTrue(TEXT("Subsystem 原子销毁 Entity"), Harness.Subsystem->DestroyEntity(Entity));
	TestEqual(TEXT("销毁同时清理 Registry"),
		Harness.Subsystem->GetRegistry().GetEntityCount(), 0);
	TestEqual(TEXT("销毁同时清理空间索引"),
		Harness.Subsystem->GetSpatialIndex().GetEntityCount(), 0);
	const uint64 CyclesBeforeIdleDestroy =
		Presentation->GetMeshPoolStats().ScheduledCycleCount;
	TestTrue(TEXT("静止观察源下销毁 Dirty 会主动唤醒 Presentation"),
		Presentation->IsTickable());
	Presentation->Tick(1.0f);
	TestTrue(TEXT("无需移动观察源也执行删除投影周期"),
		Presentation->GetMeshPoolStats().ScheduledCycleCount > CyclesBeforeIdleDestroy);
	TestEqual(TEXT("销毁脏批次自动移除 Building Render Instance"),
		Harness.Subsystem->GetRenderedInstanceCount(), 0);
	TestEqual(TEXT("销毁脏批次自动移除 MeshPool Resident Instance"),
		Presentation->GetMeshPoolStats().ResidentInstanceCount, 0);
	Presentation->UnregisterSource(Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingNetworkRestoreDrainsProjectionWithoutViewMovementTest,
	"ElementSandbox.Building.Network.RestoreDrainsProjectionWithoutViewMovement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingNetworkRestoreDrainsProjectionWithoutViewMovementTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FScopedIncrementalPresentationSettings SettingsOverride;
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Game/Building/Meshes/SM_BuildingCube_CPU.SM_BuildingCube_CPU"));
	UMaterialInterface* BurnableMaterial = LoadObject<UMaterialInterface>(
		nullptr,
		TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
	if (!TestNotNull(TEXT("加载真实木建筑 Mesh"), CubeMesh)
		|| !TestNotNull(TEXT("加载真实木建筑材质"), BurnableMaterial))
	{
		return false;
	}

	TArray<FWorldPersistentEntityRecord> Records;
	FWorldChunkCoord HomeChunk;
	{
		FBuildingSubsystemTestWorld Authority(EWorldType::Game, TEXT("BuildingNetworkRestoreAuthority"));
		if (!TestNotNull(TEXT("创建权威 Building Subsystem"), Authority.Subsystem))
		{
			return false;
		}
		UBuildTestDefinition* Definition = MakeSubsystemTestDefinition(Authority.World, CubeMesh);
		Definition->MeshParts[0].MaterialOverride = BurnableMaterial;
		Definition->MeshParts[0].CustomDataFloatCount = 1;
		TArray<FWorldEntityId> EntityIds;
		for (int32 Index = 0; Index < 5; ++Index)
		{
			const FBuildEntityHandle Entity = Authority.Subsystem->CreateEntity(
				*Definition, FTransform(FVector(100.0 + Index * 100.0, 0.0, -4.0)));
			const FWorldEntityId EntityId = Authority.Subsystem->GetWorldEntityId(Entity);
			TestTrue(TEXT("权威端创建待同步 Building"), Entity.IsSet() && EntityId.IsSet());
			if (!Entity.IsSet() || !EntityId.IsSet())
			{
				return false;
			}
			EntityIds.Add(EntityId);
		}
		FString Error;
		if (!TestTrue(TEXT("捕获网络 Snapshot 使用的 Building 记录"),
				Authority.Subsystem->CapturePersistentBatchForTesting(EntityIds, Records, Error)))
		{
			AddError(Error);
			return false;
		}
		HomeChunk = FWorldChunkCoord::FromWorldLocation(Records[0].WorldTransform.GetLocation());
		TestEqual(TEXT("地面负高度按数学向下取整进入 Z=-1 Chunk"), HomeChunk.Z, -1);
	}

	FBuildingClientProjectionWorld Client;
	UPresentationWorldSubsystem* Presentation = Client.World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!TestNotNull(TEXT("Client 创建 Building Subsystem"), Client.Subsystem) ||
		!TestNotNull(TEXT("Client 创建 Presentation Subsystem"), Presentation))
	{
		return false;
	}
	UBuildTestDefinition* ClientDefinition = MakeSubsystemTestDefinition(Client.World, CubeMesh);
	ClientDefinition->MeshParts[0].MaterialOverride = BurnableMaterial;
	ClientDefinition->MeshParts[0].CustomDataFloatCount = 1;
	if (!TestTrue(TEXT("Client 注册网络记录引用的 Definition"),
			Client.Subsystem->RegisterDefinition(*ClientDefinition)))
	{
		return false;
	}
	int32 RestoreQueryBatchCount = 0;
	int32 RestoreQueryChangeCount = 0;
	const FDelegateHandle RestoreQueryListener = Client.Subsystem->OnQuerySnapshotBatchCommitted().AddLambda(
		[&RestoreQueryBatchCount, &RestoreQueryChangeCount](const FBuildQuerySnapshotBatchRef Batch)
		{
			++RestoreQueryBatchCount;
			RestoreQueryChangeCount += Batch->Changes.Num();
		});

	FPresentationViewSource View;
	View.ViewLocation = FVector::ZeroVector;
	View.SubjectLocation = FVector::ZeroVector;
	View.Forward = FVector::ForwardVector;
	View.Revision = 1;
	const FPresentationSourceHandle Source = Presentation->RegisterSource(View);
	if (!TestTrue(TEXT("Client 注册静止观察源"), Source.IsSet()))
	{
		return false;
	}

	FString RestoreError;
	const TConstArrayView<FWorldPersistentEntityRecord> InitialRecords(Records.GetData(), 4);
	if (!TestTrue(TEXT("Client 原子恢复初始网络 Building 记录"),
			Client.Subsystem->RestorePersistentBatchForTesting(HomeChunk, InitialRecords, RestoreError)))
	{
		AddError(RestoreError);
		return false;
	}
	TestEqual(TEXT("恢复后四栋 Building 已进入 Client ECS"), Client.Subsystem->GetRegistry().GetEntityCount(), 4);
	TestEqual(TEXT("Cold Restore 只提交一个 Query Snapshot Batch"), RestoreQueryBatchCount, 1);
	TestEqual(TEXT("Cold Restore 每个 Shape 只发布一次 Upsert"), RestoreQueryChangeCount, 4);
	Client.Subsystem->OnQuerySnapshotBatchCommitted().Remove(RestoreQueryListener);
	TestEqual(TEXT("投影周期执行前没有提前生成实例"), Client.Subsystem->GetRenderedInstanceCount(), 0);

	int32 ProjectionCycles = 0;
	for (; ProjectionCycles < 512 && Client.Subsystem->GetRenderedInstanceCount() < 4; ++ProjectionCycles)
	{
		Presentation->Tick(1.0f);
		FPlatformProcess::SleepNoStats(0.001f);
	}
	TestEqual(TEXT("观察源不移动时异步选择和分批发布仍会自行排空"),
		Client.Subsystem->GetRenderedInstanceCount(), 4);
	TestTrue(TEXT("低预算配置确实跨越多个客户端投影周期"), ProjectionCycles > 1);
	TestEqual(TEXT("初始四栋 Building 已物理进入 MeshPool"),
		Presentation->GetMeshPoolStats().ResidentInstanceCount, 4);

	AMeshPoolRenderHost* RenderHost = Presentation->GetRenderHost();
	if (!TestNotNull(TEXT("初始投影创建 MeshPool Render Host"), RenderHost))
	{
		Presentation->UnregisterSource(Source);
		return false;
	}
	TInlineComponentArray<UHierarchicalInstancedStaticMeshComponent*> HierarchicalComponents(RenderHost);
	if (!TestEqual(TEXT("同 Mesh、同 1km Cell 合并为一个 HISM"), HierarchicalComponents.Num(), 1))
	{
		Presentation->UnregisterSource(Source);
		return false;
	}
	UHierarchicalInstancedStaticMeshComponent* Hierarchical = HierarchicalComponents[0];
	TestEqual(TEXT("初始 HISM CPU 实例数"), Hierarchical->GetInstanceCount(), 4);
	for (int32 InitialTreeCycles = 0;
		 InitialTreeCycles < 1024
		 && (Hierarchical->GetNumRenderInstances() != 4 || !Hierarchical->IsTreeFullyBuilt());
		 ++InitialTreeCycles)
	{
		Presentation->Tick(1.0f);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.001f);
		FPlatformProcess::SleepNoStats(0.001f);
	}
	TestEqual(TEXT("初始 HISM 已发布渲染实例数"), Hierarchical->GetNumRenderInstances(), 4);
	TestTrue(TEXT("初始 HISM Cluster Tree 已完整发布"), Hierarchical->IsTreeFullyBuilt());

	const uint64 TreeBuildsBeforeLiveInsert =
		Presentation->GetMeshPoolStats().HierarchicalTreeBuildRequests;
	const TConstArrayView<FWorldPersistentEntityRecord> LiveRecord(Records.GetData() + 4, 1);
	if (!TestTrue(TEXT("BeginPlay 后同 Chunk Live Upsert 成功恢复"),
			Client.Subsystem->RestorePersistentBatchForTesting(HomeChunk, LiveRecord, RestoreError)))
	{
		AddError(RestoreError);
		Presentation->UnregisterSource(Source);
		return false;
	}
	TestEqual(TEXT("Live Upsert 后第五栋 Building 已进入 Client ECS"),
		Client.Subsystem->GetRegistry().GetEntityCount(), 5);

	int32 LiveProjectionCycles = 0;
	for (; LiveProjectionCycles < 1024; ++LiveProjectionCycles)
	{
		Presentation->Tick(1.0f);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.001f);
		if (Client.Subsystem->GetRenderedInstanceCount() == 5
			&& Presentation->GetMeshPoolStats().ResidentInstanceCount == 5
			&& Hierarchical->GetInstanceCount() == 5
			&& Hierarchical->GetNumRenderInstances() == 5
			&& Hierarchical->IsTreeFullyBuilt())
		{
			break;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	TestEqual(TEXT("Live Upsert 进入 Building Render Processor"),
		Client.Subsystem->GetRenderedInstanceCount(), 5);
	TestEqual(TEXT("Live Upsert 已物理提交到 MeshPool"),
		Presentation->GetMeshPoolStats().ResidentInstanceCount, 5);
	TestEqual(TEXT("Live Upsert 已追加到 HISM CPU 数据"), Hierarchical->GetInstanceCount(), 5);
	TestEqual(TEXT("Live Upsert 已发布到 HISM 渲染实例"), Hierarchical->GetNumRenderInstances(), 5);
	TestTrue(TEXT("Live Upsert 后 HISM Cluster Tree 覆盖第五个实例"), Hierarchical->IsTreeFullyBuilt());
	TestTrue(TEXT("Live Upsert 触发新的 HISM Tree 发布"),
		Presentation->GetMeshPoolStats().HierarchicalTreeBuildRequests > TreeBuildsBeforeLiveInsert);
	Presentation->UnregisterSource(Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingWorldSubsystemIsolatesWorldStateTest,
	"ElementSandbox.Building.WorldSubsystem.IsolatesWorldState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingWorldSubsystemIsolatesWorldStateTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildingSubsystemTestWorld First(EWorldType::Game, TEXT("BuildingSubsystemFirst"));
	FBuildingSubsystemTestWorld Second(EWorldType::Game, TEXT("BuildingSubsystemSecond"));
	TestNotNull(TEXT("第一个 World 有 Building Subsystem"), First.Subsystem);
	TestNotNull(TEXT("第二个 World 有 Building Subsystem"), Second.Subsystem);
	if (!First.Subsystem || !Second.Subsystem)
	{
		return false;
	}

	TestNotEqual(TEXT("两个 World 不共享 Subsystem"), First.Subsystem, Second.Subsystem);
	TestNotEqual(TEXT("两个 World 不共享 Registry"),
		&First.Subsystem->GetRegistry(),
		&Second.Subsystem->GetRegistry());
	TestNotEqual(TEXT("两个 World 不共享 Spatial Index"),
		&First.Subsystem->GetSpatialIndex(),
		&Second.Subsystem->GetSpatialIndex());
	TestNotEqual(TEXT("两个 World 不共享 Presentation Subsystem"),
		First.World->GetSubsystem<UPresentationWorldSubsystem>(),
		Second.World->GetSubsystem<UPresentationWorldSubsystem>());

	First.Subsystem->GetRegistry().CreateEntity();
	TestEqual(TEXT("第一个 World 有独立 Entity"),
		First.Subsystem->GetRegistry().GetEntityCount(), 1);
	TestEqual(TEXT("第二个 World 仍为空"),
		Second.Subsystem->GetRegistry().GetEntityCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingWorldSubsystemPreDestroyCanVetoTest,
	"ElementSandbox.Building.WorldSubsystem.PreDestroyCanVeto",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingWorldSubsystemPreDestroyCanVetoTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildingSubsystemTestWorld Harness(EWorldType::Game, TEXT("BuildingPreDestroyVeto"));
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	UBuildTestDefinition* Definition = Harness.Subsystem && CubeMesh
		? MakeSubsystemTestDefinition(Harness.World, CubeMesh)
		: nullptr;
	TestNotNull(TEXT("创建 Pre-Destroy 测试 Definition"), Definition);
	if (!Harness.Subsystem || !Definition)
	{
		return false;
	}

	const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
		*Definition,
		FTransform::Identity);
		TestTrue(TEXT("创建待销毁 Building Entity"), Entity.IsSet());
		const FWorldEntityId EntityWorldEntityId = Harness.Subsystem->GetWorldEntityId(Entity);
		bool bObservedExpectedEntity = false;
		int32 DestroyedNotificationCount = 0;
		FWorldEntityId DestroyedWorldEntityId;
		const FDelegateHandle Listener = Harness.Subsystem->OnEntityPreDestroy().AddLambda(
			[&bObservedExpectedEntity, Entity](
				const FBuildEntityHandle Candidate,
				bool& bCanDestroy)
			{
				bObservedExpectedEntity = Candidate == Entity;
				bCanDestroy = false;
			});
		const FDelegateHandle DestroyedListener =
			Harness.Subsystem->OnEntityDestroyed().AddLambda(
				[&DestroyedNotificationCount, &DestroyedWorldEntityId](const FWorldEntityId WorldEntityId)
				{
					++DestroyedNotificationCount;
					DestroyedWorldEntityId = WorldEntityId;
				});

	TestFalse(TEXT("Pre-Destroy 监听方可以否决销毁"),
		Harness.Subsystem->DestroyEntity(Entity));
	TestTrue(TEXT("监听方收到完整 Generation Handle"), bObservedExpectedEntity);
	TestTrue(TEXT("否决后 Registry Entity 仍存活"),
		Harness.Subsystem->GetRegistry().IsAlive(Entity));
		TestTrue(TEXT("否决后空间记录仍存在"),
			Harness.Subsystem->GetSpatialIndex().Contains(Entity));
		TestEqual(TEXT("否决销毁不广播完成通知"), DestroyedNotificationCount, 0);

	Harness.Subsystem->OnEntityPreDestroy().Remove(Listener);
		TestTrue(TEXT("移除监听方后可正常销毁"),
			Harness.Subsystem->DestroyEntity(Entity));
		TestEqual(TEXT("成功销毁只广播一次完成通知"), DestroyedNotificationCount, 1);
		TestEqual(TEXT("完成通知携带销毁前稳定 WorldEntityId"), DestroyedWorldEntityId, EntityWorldEntityId);
		Harness.Subsystem->OnEntityDestroyed().Remove(DestroyedListener);
		return true;
}

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingCollisionHundredThousandFarEntitiesStayUnprojectedTest,
	"ElementSandbox.Building.Collision.HundredThousandFarEntitiesStayUnprojected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingCollisionHundredThousandFarEntitiesStayUnprojectedTest::RunTest(
	const FString& Parameters)
{
	UWorld::InitializationValues InitializationValues;
	InitializationValues
		.CreatePhysicsScene(true)
		.ShouldSimulatePhysics(false)
		.EnableTraceCollision(true)
		.CreateNavigation(false)
		.CreateAISystem(false);

	UWorld* World = UWorld::CreateWorld(
		EWorldType::PIE,
		false,
		TEXT("BuildingCollision100kNoSource"),
		nullptr,
		true,
		ERHIFeatureLevel::Num,
		&InitializationValues,
		true);
	if (!World)
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
	World->SetPlayInEditorInitialNetMode(NM_DedicatedServer);
	World->InitWorld(InitializationValues);
	World->UpdateWorldComponents(true, false);

	UBuildingWorldSubsystem* Subsystem =
		World->GetSubsystem<UBuildingWorldSubsystem>();
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	UBuildTestDefinition* Definition = Subsystem
		? NewObject<UBuildTestDefinition>(Subsystem)
		: nullptr;
	if (Definition && CubeMesh)
	{
		FBuildCollisionPartDefinition CollisionPart;
		CollisionPart.CollisionMesh = CubeMesh;
		Definition->CollisionParts.Add(CollisionPart);
	}

	bool bCreatedAll = Subsystem && Definition && CubeMesh;
	constexpr int32 EntityCount = 100000;
	for (int32 EntityIndex = 0; bCreatedAll && EntityIndex < EntityCount; ++EntityIndex)
	{
		const int32 ChunkX = EntityIndex % 20;
		const int32 ChunkY = (EntityIndex / 20) % 20;
		const FVector Location(
			ChunkX * 5000.0 + 1000.0,
			ChunkY * 5000.0 + 1000.0,
			1000.0);
		bCreatedAll = Subsystem->CreateEntity(
			*Definition,
			FTransform(Location),
			EBuildSpatialMobility::Static).IsSet();
	}
	TestTrue(TEXT("Dedicated World 创建十万 Collision Entity"), bCreatedAll);
	if (Subsystem)
	{
		TestEqual(TEXT("十万 Entity 全部进入 Building Registry"),
			Subsystem->GetRegistry().GetEntityCount(), EntityCount);
		TestEqual(TEXT("无 Source 时活跃 Body 为零"),
			Subsystem->GetActiveCollisionBodyCount(), 0);
		TestEqual(TEXT("无 Source 时持久 Collision Entity Record 为零"),
			Subsystem->GetActiveCollisionEntityCount(), 0);
		TestEqual(TEXT("无 Source 时 Collision Dirty/Activation Work 为零"),
			Subsystem->HasPendingCollisionWork(), false);
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingWorldSubsystemSkipsDedicatedRenderHostTest,
	"ElementSandbox.Building.WorldSubsystem.SkipsDedicatedRenderHost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingWorldSubsystemSkipsDedicatedRenderHostTest::RunTest(
	const FString& Parameters)
{
	UWorld::InitializationValues InitializationValues;
	InitializationValues
		.CreatePhysicsScene(true)
		.ShouldSimulatePhysics(false)
		.EnableTraceCollision(true)
		.CreateNavigation(false)
		.CreateAISystem(false);

	UWorld* World = UWorld::CreateWorld(
		EWorldType::PIE,
		false,
		TEXT("BuildingSubsystemDedicated"),
		nullptr,
		true,
		ERHIFeatureLevel::Num,
		&InitializationValues,
		true);
	TestNotNull(TEXT("创建 Dedicated PIE 测试 World"), World);
	if (!World)
	{
		return false;
	}

	GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
	World->SetPlayInEditorInitialNetMode(NM_DedicatedServer);
	World->InitWorld(InitializationValues);
	World->UpdateWorldComponents(true, false);

	UBuildingWorldSubsystem* Subsystem =
		World->GetSubsystem<UBuildingWorldSubsystem>();
	TestNotNull(TEXT("Dedicated World 仍创建 Building Subsystem"), Subsystem);
	TestEqual(TEXT("World NetMode 为 Dedicated Server"),
		World->GetNetMode(), NM_DedicatedServer);
	if (Subsystem)
	{
			UPresentationWorldSubsystem* Presentation =
				World->GetSubsystem<UPresentationWorldSubsystem>();
			TestNotNull(TEXT("Dedicated Server 仍有无分配 Presentation 边界"), Presentation);
			if (Presentation)
			{
				TestEqual(TEXT("Dedicated Server MeshPool 零实例"),
					Presentation->GetMeshPoolStats().ResidentInstanceCount, 0);
				TestEqual(TEXT("Dedicated Server 零 Source"), Presentation->GetSourceCount(), 0);
			}
		TestNotNull(TEXT("Dedicated Server 仍创建 Collision Host"),
			Subsystem->GetCollisionHost());
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
			nullptr,
			TEXT("/Engine/BasicShapes/Cube.Cube"));
		UBuildTestDefinition* Definition = CubeMesh
			? ElementSandbox::Building::Tests::MakeSubsystemTestDefinition(
				Subsystem,
				CubeMesh)
			: nullptr;
		const FBuildEntityHandle Entity = Definition
			? Subsystem->CreateEntity(*Definition, FTransform::Identity)
			: FBuildEntityHandle();
		TestTrue(TEXT("Dedicated Server 创建 Building 不需要表现队列"), Entity.IsSet());
			TestTrue(TEXT("Dedicated Server Motion Pin 是成功空操作"),
				Subsystem->SetPresentationMotionActive(Entity, true));
		TestTrue(TEXT("Dedicated Server Flush 是无分配成功空操作"),
			Subsystem->FlushRenderChanges());
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif

#endif
