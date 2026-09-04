#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Snapshot/WorldObjectQuerySnapshotStream.h"
#include "Tree/SettlementTreeDefinition.h"
#include "Tree/SettlementTreeTypes.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjects/StickWorldObjectDefinition.h"
#include "WorldObjectWorldSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectProductionShapeRegistrationTest,
	"ElementSandbox.WorldObjectCatalog.ShapeRegistration.ProductionDefinitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectProductionShapeRegistrationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("WorldObjectProductionShapeRegistration"),
		nullptr,
		true);
	if (!TestNotNull(TEXT("创建生产 WorldObject Shape 审计 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!TestNotNull(TEXT("WorldObject Subsystem 可用"), WorldObjects))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	// Production catalog subsystems already own and register these definitions for this
	// World. Reusing those exact objects also exercises the duplicate-id guard instead
	// of accidentally asking CreateEntity to replace a registered definition.
	UStickWorldObjectDefinition* Stick = Cast<UStickWorldObjectDefinition>(
		WorldObjects->FindDefinition(TEXT("Stick")));
	if (!TestNotNull(TEXT("生产 Stick Definition 已由 Catalog 注册"), Stick))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}
	TestTrue(TEXT("Stick Definition 注册显式有效 Shape"), Stick->IsDefinitionValid());
	TestTrue(TEXT("Stick 使用杆体 Capsule，不从拾取 Bounds 反推"),
		Stick->ShapeGeometry.Kind == EWorldObjectShapeKind::Capsule
			&& FMath::IsNearlyEqual(Stick->ShapeGeometry.Radius, 3.0)
			&& FMath::IsNearlyEqual(Stick->ShapeGeometry.CapsuleSegmentHalfLength, 35.0)
			&& Stick->InteractionLocalBounds.GetExtent().Equals(FVector(8.0, 8.0, 38.0)));
	TestFalse(TEXT("Stick 的中性 SurfaceProfile 显式存在"), Stick->SurfaceProfileId.IsNone());

	FWorldObjectCreateDesc StickDesc;
	StickDesc.Definition = Stick;
	const FWorldObjectEntityHandle StickEntity = WorldObjects->CreateEntity(StickDesc);
	TestTrue(TEXT("真实 Stick Definition 创建宿主 Entity"), StickEntity.IsSet());

	USettlementTreeDefinition* Tree = Cast<USettlementTreeDefinition>(
		WorldObjects->FindDefinition(SettlementTreeDefinitionId));
	if (!TestNotNull(TEXT("生产 Tree Definition 已由 Catalog 注册"), Tree))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}
	FWorldObjectCreateDesc TreeDesc;
	TreeDesc.Definition = Tree;
	TreeDesc.WorldTransform.SetLocation(FVector(500.0, 500.0, 0.0));
	const FWorldObjectEntityHandle TreeEntity = WorldObjects->CreateEntity(TreeDesc);
	TestTrue(TEXT("没有 HISM/碰撞 Body 时 Tree 仍注册独立宿主 Shape"),
		Tree->IsDefinitionValid() && TreeEntity.IsSet());

	FWorldObjectShapeInstanceSnapshot StickShape;
	TestTrue(TEXT("中性空间快照流可复制生产 Definition Snapshot"),
		WorldObjects->CopyEntityShapeSnapshot(StickEntity, StickShape));
	TestTrue(TEXT("Stick Snapshot 的 Element Shape 宽度没有被拾取包络放大"),
		StickShape.WorldBounds.GetExtent().Equals(FVector(3.0, 3.0, 38.0))
			&& StickShape.InteractionWorldBounds.GetExtent().Equals(FVector(8.0, 8.0, 38.0)));

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
