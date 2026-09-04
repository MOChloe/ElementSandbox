#if WITH_DEV_AUTOMATION_TESTS

#include "BuildingWorldSubsystem.h"
#include "Collision/BuildCollisionHost.h"
#include "Collision/BuildCollisionTypes.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildTransformFragment.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "Tests/BuildEntityTestTypes.h"

namespace ElementSandbox::Building::Collision::Tests
{
	struct FCollisionTestWorld final
	{
		explicit FCollisionTestWorld(const FName Name)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, Name, nullptr, true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Subsystem = World->GetSubsystem<UBuildingWorldSubsystem>();
		}

		~FCollisionTestWorld()
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

	UStaticMesh* LoadCollisionCube()
	{
		return LoadObject<UStaticMesh>(
			nullptr,
			TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

			UBuildTestDefinition* MakeCollisionDefinition(
				UObject* Outer,
				UStaticMesh* CollisionMesh,
				const int32 CollisionPartCount = 1,
				const double PartSpacing = 0.0,
				const int32 PartsPerRow = 0)
	{
		UBuildTestDefinition* Definition = Outer
			? NewObject<UBuildTestDefinition>(Outer)
			: NewObject<UBuildTestDefinition>();
		for (int32 PartId = 0; PartId < CollisionPartCount; ++PartId)
		{
				FBuildCollisionPartDefinition Part;
				Part.CollisionMesh = CollisionMesh;
					const int32 Column = PartsPerRow > 0 ? PartId % PartsPerRow : PartId;
					const int32 Row = PartsPerRow > 0 ? PartId / PartsPerRow : 0;
					Part.LocalTransform = FTransform(FVector(
						static_cast<double>(Column) * PartSpacing,
						static_cast<double>(Row) * PartSpacing,
						0.0));
				Definition->CollisionParts.Add(Part);
		}
			return Definition;
		}

		FBuildCollisionSource MakeCollisionSource(
			const FVector& SubjectLocation,
			const uint64 Revision,
			const FVector& ImmediateExtent = FVector(400.0),
			const FVector& PrefetchExtent = FVector(400.0),
			const FVector& RetentionExtent = FVector(700.0),
			const FVector& Velocity = FVector::ZeroVector)
		{
			FBuildCollisionSource Source;
			Source.SubjectLocation = SubjectLocation;
			Source.Velocity = Velocity;
			Source.ImmediateBounds = FBox::BuildAABB(
				SubjectLocation,
				ImmediateExtent);
			Source.CameraBounds = FBox::BuildAABB(
				SubjectLocation,
				FVector(100.0));
			Source.PrefetchBounds = FBox::BuildAABB(
				SubjectLocation,
				PrefetchExtent);
			Source.RetentionBounds = FBox::BuildAABB(
				SubjectLocation,
				RetentionExtent);
			Source.Revision = Revision;
			return Source;
		}

		void FlushCollisionUntilStable(
			UBuildingWorldSubsystem& Subsystem,
			const int32 MaxFlushCount = 64)
		{
			for (int32 FlushIndex = 0; FlushIndex < MaxFlushCount; ++FlushIndex)
			{
				verify(Subsystem.FlushCollisionChanges(static_cast<double>(FlushIndex)));
			if (!Subsystem.HasPendingCollisionWork())
			{
				break;
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionDefinitionContractTest,
	"ElementSandbox.Building.Collision.DefinitionContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionDefinitionContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Collision::Tests;
	UStaticMesh* Cube = LoadCollisionCube();
	TestNotNull(TEXT("加载带 Simple Collision 的 Cube"), Cube);
	if (!Cube)
	{
		return false;
	}

	UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>();
	FBuildMeshPartDefinition MeshPart;
	MeshPart.Mesh = Cube;
	MeshPart.LocalTransform = FTransform(
		FRotator(0.0, 25.0, 0.0),
		FVector(10.0, 20.0, 30.0),
		FVector(1.0, 2.0, 0.5));
	Definition->MeshParts.Add(MeshPart);

	FBuildCollisionPartDefinition CollisionPart;
	CollisionPart.CollisionMesh = Cube;
	CollisionPart.LocalTransform = FTransform(FVector(5.0, 0.0, 0.0));
	CollisionPart.DrivenMeshPartId = 0;
	CollisionPart.Mobility = EBuildCollisionMobility::Kinematic;
	Definition->CollisionParts.Add(CollisionPart);
	TestTrue(TEXT("Cube Collision Definition 通过 Simple Collision 校验"),
		Definition->HasValidCollisionDefinition());

	const FTransform EntityWorld(
		FRotator(0.0, 40.0, 0.0),
		FVector(100.0, 200.0, 300.0));
	FTransform CollisionWorld;
	TestTrue(TEXT("解析 Driven Collision Transform"),
		Definition->TryCalculateCollisionPartWorldTransform(
			0,
			EntityWorld,
			{},
			CollisionWorld));
	const FTransform Expected = CollisionPart.LocalTransform
		* MeshPart.LocalTransform
		* EntityWorld;
	TestTrue(TEXT("最终变换遵循 CollisionLocal × MeshPartLocal × EntityWorld"),
		CollisionWorld.Equals(Expected));

	FBox CollisionBounds(ForceInit);
	TestTrue(TEXT("使用 Simple Collision 计算代理 AABB"),
		Definition->TryCalculateCollisionPartWorldBounds(
			0,
			EntityWorld,
			{},
			CollisionBounds));
	const FKAggregateGeom& CubeGeometry = Cube->GetBodySetup()->AggGeom;
	TestEqual(TEXT("Engine Cube 使用一个 Box Simple Collision"),
		CubeGeometry.BoxElems.Num(), 1);
	if (CubeGeometry.BoxElems.Num() == 1)
	{
		FTransform ExpectedRigid = Expected;
		ExpectedRigid.RemoveScaling();
		const FBox ExpectedBounds = CubeGeometry.BoxElems[0]
			.GetFinalScaled(Expected.GetScale3D(), FTransform::Identity)
			.CalcAABB(ExpectedRigid, 1.0f);
		TestTrue(TEXT("Collision Bounds 与 Chaos 非均匀缩放后的 Box 一致"),
			CollisionBounds.Equals(ExpectedBounds, 0.01));
	}

	FBox DefinitionBounds(ForceInit);
	TestTrue(TEXT("Definition 世界 Bounds 同时覆盖视觉与碰撞"),
		Definition->TryCalculateWorldBounds(EntityWorld, DefinitionBounds));
	TestTrue(TEXT("Definition Bounds 包含 Collision Proxy"),
		DefinitionBounds.IsInsideOrOn(CollisionBounds));

	UBuildTestDefinition* InvalidDefinition = NewObject<UBuildTestDefinition>();
	FBuildCollisionPartDefinition InvalidPart;
	InvalidPart.CollisionMesh = NewObject<UStaticMesh>(InvalidDefinition);
	InvalidDefinition->CollisionParts.Add(InvalidPart);
	TestFalse(TEXT("没有 Simple Collision 的代理被拒绝"),
		InvalidDefinition->HasValidCollisionDefinition());
	FBuildEntityRegistry Registry;
	TestFalse(TEXT("非法碰撞配置不能创建半初始化 Entity"),
		InvalidDefinition->CreateEntity(Registry, FTransform::Identity).IsSet());
	TestEqual(TEXT("非法配置不写 Registry"), Registry.GetEntityCount(), 0);

	UBuildTestDefinition* InvalidProfileDefinition =
		MakeCollisionDefinition(nullptr, Cube);
	InvalidProfileDefinition->CollisionParts[0].CollisionProfileName =
		TEXT("ProfileThatMustNotExist");
	TestFalse(TEXT("不存在的 Collision Profile 被拒绝"),
		InvalidProfileDefinition->HasValidCollisionDefinition());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionHostClustersAndSwapRemoveTest,
	"ElementSandbox.Building.Collision.HostClustersAndSwapRemove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionHostClustersAndSwapRemoveTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Collision::Tests;
	FCollisionTestWorld Harness(TEXT("BuildCollisionHost"));
	UStaticMesh* Cube = LoadCollisionCube();
	ABuildCollisionHost* Host = Harness.Subsystem
		? Harness.Subsystem->GetCollisionHost()
		: nullptr;
	TestNotNull(TEXT("Game World 自动创建 Collision Host"), Host);
	if (!Host || !Cube)
	{
		return false;
	}

	FBuildCollisionClusterKey StaticKey;
	StaticKey.Mesh = Cube;
	StaticKey.Mobility = EBuildCollisionMobility::Static;
	StaticKey.CollisionProfileName = TEXT("BlockAll");
	FBuildCollisionClusterKey KinematicKey;
	KinematicKey.Mesh = Cube;
	KinematicKey.Mobility = EBuildCollisionMobility::Kinematic;
	KinematicKey.CollisionProfileName = TEXT("BlockAllDynamic");

	const FTransform StaticTransforms[] = {
		FTransform(FVector(100.0, 0.0, 0.0)),
		FTransform(FVector(200.0, 0.0, 0.0)),
		FTransform(FVector(300.0, 0.0, 0.0))};
	TArray<FBuildCollisionInstanceHandle> StaticInstances;
	TestTrue(TEXT("Static Cluster 批量创建三个 Body"),
		Host->AddInstances(StaticKey, StaticTransforms, StaticInstances));
	TArray<FBuildCollisionInstanceHandle> KinematicInstances;
	const FTransform KinematicTransform(FVector(500.0, 0.0, 0.0));
	TestTrue(TEXT("Kinematic Cluster 创建独立 Body"),
		Host->AddInstances(KinematicKey, MakeArrayView(&KinematicTransform, 1), KinematicInstances));
	TestEqual(TEXT("同 Mesh 不同 Mobility 严格分成两个 Cluster"),
		Host->GetClusterCount(), 2);
	TestEqual(TEXT("Host 共持有四个 Body"), Host->GetInstanceCount(), 4);

	UInstancedStaticMeshComponent* StaticComponent =
		Host->GetClusterComponent(StaticKey);
	UInstancedStaticMeshComponent* KinematicComponent =
		Host->GetClusterComponent(KinematicKey);
	TestNotNull(TEXT("Static ISM 存在"), StaticComponent);
	TestNotNull(TEXT("Kinematic ISM 存在"), KinematicComponent);
	if (StaticComponent && KinematicComponent)
	{
		TestTrue(TEXT("Collision Host Root 使用 Static Mobility"),
			Host->GetRootComponent()->GetMobility() == EComponentMobility::Static);
		TestTrue(TEXT("Static ISM 正确附着到 Collision Host Root"),
			StaticComponent->GetAttachParent() == Host->GetRootComponent());
		TestTrue(TEXT("Kinematic ISM 正确附着到 Collision Host Root"),
			KinematicComponent->GetAttachParent() == Host->GetRootComponent());
		TestTrue(TEXT("Static ISM 使用 Static Mobility"),
			StaticComponent->GetMobility() == EComponentMobility::Static);
		TestTrue(TEXT("Kinematic ISM 使用 Movable Mobility"),
			KinematicComponent->GetMobility() == EComponentMobility::Movable);
		TestEqual(TEXT("Collision ISM 使用 QueryAndPhysics"),
			KinematicComponent->GetCollisionEnabled(),
			ECollisionEnabled::QueryAndPhysics);
		TestFalse(TEXT("Collision ISM 隐藏"), KinematicComponent->IsVisible());
		TestFalse(TEXT("Collision ISM 不生成 Overlap Event"),
			KinematicComponent->GetGenerateOverlapEvents());
		TestFalse(TEXT("Collision ISM 不影响 Navigation"),
			KinematicComponent->CanEverAffectNavigation());
		TestEqual(TEXT("Kinematic ISM 应用配置 Profile"),
			KinematicComponent->GetCollisionProfileName(),
			FName(TEXT("BlockAllDynamic")));
		TestEqual(TEXT("Static Profile 创建 WorldStatic Body"),
			StaticComponent->GetCollisionObjectType(), ECC_WorldStatic);
		TestEqual(TEXT("Kinematic Profile 创建 WorldDynamic Body"),
			KinematicComponent->GetCollisionObjectType(), ECC_WorldDynamic);
		TestNotNull(TEXT("Static ISM 为 Instance 创建 BodyInstance"),
			StaticComponent->GetBodyInstance(NAME_None, false, 0));
		TestNotNull(TEXT("Kinematic ISM 为 Instance 创建 BodyInstance"),
			KinematicComponent->GetBodyInstance(NAME_None, false, 0));
	}

	const FBuildCollisionInstanceHandle Removed = StaticInstances[1];
	const FBuildCollisionInstanceHandle Moved = StaticInstances[2];
	TestTrue(TEXT("Swap-Remove 中间 Body"),
		Host->RemoveInstances(MakeArrayView(&Removed, 1)));
	TestFalse(TEXT("被删除 Handle 立即失效"), Host->IsValidInstance(Removed));
	TestTrue(TEXT("被交换 Body 的 Stable Handle 仍有效"), Host->IsValidInstance(Moved));
	FTransform MovedTransform;
	TestTrue(TEXT("Swap-Remove 后仍可读取正确 Transform"),
		Host->TryGetInstanceTransform(Moved, MovedTransform));
	TestTrue(TEXT("被交换 Body 保留原始世界位置"),
		MovedTransform.Equals(StaticTransforms[2]));

	FBuildCollisionInstanceUpdate Update;
	Update.Instance = KinematicInstances[0];
	Update.WorldTransform = FTransform(FVector(700.0, 0.0, 0.0));
	TestTrue(TEXT("Kinematic Body 使用批量非 Teleport 路径更新"),
		Host->UpdateInstances(KinematicKey, MakeArrayView(&Update, 1)));
	FTransform UpdatedTransform;
	TestTrue(TEXT("更新后 Handle 保持有效"),
		Host->TryGetInstanceTransform(KinematicInstances[0], UpdatedTransform));
	TestTrue(TEXT("Kinematic Transform 已写入 ISM"),
		UpdatedTransform.Equals(Update.WorldTransform));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionActivationAndSourceGenerationTest,
	"ElementSandbox.Building.Collision.ActivationAndSourceGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionActivationAndSourceGenerationTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Collision::Tests;
	FCollisionTestWorld First(TEXT("BuildCollisionActivationFirst"));
	FCollisionTestWorld Second(TEXT("BuildCollisionActivationSecond"));
	UStaticMesh* Cube = LoadCollisionCube();
	if (!First.Subsystem || !Second.Subsystem || !Cube)
	{
		return false;
	}

	UBuildTestDefinition* Definition = MakeCollisionDefinition(
		First.Subsystem,
		Cube);
	const FVector EntityLocation(1000.0, 1000.0, 1000.0);
	const FBuildEntityHandle Entity = First.Subsystem->CreateEntity(
		*Definition,
		FTransform(EntityLocation));
	TestTrue(TEXT("创建 Collision Entity"), Entity.IsSet());
	TestEqual(TEXT("没有 Source 时不创建 Body"),
		First.Subsystem->GetActiveCollisionBodyCount(), 0);
		TestEqual(TEXT("没有 Source 时不保存 Collision Entity Record"),
			First.Subsystem->GetActiveCollisionEntityCount(), 0);
		const FBuildEntityHandle FarUntrackedEntity = First.Subsystem->CreateEntity(
			*Definition,
			FTransform(FVector(50000.0, 50000.0, 1000.0)));
		TestTrue(TEXT("无 Source 时可创建远场逻辑 Entity"), FarUntrackedEntity.IsSet());
		TestFalse(TEXT("远场创建不留下 Collision Work"),
			First.Subsystem->HasPendingCollisionWork());
		TestTrue(TEXT("销毁未投影的远场 Entity"),
			First.Subsystem->DestroyEntity(FarUntrackedEntity));
		TestFalse(TEXT("远场销毁不制造无意义 Collision Work"),
			First.Subsystem->HasPendingCollisionWork());

			const FBuildCollisionSourceHandle FirstSource =
			First.Subsystem->RegisterCollisionSource(
				MakeCollisionSource(EntityLocation, 1));
	TestTrue(TEXT("注册 World 内 Collision Source"), FirstSource.IsSet());
	TestFalse(TEXT("Source Handle 拒绝跨 World 更新"),
			Second.Subsystem->UpdateCollisionSource(
				FirstSource,
				MakeCollisionSource(EntityLocation, 2)));
		TestTrue(TEXT("局部 Required 在首轮 Flush 投影"),
		First.Subsystem->FlushCollisionChanges());
	TestEqual(TEXT("近场 Entity 投影一个 Body"),
		First.Subsystem->GetActiveCollisionBodyCount(), 1);
	FBuildCollisionInstanceHandle StableInstance;
	TestTrue(TEXT("读取 Collision Stable Handle"),
		First.Subsystem->TryGetPartCollisionInstance(Entity, 0, StableInstance));

		TestTrue(TEXT("相同 Source Revision 幂等成功"),
			First.Subsystem->UpdateCollisionSource(
				FirstSource,
				MakeCollisionSource(EntityLocation, 1)));
		TestFalse(TEXT("相同 Source 不制造 Collision Work"),
		First.Subsystem->HasPendingCollisionWork());
	TestTrue(TEXT("幂等更新不改变 Stable Handle"),
		First.Subsystem->TryGetPartCollisionInstance(Entity, 0, StableInstance));

		const FBuildCollisionSourceHandle SecondSource =
			First.Subsystem->RegisterCollisionSource(
				MakeCollisionSource(EntityLocation, 1));
	FlushCollisionUntilStable(*First.Subsystem);
	TestEqual(TEXT("多 Source 取并集而不复制 Body"),
		First.Subsystem->GetActiveCollisionBodyCount(), 1);
	TestEqual(TEXT("记录两个 Source"), First.Subsystem->GetCollisionSourceCount(), 2);
	TestTrue(TEXT("注销第一个 Source"),
		First.Subsystem->UnregisterCollisionSource(FirstSource));
		TestFalse(TEXT("注销后旧 Generation Handle 失效"),
			First.Subsystem->UpdateCollisionSource(
				FirstSource,
				MakeCollisionSource(EntityLocation, 2)));
	FlushCollisionUntilStable(*First.Subsystem);
	TestEqual(TEXT("仍有 Source 引用时 Body 保留"),
		First.Subsystem->GetActiveCollisionBodyCount(), 1);

	TestTrue(TEXT("注销最后一个 Source"),
		First.Subsystem->UnregisterCollisionSource(SecondSource));
	FlushCollisionUntilStable(*First.Subsystem);
		TestEqual(TEXT("最后 Source 注销并经过 Grace 后删除 Body"),
		First.Subsystem->GetActiveCollisionBodyCount(), 0);
	TestEqual(TEXT("卸载后不保留 Entity Collision Record"),
		First.Subsystem->GetActiveCollisionEntityCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionLargePartNearestSurfaceTest,
	"ElementSandbox.Building.Collision.LargePartUsesNearestSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionLargePartNearestSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Collision::Tests;
	FCollisionTestWorld Harness(TEXT("BuildCollisionLargePartNearestSurface"));
	UStaticMesh* Cube = LoadCollisionCube();
	if (!TestTrue(TEXT("巨大 Part 碰撞测试环境有效"), Harness.Subsystem && Cube))
	{
		return false;
	}

	UBuildTestDefinition* Definition = MakeCollisionDefinition(Harness.Subsystem, Cube);
	Definition->CollisionParts[0].LocalTransform.SetScale3D(FVector(200.0, 1.0, 1.0));
	const FVector EntityLocation(10000.0, 0.0, 0.0);
	const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(*Definition, FTransform(EntityLocation));
	const FBuildCollisionSourceHandle Source = Harness.Subsystem->RegisterCollisionSource(
		MakeCollisionSource(FVector::ZeroVector, 1));
	if (!TestTrue(TEXT("创建中心在 100m 外但表面到达玩家的巨大 Part"), Entity.IsSet() && Source.IsSet()))
	{
		return false;
	}

	TestTrue(TEXT("Entity 原点确实在 4m Immediate 半径之外"), EntityLocation.Size() > 400.0);
	TestTrue(TEXT("巨大 Part 按世界 Bounds 最近表面进入局部碰撞"),
		Harness.Subsystem->FlushCollisionChanges());
	TestEqual(TEXT("只投影相交的巨大 Part Body"), Harness.Subsystem->GetActiveCollisionBodyCount(), 1);
	FBuildCollisionInstanceHandle Instance;
	TestTrue(TEXT("巨大 Part 获得稳定碰撞句柄"),
		Harness.Subsystem->TryGetPartCollisionInstance(Entity, 0, Instance));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionDenseChunk10kTest,
	"ElementSandbox.Building.Collision.DenseChunk10k",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionDenseChunk10kTest::RunTest(const FString& Parameters)
{
		using namespace ElementSandbox::Building::Collision::Tests;
		constexpr int32 PartCount = 10000;
	FCollisionTestWorld Harness(TEXT("BuildCollisionDenseChunk10k"));
	UStaticMesh* Cube = LoadCollisionCube();
	if (!TestTrue(TEXT("10k 碰撞测试环境有效"), Harness.Subsystem && Cube))
	{
		return false;
	}

		UBuildTestDefinition* Definition = MakeCollisionDefinition(
				Harness.Subsystem,
				Cube,
				PartCount,
				200.0,
				100);
		const FVector Location(1000.0, 1000.0, 1000.0);
		const FBuildCollisionSource LocalSource = MakeCollisionSource(
			Location,
			1,
			FVector(25.0),
			FVector(25.0),
			FVector(700.0));
		const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
			*Definition,
			FTransform(Location));
			const FBuildCollisionSourceHandle Source =
				Harness.Subsystem->RegisterCollisionSource(
					LocalSource);
			if (!TestTrue(TEXT("创建 100x100 密集网格的 10k Part Entity 与局部 Source"),
		Entity.IsSet() && Source.IsSet()))
	{
		return false;
	}

		TestTrue(TEXT("局部 Source 执行精确 Part 过滤"),
			Harness.Subsystem->FlushCollisionChanges());
		TestEqual(TEXT("候选 Entity 的 Part 只线性检查一次"),
			Harness.Subsystem->GetLastInspectedCollisionPartCount(), PartCount);
			TestTrue(TEXT("只投影精确相交的局部 Part，不投影整 Entity 或 Chunk"),
				Harness.Subsystem->GetActiveCollisionBodyCount() == 1);
	int64 TotalActivationInspections =
		Harness.Subsystem->GetLastInspectedCollisionPartCount();
	for (int32 FlushIndex = 0;
		FlushIndex < 256 && Harness.Subsystem->HasPendingCollisionWork();
		++FlushIndex)
	{
		if (!Harness.Subsystem->FlushCollisionChanges())
		{
			AddError(TEXT("10k Chunk 激活 Flush 失败"));
			return false;
		}
			TotalActivationInspections +=
			Harness.Subsystem->GetLastInspectedCollisionPartCount();
	}
		TestFalse(TEXT("局部候选队列已收敛"),
			Harness.Subsystem->HasPendingCollisionWork());
			TestTrue(TEXT("最终 Resident 仍远小于 10k"),
				Harness.Subsystem->GetActiveCollisionBodyCount() == 1);
	TestEqual(TEXT("激活成员检查线性经过每个 Part 一次"),
		TotalActivationInspections,
		static_cast<int64>(PartCount));
	FBuildCollisionInstanceHandle FirstHandle;
	FBuildCollisionInstanceHandle LastHandle;
		TestTrue(TEXT("近场首 Part 有 Stable Handle"),
			Harness.Subsystem->TryGetPartCollisionInstance(Entity, 0, FirstHandle));
		TestFalse(TEXT("远场末 Part 没有 Chaos Body"),
			Harness.Subsystem->TryGetPartCollisionInstance(
				Entity,
				PartCount - 1,
				LastHandle));

			TestTrue(TEXT("相同 Source 更新保持幂等"),
				Harness.Subsystem->UpdateCollisionSource(
					Source,
					LocalSource));
		TestFalse(TEXT("幂等更新不排入重复选择"),
		Harness.Subsystem->HasPendingCollisionWork());
			TestEqual(TEXT("幂等更新不复制 Body"),
				Harness.Subsystem->GetActiveCollisionBodyCount(),
				1);

	TestTrue(TEXT("注销最后一个 Source 开始预算化停用"),
		Harness.Subsystem->UnregisterCollisionSource(Source));
	int64 TotalDeactivationInspections = 0;
	for (int32 FlushIndex = 0;
		FlushIndex < 256 && Harness.Subsystem->HasPendingCollisionWork();
		++FlushIndex)
	{
			if (!Harness.Subsystem->FlushCollisionChanges(
					static_cast<double>(FlushIndex)))
		{
			AddError(TEXT("10k Chunk 停用 Flush 失败"));
			return false;
		}
		TotalDeactivationInspections +=
			Harness.Subsystem->GetLastInspectedCollisionPartCount();
	}
	TestFalse(TEXT("10k Part 最终全部完成停用"),
		Harness.Subsystem->HasPendingCollisionWork());
		TestEqual(TEXT("Grace 淘汰不重新扫描远场 10k Part"),
			TotalDeactivationInspections, 0ll);
	TestEqual(TEXT("停用后 Body 与 Entity Collision Record 全部释放"),
		Harness.Subsystem->GetActiveCollisionBodyCount()
			+ Harness.Subsystem->GetActiveCollisionEntityCount(),
		0);
	TestFalse(TEXT("停用后旧 Part Handle 反向查询失效"),
		Harness.Subsystem->TryGetPartCollisionInstance(Entity, 0, FirstHandle));
	AddInfo(FString::Printf(
			TEXT("LocalCollision10k: SelectionInspections=%lld EvictionInspections=%lld Bodies=%d"),
		TotalActivationInspections,
		TotalDeactivationInspections,
		Harness.Subsystem->GetActiveCollisionBodyCount()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionActivationBudgetTest,
	"ElementSandbox.Building.Collision.ActivationBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionActivationBudgetTest::RunTest(const FString& Parameters)
{
		using namespace ElementSandbox::Building::Collision::Tests;
		FCollisionTestWorld Harness(TEXT("BuildCollisionBudget"));
		const int32 PartBudget =
			FBuildCollisionActivationConfig().MaxPrefetchAddsPerFrame;
	UStaticMesh* Cube = LoadCollisionCube();
	if (!Harness.Subsystem || !Cube)
	{
		return false;
	}

	UBuildTestDefinition* Definition = MakeCollisionDefinition(
			Harness.Subsystem,
			Cube,
			200,
			100.0);
		const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
			*Definition,
			FTransform::Identity);
		TestTrue(TEXT("创建 200 Collision Part 的长条 Entity"), Entity.IsSet());
		const FVector SourceLocation(-200.0, 0.0, 0.0);
		const FBuildCollisionSourceHandle Source =
			Harness.Subsystem->RegisterCollisionSource(
				MakeCollisionSource(
					SourceLocation,
					1,
					FVector(25.0),
					FVector(20500.0),
					FVector(21000.0),
					FVector(500.0, 0.0, 0.0)));
		TestTrue(TEXT("注册运动预测 Source"), Source.IsSet());
		TestTrue(TEXT("首轮 Collision Prefetch Flush"),
			Harness.Subsystem->FlushCollisionChanges());
		TestEqual(TEXT("局部选择精确检查 200 Part"),
			Harness.Subsystem->GetLastInspectedCollisionPartCount(), 200);
		TestEqual(TEXT("首轮普通 Prefetch 严格受 16 Part 预算"),
			Harness.Subsystem->GetActiveCollisionBodyCount(), PartBudget);

	FlushCollisionUntilStable(*Harness.Subsystem);
		TestEqual(TEXT("后续预算批次最终投影全部 Required Part"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 200);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionRetentionGraceAndHostRecoveryTest,
	"ElementSandbox.Building.Collision.RetentionGraceAndHostRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionRetentionGraceAndHostRecoveryTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Collision::Tests;
	FCollisionTestWorld Harness(TEXT("BuildCollisionRetentionGrace"));
	UStaticMesh* Cube = LoadCollisionCube();
	if (!Harness.Subsystem || !Cube)
	{
		return false;
	}

	UBuildTestDefinition* Definition = MakeCollisionDefinition(
		Harness.Subsystem,
		Cube);
	Definition->CollisionParts[0].LocalTransform = FTransform(
		FRotator::ZeroRotator,
		FVector::ZeroVector,
		FVector(3.0));
	const FVector BoundaryLocation(4950.0, 1000.0, 1000.0);
	const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
		*Definition,
		FTransform(BoundaryLocation));
	const FBuildCollisionSourceHandle Source =
		Harness.Subsystem->RegisterCollisionSource(
			MakeCollisionSource(BoundaryLocation, 1));
	FlushCollisionUntilStable(*Harness.Subsystem);
	TestEqual(TEXT("局部 Source 创建一个 Chaos Body"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 1);

	const FVector FarLocation(30000.0, 1000.0, 1000.0);
	TestTrue(TEXT("Source 离开 Retention"),
		Harness.Subsystem->UpdateCollisionSource(
			Source,
			MakeCollisionSource(FarLocation, 2)));
	TestTrue(TEXT("开始 Grace 计时"), Harness.Subsystem->FlushCollisionChanges(0.0));
	TestEqual(TEXT("离开 Retention 后 3 秒内不立即删除"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 1);
	TestTrue(TEXT("Grace 未到期仍可 Flush"), Harness.Subsystem->FlushCollisionChanges(2.9));
	TestEqual(TEXT("2.9 秒仍保留 Body"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 1);
	TestTrue(TEXT("Grace 到期执行淘汰"), Harness.Subsystem->FlushCollisionChanges(3.1));
	TestEqual(TEXT("3 秒后低频回收 Body"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 0);

	TestTrue(TEXT("Source 返回 Entity 附近"),
		Harness.Subsystem->UpdateCollisionSource(
			Source,
			MakeCollisionSource(BoundaryLocation, 3)));
	TestTrue(TEXT("返回后重新投影当前 Body"),
		Harness.Subsystem->FlushCollisionChanges(4.0));
	TestEqual(TEXT("只恢复一份 Body"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 1);

	ABuildCollisionHost* Host = Harness.Subsystem->GetCollisionHost();
	TestNotNull(TEXT("Collision Host 存在"), Host);
	if (Host)
	{
		Host->ClearInstances();
	}
	FBuildTransformFragment* Transform =
		Harness.Subsystem->GetRegistry().FindMutableFragment<FBuildTransformFragment>(Entity);
	if (Transform)
	{
		Transform->WorldTransform.SetLocation(
			BoundaryLocation + FVector(100.0, 0.0, 0.0));
	}
	TestTrue(TEXT("更新 Resident Entity 空间 Transform"),
		Transform && Harness.Subsystem->CommitEntityTransformChange(Entity));
	TestFalse(TEXT("Host 外部失效时当前批次触发 Clear-All 恢复"),
		Harness.Subsystem->FlushCollisionChanges(4.1));
	TestTrue(TEXT("下一 Flush 从局部 Source 完整重投影"),
		Harness.Subsystem->FlushCollisionChanges(4.2));
	TestEqual(TEXT("Host 重建后仍只有一个 Resident Body"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 1);

	TestTrue(TEXT("真实 Entity 销毁"), Harness.Subsystem->DestroyEntity(Entity));
	TestTrue(TEXT("销毁立即提交碰撞删除"),
		Harness.Subsystem->FlushCollisionChanges(4.3));
	TestEqual(TEXT("真实销毁不等待 Grace"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 0);
	TestTrue(TEXT("注销 Source"), Harness.Subsystem->UnregisterCollisionSource(Source));
	return true;
}

#endif
