#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "WorldDestructionAuthorityService.h"

#include "BuildingWorldSubsystem.h"
#include "Chunk/WorldChunkTypes.h"
#include "Components/BoxComponent.h"
#include "Definition/WorldObjectDefinition.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/BuildDamageFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildWorldIdentityFragment.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectDamageFragment.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Tree/SettlementTreeDefinition.h"
#include "Wood/WoodBuildingDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"

namespace ElementSandbox::AxeDestruction::Tests
{
	using namespace UE::ElementSandbox::Destruction;

	struct FDestructionWorld final
	{
		explicit FDestructionWorld(const ENetMode NetMode = NM_Standalone)
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
				TEXT("AxeDestructionAuthority"),
				nullptr,
				true,
				ERHIFeatureLevel::Num,
				&InitializationValues,
				true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(NetMode);
			World->InitWorld(InitializationValues);
			World->UpdateWorldComponents(true, false);
			Buildings = World->GetSubsystem<UBuildingWorldSubsystem>();
			WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
		}

		~FDestructionWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		void AdvancePostActorFrame(const float DeltaSeconds = 1.0f / 60.0f) const
		{
			++GFrameCounter;
			FWorldDelegates::OnWorldPostActorTick.Broadcast(World, LEVELTICK_All, DeltaSeconds);
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Buildings = nullptr;
		UWorldObjectWorldSubsystem* WorldObjects = nullptr;
	};

	FWorldDestructionTarget MakeTarget(
		UWorldObjectWorldSubsystem& WorldObjects,
		const FWorldObjectEntityHandle Entity)
	{
		FWorldDestructionTarget Target;
		Target.Domain = EWorldDestructionTargetDomain::WorldObject;
		Target.WorldObject = Entity;
		Target.WorldEntityId = WorldObjects.GetWorldEntityId(Entity);
		const FWorldObjectWorldIdentityFragment* Identity =
			WorldObjects.GetRegistry().FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
		Target.SourceRevision = Identity ? Identity->StateRevision : 0;
		return Target;
	}

	FWorldDestructionTarget MakeTarget(
		UBuildingWorldSubsystem& Buildings,
		const FBuildEntityHandle Entity)
	{
		FWorldDestructionTarget Target;
		Target.Domain = EWorldDestructionTargetDomain::Building;
		Target.Building = Entity;
		Target.WorldEntityId = Buildings.GetWorldEntityId(Entity);
		const FBuildWorldIdentityFragment* Identity =
			Buildings.GetRegistry().FindFragment<FBuildWorldIdentityFragment>(Entity);
		Target.SourceRevision = Identity ? Identity->StateRevision : 0;
		return Target;
	}

	bool ApplyDamage(UWorld& World, const FWorldDestructionTarget& Target, const float Damage)
	{
		FWorldDestructionRequest Request;
		Request.Target = Target;
		Request.DamageMode = EWorldDestructionDamageMode::Additive;
		Request.Damage = Damage;
		return FWorldDestructionAuthorityService::TryApplyRequest(World, Request);
	}

	FWorldObjectEntityHandle CreateTree(
		UWorldObjectWorldSubsystem& WorldObjects,
		UWorldObjectDefinition& Definition,
		const FVector& Location)
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = &Definition;
		Desc.WorldTransform = FTransform(Location);
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		return WorldObjects.CreateEntity(Desc);
	}

	int32 GatherWoodBlocks(
		const UWorldObjectWorldSubsystem& WorldObjects,
		TArray<FWorldObjectEntityHandle>& OutEntities)
	{
		OutEntities.Reset();
		const UWorldObjectDefinition* WoodBlock = GetDefault<UWoodBlockWorldObjectDefinition>();
		const auto Definitions =
			WorldObjects.GetRegistry().GetFragmentPoolView<FWorldObjectDefinitionFragment>();
		for (int32 Index = 0; Index < Definitions.Num(); ++Index)
		{
			if (Definitions.Fragments[Index].Definition.Get() == WoodBlock)
			{
				OutEntities.Add(Definitions.Entities[Index]);
			}
		}
		return OutEntities.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAxeMillionUndamagedBuildingsStaySparseTest,
	"ElementSandbox.AxeDestruction.MillionUndamagedBuildingsStaySparse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAxeMillionUndamagedBuildingsStaySparseTest::RunTest(const FString& Parameters)
{
	constexpr int32 BuildingCount = 1'000'000;
	FBuildEntityRegistry Registry;
	Registry.ReserveEntityCapacity(BuildingCount);
	for (int32 Index = 0; Index < BuildingCount; ++Index)
	{
		if (!Registry.CreateEntity().IsSet())
		{
			AddError(FString::Printf(TEXT("Building %d 创建失败"), Index));
			return false;
		}
	}
	TestEqual(TEXT("一百万静止 Building 实体全部存在"),
		Registry.GetEntityCount(), BuildingCount);
	TestEqual(TEXT("未受击总人口不分配 Damage Fragment"),
		Registry.GetFragmentCount<FBuildDamageFragment>(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAxeNearestSparseDamageProductsTest,
	"ElementSandbox.AxeDestruction.NearestSparseDamageProducts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAxeNearestSparseDamageProductsTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::AxeDestruction::Tests;
	FDestructionWorld Harness;
	if (!Harness.Buildings || !Harness.WorldObjects)
	{
		return false;
	}

	USettlementTreeDefinition* TreeDefinition =
		Cast<USettlementTreeDefinition>(Harness.WorldObjects->FindDefinition(
			GetDefault<USettlementTreeDefinition>()->DefinitionId));
	UWoodBuildingDefinition* BuildingDefinition =
		NewObject<UWoodBuildingDefinition>(Harness.World);
	TestTrue(TEXT("测试木制 Building Definition 初始化"),
		BuildingDefinition && BuildingDefinition->Initialize(
			TEXT("Test.Axe.WoodBuilding"), FVector(100.0, 100.0, 300.0)));
	if (!TreeDefinition || !BuildingDefinition || !BuildingDefinition->HasValidDefinitionId())
	{
		return false;
	}

	const FWorldObjectEntityHandle Tree = CreateTree(
		*Harness.WorldObjects, *TreeDefinition, FVector(600.0, 0.0, 0.0));
	const FBuildEntityHandle Building = Harness.Buildings->CreateEntity(
		*BuildingDefinition, FTransform(FVector(1100.0, 0.0, 0.0)));
	TestTrue(TEXT("创建可破坏树木"), Tree.IsSet());
	TestTrue(TEXT("创建可破坏 Building"), Building.IsSet());
	TestFalse(TEXT("未受击树木不分配 Damage Fragment"),
		Harness.WorldObjects->GetRegistry().HasFragment<FWorldObjectDamageFragment>(Tree));
	TestFalse(TEXT("未受击 Building 不分配 Damage Fragment"),
		Harness.Buildings->GetRegistry().HasFragment<FBuildDamageFragment>(Building));

	FWorldDestructionTarget Nearest;
	TestTrue(TEXT("跨 Building/WorldObject 查询命中最近目标"),
		FWorldDestructionAuthorityService::TryResolveNearestTarget(
			*Harness.World,
			FVector(0.0, 0.0, 150.0),
			FVector::ForwardVector,
			FVector(0.0, 0.0, 150.0),
			2000.0,
			Nearest));
	TestTrue(TEXT("最近目标是树木 WorldObject"),
		Nearest.Domain == EWorldDestructionTargetDomain::WorldObject
		&& Nearest.WorldObject == Tree);
	TestTrue(TEXT("第一击立即写入稀疏伤害"),
		ApplyDamage(
			*Harness.World, Nearest, 25.0f));
	const FWorldObjectDamageFragment* Damage =
		Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectDamageFragment>(Tree);
	TestTrue(TEXT("第一击只创建一份 Damage Fragment"),
		Damage && FMath::IsNearlyEqual(Damage->AccumulatedDamage, 25.0f)
		&& Harness.WorldObjects->GetRegistry()
			.GetFragmentPoolView<FWorldObjectDamageFragment>().Num() == 1);
	TestFalse(TEXT("同一次挥砍不会伤害后方 Building"),
		Harness.Buildings->GetRegistry().HasFragment<FBuildDamageFragment>(Building));

	TestTrue(TEXT("第二击累计"),
		ApplyDamage(
			*Harness.World, Nearest, 25.0f));
	TestTrue(TEXT("第三击累计"),
		ApplyDamage(
			*Harness.World, Nearest, 25.0f));
	bool bObservedStagedProducts = false;
	bool bAllStagedPhysicsHeldAtInitialTransform = true;
	const FDelegateHandle PreDestroyHandle = Harness.WorldObjects->OnEntityPreDestroy().AddLambda(
		[&](const FWorldObjectEntityHandle Entity, bool&)
		{
			if (Entity != Tree)
			{
				return;
			}
			TArray<FWorldObjectEntityHandle> StagedProducts;
			bObservedStagedProducts = GatherWoodBlocks(*Harness.WorldObjects, StagedProducts) >= 3;
			for (const FWorldObjectEntityHandle Product : StagedProducts)
			{
				UWorldObjectProxyComponent* Proxy = Harness.WorldObjects->GetProxy(Product);
				const AWorldObjectPhysicsProxyActor* PhysicsProxy =
					Proxy ? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
				bAllStagedPhysicsHeldAtInitialTransform &= PhysicsProxy
					&& PhysicsProxy->IsPhysicsConfigured()
					&& !PhysicsProxy->IsPhysicsReleased()
					&& !PhysicsProxy->GetPhysicsBox()->IsSimulatingPhysics();
			}
		});
	TestTrue(TEXT("第四击达到阈值并完成破坏事务"),
		ApplyDamage(
			*Harness.World, Nearest, 25.0f));
	Harness.WorldObjects->OnEntityPreDestroy().Remove(PreDestroyHandle);
	TestTrue(TEXT("源销毁前已准备好全部木块"), bObservedStagedProducts);
	TestTrue(TEXT("跨域事务发布前不允许隐形 Physics Proxy 先下落"),
		bAllStagedPhysicsHeldAtInitialTransform);
	TestFalse(TEXT("树木源 Entity 已 GameplayDestroy"),
		Harness.WorldObjects->IsEntityAlive(Tree));
	TestTrue(TEXT("后方 Building 仍存活"), Harness.Buildings->IsEntityAlive(Building));

	TArray<FWorldObjectEntityHandle> Products;
	const int32 ProductCount = GatherWoodBlocks(*Harness.WorldObjects, Products);
	TestTrue(TEXT("破坏生成 3–6 个固定木块"), ProductCount >= 3 && ProductCount <= 6);
	TestEqual(TEXT("全部木块直接进入 Physics Active Array"),
		Harness.WorldObjects->GetRuntimeStats().ActiveCount, ProductCount);
	for (const FWorldObjectEntityHandle Product : Products)
	{
		const FWorldObjectMotionFragment* Motion =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Product);
		const FWorldObjectPhysicsBodyFragment* Physics =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectPhysicsBodyFragment>(Product);
		UWorldObjectProxyComponent* Proxy = Harness.WorldObjects->GetProxy(Product);
		const AWorldObjectPhysicsProxyActor* PhysicsProxy =
			Proxy ? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
		TestTrue(TEXT("木块使用 Physics + LooseDebris"),
			Motion && Motion->State == EWorldObjectMotionState::Physics
			&& Physics
			&& Physics->CollisionPolicy == EWorldObjectPhysicsCollisionPolicy::LooseDebris
			&& Proxy);
		TestTrue(TEXT("木块发布后仍先保留初始位置，等待可见投影建立"),
			PhysicsProxy && PhysicsProxy->IsPhysicsConfigured()
				&& !PhysicsProxy->IsPhysicsReleased()
				&& !PhysicsProxy->GetPhysicsBox()->IsSimulatingPhysics());
		TestFalse(TEXT("木块自身默认不可破坏"),
			Harness.WorldObjects->GetRegistry().HasFragment<FWorldObjectDamageFragment>(Product));
		Harness.WorldObjects->QueueProxyMotionState(
			Harness.WorldObjects->GetWorldEntityId(Product),
			EWorldObjectMotionState::Dormant);
	}
	Harness.AdvancePostActorFrame();
	TestEqual(TEXT("Chaos Sleep 后全部木块退出 Active Array"),
		Harness.WorldObjects->GetRuntimeStats().ActiveCount, 0);
	TestEqual(TEXT("Dormant 木块回收全部自动 Physics Proxy"),
		Harness.WorldObjects->GetRuntimeStats().BoundProxyCount, 0);
	for (const FWorldObjectEntityHandle Product : Products)
	{
		const FWorldObjectMotionFragment* Motion =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Product);
		TestTrue(TEXT("木块稳定留在 Dormant ECS/HISM 投影"),
			Motion && Motion->State == EWorldObjectMotionState::Dormant
			&& !Harness.WorldObjects->GetProxy(Product));
	}

	UWorldObjectItemCatalogSubsystem* ItemCatalog =
		Harness.World->GetSubsystem<UWorldObjectItemCatalogSubsystem>();
	TestTrue(TEXT("木块存在 WorldObject→Item 拾取映射"),
		ItemCatalog && ItemCatalog->FindItemDefinition(
			GetDefault<UWoodBlockWorldObjectDefinition>()) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAxeRuntimeOnlyDamageAndGuardTest,
	"ElementSandbox.AxeDestruction.RuntimeOnlyDamageAndGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAxeRuntimeOnlyDamageAndGuardTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::AxeDestruction::Tests;
	FDestructionWorld Harness;
	if (!Harness.Buildings || !Harness.WorldObjects)
	{
		return false;
	}

	USettlementTreeDefinition* TreeDefinition =
		Cast<USettlementTreeDefinition>(Harness.WorldObjects->FindDefinition(
			GetDefault<USettlementTreeDefinition>()->DefinitionId));
	if (!TreeDefinition)
	{
		AddError(TEXT("生产 Tree Definition 未由 Catalog 注册"));
		return false;
	}
	const FWorldObjectEntityHandle Tree = CreateTree(
		*Harness.WorldObjects, *TreeDefinition, FVector(500.0, 0.0, 0.0));
	const FWorldDestructionTarget OriginalTarget = MakeTarget(*Harness.WorldObjects, Tree);
	TestTrue(TEXT("树木第一击成功"),
		ApplyDamage(
			*Harness.World, OriginalTarget, 25.0f));
	const FWorldEntityId TreeId = OriginalTarget.WorldEntityId;
	TArray<FWorldPersistentEntityRecord> Records;
	FString Error;
	TestTrue(TEXT("Capture 不依赖 RuntimeOnly Damage"),
		Harness.WorldObjects->CapturePersistentBatchForTesting(
			MakeArrayView(&TreeId, 1), Records, Error));
	TestEqual(TEXT("Capture 一条树记录"), Records.Num(), 1);
	if (Records.Num() != 1)
	{
		return false;
	}
	const FWorldChunkCoord HomeChunk =
		FWorldChunkCoord::FromWorldLocation(Records[0].WorldTransform.GetLocation());
	TestTrue(TEXT("RuntimeEvict 与 GameplayDestroy 分离"),
		Harness.WorldObjects->RuntimeEvictPersistentBatchForTesting(
			HomeChunk, MakeArrayView(&TreeId, 1), Error));
	TestFalse(TEXT("旧 Generation 已失效"), Harness.WorldObjects->IsEntityAlive(Tree));
	TestTrue(TEXT("Restore 使用同一持久 ID 重建新 Generation"),
		Harness.WorldObjects->RestorePersistentBatchForTesting(HomeChunk, Records, Error));
	const FWorldObjectEntityHandle Restored = Harness.WorldObjects->FindEntity(TreeId);
	TestTrue(TEXT("重新注入后树木恢复"), Restored.IsSet() && Restored != Tree);
	TestFalse(TEXT("重新注入不恢复临时伤害，等价于满耐久"),
		Harness.WorldObjects->GetRegistry().HasFragment<FWorldObjectDamageFragment>(Restored));
	TestFalse(TEXT("旧 Generation/ID 组合不能继续扣血"),
		ApplyDamage(
			*Harness.World, OriginalTarget, 25.0f));

	UWorldObjectDefinition* ObstacleDefinition =
		NewObject<UWorldObjectDefinition>(Harness.World);
	ObstacleDefinition->DefinitionId = TEXT("Test.Axe.Obstacle");
	ObstacleDefinition->SpatialClass = EWorldObjectSpatialClass::PermanentStatic;
	ObstacleDefinition->InteractionLocalBounds =
		FBox(FVector(-40.0), FVector(40.0));
	ObstacleDefinition->ShapeGeometry =
		FWorldObjectShapeDefinition::MakeObbFromBounds(
			ObstacleDefinition->InteractionLocalBounds);
	const FWorldObjectEntityHandle Obstacle = CreateTree(
		*Harness.WorldObjects,
		*ObstacleDefinition,
		FVector(200.0, 0.0, 150.0));
	// 非破坏对象成为最近遮挡时，不允许穿透选中后方树。
	TestTrue(TEXT("创建不可破坏遮挡物"), Obstacle.IsSet());
	FWorldDestructionTarget Occluded;
	TestFalse(TEXT("最近不可破坏对象阻挡后方可破坏目标"),
		FWorldDestructionAuthorityService::TryResolveNearestTarget(
			*Harness.World,
			FVector(0.0, 0.0, 150.0),
			FVector::ForwardVector,
			FVector(0.0, 0.0, 150.0),
			1000.0,
			Occluded));

	FDestructionWorld ClientHarness(NM_Client);
	TestEqual(TEXT("客户端测试 World 的 NetMode"), ClientHarness.World->GetNetMode(), NM_Client);
	TestFalse(TEXT("客户端不能执行权威伤害"),
		ApplyDamage(
			*ClientHarness.World, MakeTarget(*Harness.WorldObjects, Restored), 100.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAxeBuildingRollbackTest,
	"ElementSandbox.AxeDestruction.BuildingRollbackAndStableRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAxeBuildingRollbackTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::AxeDestruction::Tests;
	FDestructionWorld Harness;
	if (!Harness.Buildings || !Harness.WorldObjects)
	{
		return false;
	}

	UWoodBuildingDefinition* Definition =
		NewObject<UWoodBuildingDefinition>(Harness.World);
	TestTrue(TEXT("回滚测试 Building Definition 初始化"),
		Definition && Definition->Initialize(
			TEXT("Test.Axe.RollbackBuilding"), FVector(100.0, 100.0, 300.0)));
	if (!Definition || !Definition->HasValidDefinitionId())
	{
		return false;
	}
	const FBuildEntityHandle Building = Harness.Buildings->CreateEntity(
		*Definition, FTransform(FVector(500.0, 0.0, 0.0)));
	const FWorldDestructionTarget Target = MakeTarget(*Harness.Buildings, Building);
	for (int32 Hit = 0; Hit < 3; ++Hit)
	{
		TestTrue(TEXT("阈值前 Building 伤害累计"),
			ApplyDamage(
				*Harness.World, Target, 25.0f));
	}
	const FBuildDamageFragment* BeforeFailure =
		Harness.Buildings->GetRegistry().FindFragment<FBuildDamageFragment>(Building);
	TestTrue(TEXT("阈值前累计为 75"),
		BeforeFailure && FMath::IsNearlyEqual(BeforeFailure->AccumulatedDamage, 75.0f));

	const FDelegateHandle RejectHandle = Harness.Buildings->OnEntityPreDestroy().AddLambda(
		[](const FBuildEntityHandle, bool& bCanDestroy)
		{
			bCanDestroy = false;
		});
	TArray<FWorldObjectEntityHandle> Products;
	const int32 ProductCountBefore = GatherWoodBlocks(*Harness.WorldObjects, Products);
	TestFalse(TEXT("源销毁被否决时整次破坏失败"),
		ApplyDamage(
			*Harness.World, Target, 25.0f));
	Harness.Buildings->OnEntityPreDestroy().Remove(RejectHandle);
	const FBuildDamageFragment* AfterFailure =
		Harness.Buildings->GetRegistry().FindFragment<FBuildDamageFragment>(Building);
	TestTrue(TEXT("失败后源 Building 和伤害都恢复"),
		Harness.Buildings->IsEntityAlive(Building)
		&& AfterFailure
		&& FMath::IsNearlyEqual(AfterFailure->AccumulatedDamage, 75.0f));
	TestEqual(TEXT("失败后不留下半完成木块 Entity"),
		GatherWoodBlocks(*Harness.WorldObjects, Products), ProductCountBefore);

	TestTrue(TEXT("相同源与 Revision 的重试成功"),
		ApplyDamage(
			*Harness.World, Target, 25.0f));
	TestFalse(TEXT("成功后源 Building 已终结"),
		Harness.Buildings->IsEntityAlive(Building));
	const int32 ProductCountAfter = GatherWoodBlocks(*Harness.WorldObjects, Products);
	TestTrue(TEXT("重试生成 3–6 个木块"),
		ProductCountAfter - ProductCountBefore >= 3
		&& ProductCountAfter - ProductCountBefore <= 6);
	TestFalse(TEXT("已删除的 stale Building 不能重复生成"),
		ApplyDamage(
			*Harness.World, Target, 100.0f));
	return true;
}

#endif
