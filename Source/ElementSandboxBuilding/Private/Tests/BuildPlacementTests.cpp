#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "BuildingWorldSubsystem.h"

#include "Components/StaticMeshComponent.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Definition/WorldObjectDefinition.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/ConvexElem.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "Placement/BuildPlacementTypes.h"
#include "Tests/BuildEntityTestTypes.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"

namespace ElementSandbox::Building::PlacementTests
{
	enum class ETestSimpleShape : uint8
	{
		Box,
		Sphere,
		Capsule,
		Convex
	};

	struct FPlacementTestWorld final
	{
		FPlacementTestWorld()
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
				TEXT("BuildingPlacementRules"),
				nullptr,
				true,
				ERHIFeatureLevel::Num,
				&InitializationValues,
				true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(NM_Standalone);
			World->InitWorld(InitializationValues);
			World->UpdateWorldComponents(true, false);
			Building = World->GetSubsystem<UBuildingWorldSubsystem>();
			VisualCube = LoadObject<UStaticMesh>(
				nullptr,
				TEXT("/Engine/BasicShapes/Cube.Cube"));
		}

		~FPlacementTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		UStaticMesh* MakeSimpleShapeMesh(
			const ETestSimpleShape Shape,
			const TCHAR* Name,
			const ECollisionTraceFlag TraceFlag = CTF_UseDefault)
		{
			UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(VisualCube, World, Name);
			UBodySetup* BodySetup = NewObject<UBodySetup>(Mesh);
			BodySetup->CollisionTraceFlag = TraceFlag;
			switch (Shape)
			{
			case ETestSimpleShape::Box:
				BodySetup->AggGeom.BoxElems.Add(FKBoxElem(100.0f));
				break;
			case ETestSimpleShape::Sphere:
				BodySetup->AggGeom.SphereElems.Add(FKSphereElem(50.0f));
				break;
			case ETestSimpleShape::Capsule:
				BodySetup->AggGeom.SphylElems.Add(FKSphylElem(25.0f, 50.0f));
				break;
			case ETestSimpleShape::Convex:
			{
				FKConvexElem Convex;
				Convex.ConvexFromBoxElem(FKBoxElem(100.0f));
				BodySetup->AggGeom.ConvexElems.Add(MoveTemp(Convex));
				break;
			}
			default:
				checkNoEntry();
			}
			Mesh->SetBodySetup(BodySetup);
			// 只为测试构造四类已 Cook 代理；正式摆放路径从不执行 Cook。
			BodySetup->CreatePhysicsMeshes();
			ShapeMeshes.Add(Mesh);
			return Mesh;
		}

		UBuildTestDefinition* MakeDefinition(
			const FName DefinitionId,
			UStaticMesh& CollisionMesh,
			const TConstArrayView<FTransform> PartTransforms)
		{
			UBuildTestDefinition* Definition =
				NewObject<UBuildTestDefinition>(World);
			Definition->DefinitionId = DefinitionId;
			for (const FTransform& PartTransform : PartTransforms)
			{
				FBuildMeshPartDefinition& MeshPart =
					Definition->MeshParts.AddDefaulted_GetRef();
				MeshPart.Mesh = VisualCube;
				MeshPart.LocalTransform = PartTransform;

				FBuildCollisionPartDefinition& CollisionPart =
					Definition->CollisionParts.AddDefaulted_GetRef();
				CollisionPart.CollisionMesh = &CollisionMesh;
				CollisionPart.LocalTransform = PartTransform;
			}
			Definitions.Add(Definition);
			return Definition;
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Building = nullptr;
		UStaticMesh* VisualCube = nullptr;
		TArray<TObjectPtr<UStaticMesh>> ShapeMeshes;
		TArray<TObjectPtr<UBuildTestDefinition>> Definitions;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPlacementSimpleShapeCoverageTest,
	"ElementSandbox.Building.Placement.SimpleShapeCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPlacementSimpleShapeCoverageTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::PlacementTests;
	FPlacementTestWorld Harness;
	if (!Harness.Building || !Harness.VisualCube)
	{
		return false;
	}
	const ETestSimpleShape Shapes[] =
	{
		ETestSimpleShape::Box,
		ETestSimpleShape::Sphere,
		ETestSimpleShape::Capsule,
		ETestSimpleShape::Convex
	};
	for (int32 ShapeIndex = 0; ShapeIndex < UE_ARRAY_COUNT(Shapes); ++ShapeIndex)
	{
		UStaticMesh* ShapeMesh = Harness.MakeSimpleShapeMesh(
			Shapes[ShapeIndex],
			*FString::Printf(TEXT("PlacementShapeMesh_%d"), ShapeIndex));
		const FTransform PartTransform = FTransform::Identity;
		UBuildTestDefinition* Definition = Harness.MakeDefinition(
			FName(*FString::Printf(TEXT("Building.Test.PlacementShape.%d"), ShapeIndex)),
			*ShapeMesh,
			TConstArrayView<FTransform>(&PartTransform, 1));
		const FVector Location(ShapeIndex * 1000.0, 0.0, 0.0);
		const FBuildEntityHandle Existing = Harness.Building->CreateEntity(
			*Definition,
			FTransform(Location));
		TestTrue(TEXT("四类 Simple Shape 都能创建 Building"), Existing.IsSet());
		FBuildPlacementEvaluation Evaluation;
		TestTrue(TEXT("四类 Simple Shape 都能执行精确查询"),
			Harness.Building->EvaluatePlacement(
				*Definition,
				FTransform(Location),
				Location,
				500.0,
				Evaluation));
		TestEqual(TEXT("重叠 Simple Shape 被拒绝"),
			Evaluation.Failure,
			EBuildPlacementFailure::BlockedByBuilding);
		TestTrue(TEXT("查询进入无场景 Simple Shape 精确相交"),
			Evaluation.TestedShapePairCount > 0);
		if (ShapeIndex == 0)
		{
			FBuildPlacementEvaluation ScaledEvaluation;
			TestTrue(TEXT("权威摆放评估接受有限正 Scale 的回收构件"),
				Harness.Building->EvaluatePlacement(
					*Definition,
					FTransform(
						FQuat::Identity,
						Location + FVector(300.0, 0.0, 0.0),
						FVector(2.0, 0.5, 1.5)),
					Location + FVector(300.0, 0.0, 0.0),
					500.0,
					ScaledEvaluation));
			TestTrue(TEXT("不与既有构件重叠的缩放候选允许摆放"),
				ScaledEvaluation.IsAllowed());
			FBuildPlacementEvaluation ReusedScaledEvaluation;
			Harness.Building->EvaluatePlacement(
				*Definition,
				FTransform(
					FQuat::Identity,
					Location + FVector(300.0, 0.0, 0.0),
					FVector(2.0, 0.5, 1.5)),
				Location + FVector(300.0, 0.0, 0.0),
				500.0,
				ReusedScaledEvaluation);
			TestTrue(TEXT("重复预览相同尺寸保持相同判定"),
				ReusedScaledEvaluation.IsAllowed());
		}
	}

	UStaticMesh* ComplexMesh = Harness.MakeSimpleShapeMesh(
		ETestSimpleShape::Box,
		TEXT("PlacementComplexAsSimple"),
		CTF_UseComplexAsSimple);
	const FTransform PartTransform = FTransform::Identity;
	UBuildTestDefinition* ComplexDefinition = Harness.MakeDefinition(
		TEXT("Building.Test.PlacementComplexRejected"),
		*ComplexMesh,
		TConstArrayView<FTransform>(&PartTransform, 1));
	TestFalse(TEXT("显示 Mesh 的 Complex-As-Simple 不可进入摆放规则"),
		ComplexDefinition->HasValidCollisionDefinition());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPlacementToleranceAndMultiPartGapTest,
	"ElementSandbox.Building.Placement.ToleranceAndMultiPartGap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPlacementToleranceAndMultiPartGapTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::PlacementTests;
	FPlacementTestWorld Harness;
	if (!Harness.Building || !Harness.VisualCube)
	{
		return false;
	}
	UStaticMesh* BoxMesh = Harness.MakeSimpleShapeMesh(
		ETestSimpleShape::Box,
		TEXT("PlacementToleranceBox"));
	const FTransform FullPart = FTransform::Identity;
	UBuildTestDefinition* FullDefinition = Harness.MakeDefinition(
		TEXT("Building.Test.PlacementTolerance"),
		*BoxMesh,
		TConstArrayView<FTransform>(&FullPart, 1));
	TestTrue(TEXT("创建接触容差基准 Building"),
		Harness.Building->CreateEntity(*FullDefinition, FTransform::Identity).IsSet());

	auto EvaluateAtX = [&Harness, FullDefinition](const double X)
	{
		FBuildPlacementEvaluation Evaluation;
		Harness.Building->EvaluatePlacement(
			*FullDefinition,
			FTransform(FVector(X, 0.0, 0.0)),
			FVector(X, 0.0, 0.0),
			500.0,
			Evaluation,
			0.5);
		return Evaluation;
	};
	TestTrue(TEXT("零穿透接触合法"), EvaluateAtX(100.0).IsAllowed());
	TestTrue(TEXT("0.5cm 内微小穿透按接触处理"), EvaluateAtX(99.75).IsAllowed());
	TestEqual(TEXT("超过 0.5cm 的真实穿透非法"),
		EvaluateAtX(99.25).Failure,
		EBuildPlacementFailure::BlockedByBuilding);

	const FTransform GapParts[] =
	{
		FTransform(FVector(0.0, -100.0, 0.0)),
		FTransform(FVector(0.0, 100.0, 0.0))
	};
	UBuildTestDefinition* GapDefinition = Harness.MakeDefinition(
		TEXT("Building.Test.PlacementGap"),
		*BoxMesh,
		GapParts);
	const FVector GapLocation(1000.0, 0.0, 0.0);
	TestTrue(TEXT("创建双 Part 且中间留空的 Building"),
		Harness.Building->CreateEntity(
			*GapDefinition,
			FTransform(GapLocation)).IsSet());
	const FTransform SmallPart(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(0.5));
	UBuildTestDefinition* SmallDefinition = Harness.MakeDefinition(
		TEXT("Building.Test.PlacementGapCandidate"),
		*BoxMesh,
		TConstArrayView<FTransform>(&SmallPart, 1));
	TestTrue(TEXT("注册空隙候选 Definition"),
		Harness.Building->RegisterDefinition(*SmallDefinition));
	FBuildPlacementEvaluation GapEvaluation;
	Harness.Building->EvaluatePlacement(
		*SmallDefinition,
		FTransform(GapLocation),
		GapLocation,
		500.0,
		GapEvaluation);
	TestTrue(TEXT("总 AABB 相交但 Collision Part 空隙允许摆放"),
		GapEvaluation.IsAllowed());
	TestEqual(TEXT("空隙查询只检查两个 Part Pair"),
		GapEvaluation.TestedPartPairCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPlacementDenseBroadphaseAndWorldBlockerTest,
	"ElementSandbox.Building.Placement.DenseBroadphaseAndWorldBlocker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPlacementDenseBroadphaseAndWorldBlockerTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::PlacementTests;
	FPlacementTestWorld Harness;
	if (!Harness.Building || !Harness.VisualCube)
	{
		return false;
	}
	UStaticMesh* BoxMesh = Harness.MakeSimpleShapeMesh(
		ETestSimpleShape::Box,
		TEXT("PlacementDenseBox"));
	const FTransform Part = FTransform::Identity;
	UBuildTestDefinition* Definition = Harness.MakeDefinition(
		TEXT("Building.Test.PlacementDense"),
		*BoxMesh,
		TConstArrayView<FTransform>(&Part, 1));
	constexpr int32 DenseEntityCount = 500;
	bool bCreatedAll = true;
	for (int32 Index = 0; Index < DenseEntityCount; ++Index)
	{
		const FVector Location(
			10000.0 + (Index % 20) * 200.0,
			10000.0 + (Index / 20) * 200.0,
			0.0);
		bCreatedAll &= Harness.Building->CreateEntity(
			*Definition,
			FTransform(Location)).IsSet();
	}
	TestTrue(TEXT("密集场景创建 500 个 Building"), bCreatedAll);
	const FVector OccupiedLocation(12000.0, 12000.0, 0.0);
	FBuildPlacementEvaluation DenseEvaluation;
	Harness.Building->EvaluatePlacement(
		*Definition,
		FTransform(OccupiedLocation),
		OccupiedLocation,
		500.0,
		DenseEvaluation);
	TestEqual(TEXT("密集场景占用位置被拒绝"),
		DenseEvaluation.Failure,
		EBuildPlacementFailure::BlockedByBuilding);
	TestTrue(TEXT("Broad Phase 只返回邻近实体而非遍历 500 个"),
		DenseEvaluation.CandidateEntityCount < 10);
	TestTrue(TEXT("精确 Shape Pair 数保持常数级"),
		DenseEvaluation.TestedShapePairCount < 10);

	AStaticMeshActor* Blocker = Harness.World->SpawnActor<AStaticMeshActor>();
	const FVector BlockedLocation(30000.0, 0.0, 0.0);
	Blocker->GetStaticMeshComponent()->SetStaticMesh(Harness.VisualCube);
	Blocker->GetStaticMeshComponent()->SetCollisionEnabled(
		ECollisionEnabled::QueryAndPhysics);
	Blocker->GetStaticMeshComponent()->SetCollisionResponseToAllChannels(ECR_Block);
	Blocker->SetActorLocation(BlockedLocation);
	Blocker->GetStaticMeshComponent()->RecreatePhysicsState();
	FBuildPlacementEvaluation WorldEvaluation;
	Harness.Building->EvaluatePlacement(
		*Definition,
		FTransform(BlockedLocation),
		BlockedLocation,
		500.0,
		WorldEvaluation);
	TestEqual(TEXT("WorldStatic Simple Collision 阻挡摆放"),
		WorldEvaluation.Failure,
		EBuildPlacementFailure::BlockedByWorld);

	UWorldObjectWorldSubsystem* WorldObjects =
		Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>();
	TestNotNull(TEXT("摆放规则可以访问 WorldObject 空间索引"), WorldObjects);
	if (WorldObjects)
	{
		UWorldObjectDefinition* WorldObjectDefinition =
			NewObject<UWorldObjectDefinition>(Harness.World);
		WorldObjectDefinition->DefinitionId = TEXT("WorldObject.Test.PlacementBlocker");
		WorldObjectDefinition->SpatialClass =
			EWorldObjectSpatialClass::PermanentStatic;
		WorldObjectDefinition->InteractionLocalBounds = FBox(
			FVector(-50.0),
			FVector(50.0));
		const FVector WorldObjectLocation(32000.0, 0.0, 0.0);
		FWorldObjectCreateDesc Desc;
		Desc.Definition = WorldObjectDefinition;
		Desc.WorldTransform = FTransform(WorldObjectLocation);
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		TestTrue(TEXT("创建无 Actor 的 PermanentStatic WorldObject"),
			WorldObjects->CreateEntity(Desc).IsSet());

		FBuildPlacementEvaluation WorldObjectEvaluation;
		Harness.Building->EvaluatePlacement(
			*Definition,
			FTransform(WorldObjectLocation),
			WorldObjectLocation,
			500.0,
			WorldObjectEvaluation);
		TestEqual(TEXT("纯 ECS WorldObject Bounds 阻挡摆放"),
			WorldObjectEvaluation.Failure,
			EBuildPlacementFailure::BlockedByWorld);
		TestEqual(TEXT("WorldObject 查询只返回相交候选"),
			WorldObjectEvaluation.WorldObjectCandidateCount,
			1);
	}
	return true;
}

#endif
