#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Door/DoorBuildingDefinition.h"
#include "Door/DoorInteractionResolver.h"
#include "Door/DoorProcessor.h"
#include "Door/DoorStateFragment.h"
#include "Combustion/BuildCombustionCatalog.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Entity/BuildDamageFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildRenderCustomDataFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Engine/StaticMesh.h"
#include "Item/ItemInstance.h"
#include "Materials/MaterialInterface.h"
#if WITH_EDITOR
#include "Materials/Material.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorDefinitionImplementsInventoryContractTest,
	"ElementSandbox.BuildingCatalog.Door.ImplementsInventoryDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorDefinitionImplementsInventoryContractTest::RunTest(const FString& Parameters)
{
	UDoorBuildingDefinition* Definition = NewObject<UDoorBuildingDefinition>();
	UItemInstance* Item = NewObject<UItemInstance>();

	TestEqual(TEXT("Door 预配置七个家用门 Mesh Part"),
		Definition->MeshParts.Num(), 7);
	for (int32 PartId = 0; PartId < Definition->MeshParts.Num(); ++PartId)
	{
		TestNotNull(
			FString::Printf(TEXT("Door Mesh Part %d 已绑定 Mesh"), PartId),
			Definition->MeshParts[PartId].Mesh.Get());
		TestNotNull(
			FString::Printf(TEXT("Door Mesh Part %d 已绑定白色材质"), PartId),
			Definition->MeshParts[PartId].MaterialOverride.Get());
		if (Definition->MeshParts[PartId].MaterialOverride)
		{
			TestEqual(
				FString::Printf(TEXT("Door Mesh Part %d 使用统一白色材质"), PartId),
				Definition->MeshParts[PartId].MaterialOverride->GetPathName(),
				FString(TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable")));
			TestTrue(
				FString::Printf(TEXT("Door Mesh Part %d 材质支持实例化渲染"), PartId),
				Definition->MeshParts[PartId].MaterialOverride
					->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes));
		}
	}
	TestTrue(TEXT("Door 使用项目内 CPU 可读立方体"),
		Definition->MeshParts[0].Mesh
			&& Definition->MeshParts[0].Mesh->bAllowCPUAccess);
	TestTrue(TEXT("Door 使用项目内 CPU 可读球体"),
		Definition->MeshParts[6].Mesh
			&& Definition->MeshParts[6].Mesh->bAllowCPUAccess);
	for (const FBuildMeshPartDefinition& Part : Definition->MeshParts)
	{
		TestEqual(TEXT("Door 每个 Part 声明一个 BurnAmount"),
			Part.CustomDataFloatCount, 1);
	}
#if WITH_EDITOR
	const UMaterial* BurnableMaterial = Cast<UMaterial>(
		Definition->MeshParts[0].MaterialOverride.Get());
	TestNotNull(TEXT("统一可燃建筑材质可读取表达式契约"), BurnableMaterial);
	const UMaterialExpressionPerInstanceCustomData* BurnAmountExpression = nullptr;
	if (BurnableMaterial)
	{
		for (const TObjectPtr<UMaterialExpression>& Expression :
			BurnableMaterial->GetExpressionCollection().Expressions)
		{
			const UMaterialExpressionPerInstanceCustomData* Candidate =
				Cast<UMaterialExpressionPerInstanceCustomData>(Expression.Get());
			if (Candidate && Candidate->DataIndex == 0)
			{
				BurnAmountExpression = Candidate;
				break;
			}
		}
	}
	TestNotNull(TEXT("统一可燃建筑材质从 CustomData[0] 读取 BurnAmount"),
		BurnAmountExpression);
	if (BurnAmountExpression)
	{
		TestEqual(TEXT("缺少实例数据时可燃材质默认呈现完全烧焦"),
			BurnAmountExpression->ConstDefaultValue, 1.0f);
	}
#endif
	TestTrue(TEXT("生产 Door 使用统一斧头破坏配置"),
		Definition->Destruction.IsEnabled() && Definition->Destruction.IsValid());
	for (const int32 MovingPartId : {0, 4, 5, 6})
	{
		TestTrue(TEXT("Door 运动 Part 使用可晋升表现策略"),
			Definition->MeshParts[MovingPartId].PresentationPolicy
				== EBuildMeshPartPresentationPolicy::ProximityPromotable);
	}
	for (const int32 FramePartId : {1, 2, 3})
	{
		TestTrue(TEXT("Door 门框使用永久 Static 表现策略"),
			Definition->MeshParts[FramePartId].PresentationPolicy
				== EBuildMeshPartPresentationPolicy::Static);
	}
	TestEqual(TEXT("Door 配置一个门扇和三个门框碰撞代理"),
		Definition->CollisionParts.Num(), 4);
	if (Definition->CollisionParts.Num() == 4)
	{
		const FBuildCollisionPartDefinition& LeafCollision =
			Definition->CollisionParts[0];
		TestEqual(TEXT("门扇碰撞由 Mesh Part 0 驱动"),
			LeafCollision.DrivenMeshPartId, 0);
		TestTrue(TEXT("门扇碰撞使用 Kinematic Mobility"),
			LeafCollision.Mobility == EBuildCollisionMobility::Kinematic);
		TestEqual(TEXT("门扇默认使用 BlockAllDynamic"),
			LeafCollision.GetEffectiveCollisionProfileName(),
			FName(TEXT("BlockAllDynamic")));
		for (int32 CollisionPartId = 1; CollisionPartId < 4; ++CollisionPartId)
		{
			const FBuildCollisionPartDefinition& FrameCollision =
				Definition->CollisionParts[CollisionPartId];
			TestEqual(TEXT("门框碰撞按 PartId 驱动"),
				FrameCollision.DrivenMeshPartId, CollisionPartId);
			TestTrue(TEXT("门框碰撞使用 Static Mobility"),
				FrameCollision.Mobility == EBuildCollisionMobility::Static);
		}
	}
	TestTrue(TEXT("Door 所有碰撞代理都具有 Simple Collision"),
		Definition->HasValidCollisionDefinition());
	TestTrue(TEXT("Door Definition 可以直接作为背包定义"), Item->Initialize(Definition));
	TestTrue(TEXT("ItemInstance 保留 Door Definition 身份"),
		Item->GetDefinition().GetObject() == Definition);
	TestEqual(TEXT("未配置时不会凭空生成 Item Feature"), Item->GetFeatures().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorDefinitionInitializesRuntimeFragmentsTest,
	"ElementSandbox.BuildingCatalog.Door.InitializesRuntimeFragments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorDefinitionInitializesRuntimeFragmentsTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	UDoorBuildingDefinition* Definition = NewObject<UDoorBuildingDefinition>();
	const FTransform InitialWorldTransform(FVector(250.0, -50.0, 20.0));

	const FBuildEntityHandle Entity = Definition->CreateEntity(
		Registry,
		InitialWorldTransform);
	TestTrue(TEXT("Door Definition 创建有效 Building Entity"), Registry.IsAlive(Entity));

	const FBuildTransformFragment* TransformFragment =
		Registry.FindFragment<FBuildTransformFragment>(Entity);
	TestNotNull(TEXT("Door 复用通用 Transform Fragment"), TransformFragment);
	if (TransformFragment)
	{
		TestTrue(TEXT("Door 保存创建时的世界位姿"),
			TransformFragment->WorldTransform.Equals(InitialWorldTransform));
	}

	const FBuildDefinitionFragment* DefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	TestNotNull(TEXT("Door 只复用通用 Definition Fragment"), DefinitionFragment);
	if (DefinitionFragment)
	{
		TestTrue(TEXT("Definition Fragment 直接持有 Door Definition"),
			DefinitionFragment->Definition.Get() == Definition);
		TestEqual(TEXT("Door Entity 共享完整家用门配置"),
			DefinitionFragment->Definition->MeshParts.Num(), 7);
	}

	const FBuildDoorStateFragment* DoorState =
		Registry.FindFragment<FBuildDoorStateFragment>(Entity);
	TestNotNull(TEXT("Door Entity 初始化专属状态 Fragment"), DoorState);
	if (DoorState)
	{
		TestTrue(TEXT("Door 初始状态为关闭"),
			DoorState->State == EBuildDoorState::Closed);
		TestEqual(TEXT("Door 初始没有过渡起始时间"),
			DoorState->TransitionStartServerTimeSeconds,
			0.0);
	}

	int32 BurnCustomDataIndex = INDEX_NONE;
	TestTrue(TEXT("Door 由 Catalog 显式声明燃烧资格"),
		TryGetBuildCombustionConfiguration(*Definition, BurnCustomDataIndex)
			&& BurnCustomDataIndex == 0);
	const FBuildRenderCustomDataFragment* CustomData =
		Registry.FindFragment<FBuildRenderCustomDataFragment>(Entity);
	TestNull(TEXT("Door 稳定 Cold 不分配 Entity CustomData"), CustomData);
	TestNull(TEXT("Door 未受击时不分配 Damage Fragment"),
		Registry.FindFragment<FBuildDamageFragment>(Entity));

	EBuildDoorInteractionIntent BurnedOutIntent = EBuildDoorInteractionIntent::None;
	TestTrue(TEXT("元素表现状态不参与 Door 交互解析"),
		TryResolveBuildDoorInteraction(Registry, Entity, BurnedOutIntent));
	TestTrue(TEXT("关闭门仍解析为 Open"),
		BurnedOutIntent == EBuildDoorInteractionIntent::Open);

	const FBuildPartTransformFragment* PartTransforms =
		Registry.FindFragment<FBuildPartTransformFragment>(Entity);
	TestNotNull(TEXT("Door 初始化每实例 Part Transform"), PartTransforms);
	if (PartTransforms)
	{
		TestEqual(TEXT("Door Part Transform 与共享 Mesh Part 一一对应"),
			PartTransforms->LocalTransforms.Num(), Definition->MeshParts.Num());
		for (int32 PartId = 0; PartId < Definition->MeshParts.Num(); ++PartId)
		{
			TestTrue(
				FString::Printf(TEXT("Door Part %d 初始使用共享局部 Transform"), PartId),
				PartTransforms->LocalTransforms[PartId].Equals(
					Definition->MeshParts[PartId].LocalTransform));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSettlementDoorDefinitionInitializesRuntimeFragmentsTest,
	"ElementSandbox.BuildingCatalog.Door.SettlementCompanionConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementDoorDefinitionInitializesRuntimeFragmentsTest::RunTest(
	const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	UDoorBuildingDefinition* Definition = NewObject<UDoorBuildingDefinition>();
	TestTrue(TEXT("同一 Door Definition 可切换为聚落伴生配置"),
		Definition && Definition->InitializeAsSettlementCompanion());
	if (!Definition)
	{
		return false;
	}
	TestEqual(TEXT("聚落伴生门使用独立稳定 DefinitionId"),
		Definition->DefinitionId, FName(TEXT("Settlement.Door")));
	TestEqual(TEXT("聚落伴生门仍复用原始七个 Mesh Part"),
		Definition->MeshParts.Num(), 7);
	TestTrue(TEXT("聚落伴生门同样配置统一斧头破坏"),
		Definition->Destruction.IsEnabled() && Definition->Destruction.IsValid());

	const FBuildEntityHandle Entity =
		Definition->CreateEntity(Registry, FTransform::Identity);
	TestTrue(TEXT("聚落伴生门仍配置完整开合 Fragment"),
		Registry.HasFragment<FBuildDoorStateFragment>(Entity)
			&& Registry.HasFragment<FBuildPartTransformFragment>(Entity));
	TestFalse(TEXT("聚落伴生门未受击时不分配 Damage Fragment"),
		Registry.HasFragment<FBuildDamageFragment>(Entity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorDefinitionSweptBoundsTest,
	"ElementSandbox.BuildingCatalog.Door.SweptBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorDefinitionSweptBoundsTest::RunTest(const FString& Parameters)
{
	UDoorBuildingDefinition* Definition = NewObject<UDoorBuildingDefinition>();
	TArray<FTransform> PartTransforms;
	PartTransforms.Reserve(Definition->MeshParts.Num());
	for (const FBuildMeshPartDefinition& Part : Definition->MeshParts)
	{
		PartTransforms.Add(Part.LocalTransform);
	}

	const FTransform WorldTransform(
		FRotator(0.0, 37.0, 0.0),
		FVector(250.0, -125.0, 40.0));
	FBox SweptBounds(ForceInit);
	TestTrue(TEXT("Door 能计算保守 Swept AABB"),
		Definition->TryCalculateWorldBounds(
			WorldTransform,
			PartTransforms,
			SweptBounds));

	for (int32 SampleIndex = 0; SampleIndex <= 16; ++SampleIndex)
	{
		const float OpenFraction = static_cast<float>(SampleIndex) / 16.0f;
		const FTransform DoorMotion =
			FBuildDoorProcessor::CalculateDoorMotion(OpenFraction);
		for (const int32 MovingPartId : {0, 4, 5, 6})
		{
			const FBuildMeshPartDefinition& Part =
				Definition->MeshParts[MovingPartId];
			const FTransform PartWorldTransform =
				Part.LocalTransform * DoorMotion * WorldTransform;
			const FBox SampleBounds =
				Part.Mesh->GetBoundingBox().TransformBy(PartWorldTransform);
			TestTrue(
				FString::Printf(
					TEXT("Swept AABB 包含采样 %d 的运动 Part %d"),
					SampleIndex,
					MovingPartId),
				SweptBounds.IsInsideOrOn(SampleBounds));
		}
	}

	const int32 MovingPartIds[] = {0, 4, 5, 6};
	TestFalse(TEXT("保守 Swept Bounds 已覆盖全部 Door 运动 Part"),
		Definition->DoPartTransformChangesAffectSpatialBounds(MovingPartIds));
	const int32 FramePartIds[] = {1};
	TestTrue(TEXT("门框 Transform 变化仍要求重算空间 Bounds"),
		Definition->DoPartTransformChangesAffectSpatialBounds(FramePartIds));
	return true;
}

#endif
