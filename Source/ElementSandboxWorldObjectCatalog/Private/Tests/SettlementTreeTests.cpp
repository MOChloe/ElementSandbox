#if WITH_DEV_AUTOMATION_TESTS

#include "Tree/SettlementTreeCollisionWorldSubsystem.h"
#include "Tree/SettlementTreeDefinition.h"
#include "Presentation/DeferredHISMComponent.h"
#include "Tree/SettlementTreeMeshFactory.h"
#include "Tree/SettlementTreePresentationWorldSubsystem.h"
#include "Tree/SettlementTreeSelection.h"
#include "Tree/SettlementTreeSettings.h"
#include "Tree/SettlementTreeTypes.h"
#include "Tree/SettlementTreeWorldSubsystem.h"

#include "Async/TaskGraphInterfaces.h"
#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "InstancedStaticMeshDelegates.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "PresentationWorldSubsystem.h"
#include "RenderCommandFence.h"
#include "StaticMeshResources.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeAssetContractTest, "ElementSandbox.WorldObjects.Tree.AssetContract",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeAssetContractTest::RunTest(const FString& Parameters)
{
	USettlementTreeDefinition* Definition = NewObject<USettlementTreeDefinition>(GetTransientPackage());
	TestEqual(TEXT("树 Definition ID 固定"), Definition->DefinitionId, SettlementTreeDefinitionId);
	TestEqual(TEXT("树属于 PermanentStatic"), static_cast<uint8>(Definition->SpatialClass),
				  static_cast<uint8>(EWorldObjectSpatialClass::PermanentStatic));
	TestEqual(TEXT("树保留显式中性木质 Surface 标签"), Definition->SurfaceProfileId,
		FName(TEXT("Surface.WorldObject.TreeWood")));
	TestEqual(TEXT("树 HISM 固定使用颜色与烧黑两项 Custom Data"),
		SettlementTreeCustomDataFloatCount, 2);
	UStaticMesh* Mesh = FSettlementTreeMeshFactory::Create(*GetTransientPackage());
	if (!TestNotNull(TEXT("可生成低多边形组合树"), Mesh) || !Mesh->GetRenderData())
	{
		return false;
	}
	TestEqual(TEXT("树只有一个材质槽"), Mesh->GetStaticMaterials().Num(), 1);
	TestTrue(TEXT("树使用读取 PerInstanceCustomData 的正式材质"),
			 Mesh->GetMaterial(0) && Mesh->GetMaterial(0)->GetPathName() == TEXT("/Game/WorldObjects/Trees/"
																				 "M_SettlementTree.M_SettlementTree"));
	TestTrue(TEXT("树材质必须为完全不透明"),
			 Mesh->GetMaterial(0) && Mesh->GetMaterial(0)->GetBlendMode() == BLEND_Opaque);
	TestTrue(TEXT("树冠内外观察都不得发生单面穿透"), Mesh->GetMaterial(0) && Mesh->GetMaterial(0)->IsTwoSided());
	TestEqual(TEXT("树固定三档 LOD"), Mesh->GetRenderData()->LODResources.Num(), 3);
	if (Mesh->GetRenderData()->LODResources.Num() == 3)
	{
		TestTrue(TEXT("LOD0 不超过 256 三角形"), Mesh->GetRenderData()->LODResources[0].GetNumTriangles() <= 256);
		TestTrue(TEXT("LOD1 不超过 96 三角形"), Mesh->GetRenderData()->LODResources[1].GetNumTriangles() <= 96);
		TestTrue(TEXT("LOD2 不超过 24 三角形"), Mesh->GetRenderData()->LODResources[2].GetNumTriangles() <= 24);
		TestEqual(TEXT("LOD0 固定为短树干加两个封底圆锥"),
				  static_cast<int32>(Mesh->GetRenderData()->LODResources[0].GetNumTriangles()), 40);
		TestEqual(TEXT("LOD1 保持两个封底圆锥"),
				  static_cast<int32>(Mesh->GetRenderData()->LODResources[1].GetNumTriangles()), 32);
		TestEqual(TEXT("LOD2 仍保持两个封底圆锥"),
				  static_cast<int32>(Mesh->GetRenderData()->LODResources[2].GetNumTriangles()), 24);
		TestTrue(TEXT("LOD1 阈值固定为 0.10"),
				 FMath::IsNearlyEqual(Mesh->GetRenderData()->ScreenSize[1].Default, 0.10f));
		TestTrue(TEXT("LOD2 阈值固定为 0.025"),
				 FMath::IsNearlyEqual(Mesh->GetRenderData()->ScreenSize[2].Default, 0.025f));
	}
	const UBodySetup* BodySetup = Mesh->GetBodySetup();
	TestTrue(TEXT("树只带一个简单树干胶囊碰撞"),
			 BodySetup && BodySetup->AggGeom.SphylElems.Num() == 1 && BodySetup->AggGeom.BoxElems.IsEmpty() &&
				 BodySetup->AggGeom.SphereElems.IsEmpty() && BodySetup->AggGeom.ConvexElems.IsEmpty());
	if (BodySetup && BodySetup->AggGeom.SphylElems.Num() == 1)
	{
		const FKSphylElem& Trunk = BodySetup->AggGeom.SphylElems[0];
		TestTrue(TEXT("基础树干碰撞半径约 15cm，正式缩放后约 38–52cm"), FMath::IsNearlyEqual(Trunk.Radius, 15.0f));
		TestTrue(TEXT("树干总高约 260cm"), FMath::IsNearlyEqual(Trunk.Length + Trunk.Radius * 2.0f, 260.0f));
	}

	const float FirstColor = ComputeSettlementTreeColorVariation(FWorldEntityId(29254201));
	const float SameColor = ComputeSettlementTreeColorVariation(FWorldEntityId(29254201));
	const float OtherColor = ComputeSettlementTreeColorVariation(FWorldEntityId(29254202));
	TestEqual(TEXT("颜色变化由 WorldEntityId 稳定推导"), FirstColor, SameColor);
	TestNotEqual(TEXT("不同树可得到不同颜色变化"), FirstColor, OtherColor);
	TestTrue(TEXT("颜色 Custom Data 规范化"), FirstColor >= 0.0f && FirstColor <= 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeCatalogLifecycleTest,
								 "ElementSandbox.WorldObjects.Tree.CatalogLifecycle",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeCatalogLifecycleTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("SettlementTreeCatalogLifecycle"), nullptr, true);
	if (!TestNotNull(TEXT("创建 Tree Catalog 测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	USettlementTreeWorldSubsystem* Catalog = World->GetSubsystem<USettlementTreeWorldSubsystem>();
	UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	bool bResult = TestTrue(TEXT("独立 Tree Catalog 与 WorldObject ECS 均存在"), Catalog && WorldObjects);
	if (Catalog && WorldObjects)
	{
		int32 PublishedBatchCount = 0;
		bool bPublishedBounds = false;
		bool bPublishedEmptyCell = false;
		bool bPublishedIncrementalAdd = false;
		bool bPublishedIncrementalRemove = false;
		bool bPublishedBurnUpdate = false;
		FWorldObjectEntityHandle Entity;
		const FDelegateHandle PublishedHandle = Catalog->OnCellsPublished().AddLambda(
			[&](const TConstArrayView<FSettlementTreeCellChange> Changes)
			{
				++PublishedBatchCount;
				for (const FSettlementTreeCellChange& Change : Changes)
				{
					bPublishedBounds |= Change.Revision > 0 && Change.Snapshot &&
										Change.Snapshot->AggregateBounds.IsValid != 0 && Change.Snapshot->Shards &&
										Change.Snapshot->TreeCount == 1 && Change.Snapshot->Shards->Num() == 1 &&
										(*Change.Snapshot->Shards)[0].Trees &&
										(*Change.Snapshot->Shards)[0].Trees->Num() == 1;
					bPublishedIncrementalAdd |=
						Change.UpsertedTrees && Change.UpsertedTrees->Num() == 1 &&
						Change.UpsertedTrees->ContainsByPredicate([&](const FSettlementTreeCandidate& Tree)
																	  { return Tree.Entity == Entity; });
					bPublishedIncrementalRemove |= Change.RemovedEntities && Change.RemovedEntities->Contains(Entity);
					bPublishedBurnUpdate |= Change.UpsertedTrees
						&& Change.UpsertedTrees->ContainsByPredicate(
							[&](const FSettlementTreeCandidate& Tree)
							{
								return Tree.Entity == Entity && FMath::IsNearlyEqual(Tree.BurnAmount, 0.65f);
							});
					bPublishedEmptyCell |= !Change.Snapshot;
				}
			});
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Catalog->GetDefinition();
		Desc.WorldTransform = FTransform(FRotator(0.0, 37.0, 0.0), FVector(-100001.0, -250001.0, 0.0), FVector(1.05));
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		Entity = WorldObjects->CreateEntity(Desc);
		FSettlementTreeCandidate Candidate;
		bResult &= TestTrue(TEXT("WorldObject 创建事件同步装填 Tree Slot"),
							Entity.IsSet() && Catalog->TryGetTree(Entity, Candidate));
		bResult &= TestTrue(TEXT("负坐标按数学向下取整进入 1km Cell"), Candidate.Cell == FIntPoint(-2, -3));
		bResult &= TestEqual(TEXT("冷树不占用烧黑表现值"), Candidate.BurnAmount, 0.0f);
		bResult &= TestEqual(TEXT("Catalog Resident 计数"), Catalog->GetStats().ResidentTreeCount, 1);
		Catalog->Tick(0.0f);
		bResult &= TestTrue(TEXT("新增树发布 Changed Cell 的不可变 Bounds Snapshot 与本批 Upsert"),
							PublishedBatchCount == 1 && bPublishedBounds && bPublishedIncrementalAdd);
		bResult &= TestTrue(TEXT("Catalog 记录 Cell 发布累计计数"), Catalog->GetStats().CellPublishCount == 1ll);
		bResult &= TestTrue(TEXT("已提交燃烧状态可增量刷新树木烧黑值"),
			Catalog->CommitBurnAmount(Entity, 0.65f));
		Catalog->Tick(0.0f);
		bResult &= TestTrue(TEXT("烧黑更新发布单树 Upsert"), PublishedBatchCount == 2 && bPublishedBurnUpdate);
		bResult &= TestTrue(TEXT("Catalog 当前候选保留烧黑值"),
			Catalog->TryGetTree(Entity, Candidate) && FMath::IsNearlyEqual(Candidate.BurnAmount, 0.65f));
		bResult &= TestTrue(TEXT("GameplayDestroy 从 Tree Slot remove-swap 移除"), WorldObjects->DestroyEntity(Entity));
		bResult &= TestEqual(TEXT("销毁后 Tree Catalog 归零"), Catalog->GetStats().ResidentTreeCount, 0);
		Catalog->Tick(0.0f);
		bResult &= TestTrue(TEXT("空 Cell 同样发布，用于增量删除成员"),
			PublishedBatchCount == 3 && bPublishedEmptyCell && bPublishedIncrementalRemove);
		Catalog->OnCellsPublished().Remove(PublishedHandle);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeIncrementalCellInjectionTest,
								 "ElementSandbox.WorldObjects.Tree.IncrementalCellInjection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeIncrementalCellInjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World =
		UWorld::CreateWorld(EWorldType::Game, false, TEXT("SettlementTreeIncrementalCellInjection"), nullptr, true);
	if (!TestNotNull(TEXT("创建 Tree Cell 增量装填测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	USettlementTreePresentationWorldSubsystem* Trees = World->GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	USettlementTreeWorldSubsystem* Catalog = World->GetSubsystem<USettlementTreeWorldSubsystem>();
	UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	bool bResult = TestTrue(TEXT("增量装填测试依赖可用"), Presentation && Trees && Catalog && WorldObjects);
	if (Presentation && Trees && Catalog && WorldObjects)
	{
		FPresentationViewSource View;
		View.ViewLocation = FVector::ZeroVector;
		View.SubjectLocation = FVector::ZeroVector;
		View.Forward = FVector::ForwardVector;
		View.Right = FVector::RightVector;
		View.Up = FVector::UpVector;
		View.HorizontalFOVDegrees = 90.0f;
		View.AspectRatio = 16.0f / 9.0f;
		View.ViewportSize = FIntPoint(1280, 720);
		View.Revision = 1;
		const FPresentationSourceHandle Source = Presentation->RegisterSource(View);
		const double Deadline = FPlatformTime::Seconds() + 3.0;
		while (!Trees->IsIdle() && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			Trees->Tick(0.01f);
			FPlatformProcess::Sleep(0.01f);
		}
		bResult &= TestTrue(TEXT("空 Catalog 的初始双缓冲已稳定"), Trees->IsIdle());

		const FSettlementTreePresentationStats Before = Trees->GetStats();
		const FSettlementTreeCatalogStats CatalogBefore = Catalog->GetStats();
		constexpr int32 InjectionCount = 64;
		for (int32 Index = 0; Index < InjectionCount; ++Index)
		{
			FWorldObjectCreateDesc Desc;
			Desc.Definition = Catalog->GetDefinition();
			// 同一个 1km Cell 内覆盖 64 个不同 100m Snapshot Shard；每次发布只能
			// 复制本 Shard 的一个新增候选，不能复制此前已装填的全部树。
			Desc.WorldTransform =
				FTransform(FVector(15000.0 + (Index % 8) * 10000.0, 5000.0 + (Index / 8) * 10000.0, 0.0));
			Desc.MotionState = EWorldObjectMotionState::Dormant;
			bResult &= WorldObjects->CreateEntity(Desc).IsSet();
			Catalog->Tick(0.0f);
		}
		const FSettlementTreePresentationStats After = Trees->GetStats();
		const FSettlementTreeCatalogStats CatalogAfter = Catalog->GetStats();
		bResult &= TestEqual(TEXT("同一 1km Cell 的64批注入只测试64个新增候选"),
							 After.CandidateTestCount - Before.CandidateTestCount, static_cast<int64>(InjectionCount));
		bResult &= TestEqual(TEXT("同一 1km Cell 的64批注入只执行64次相关 Cell 增量"),
							 After.CellDeltaEvaluationCount - Before.CellDeltaEvaluationCount,
							 static_cast<int64>(InjectionCount));
		bResult &= TestEqual(TEXT("64个100m Shard 的连续发布只复制64个 Snapshot Candidate"),
							 CatalogAfter.SnapshotCandidateCopyCount - CatalogBefore.SnapshotCandidateCopyCount,
							 static_cast<int64>(InjectionCount));
		bResult &= TestEqual(TEXT("64个100m Shard 各只重建一次"),
							 CatalogAfter.SnapshotShardPublishCount - CatalogBefore.SnapshotShardPublishCount,
							 static_cast<int64>(InjectionCount));
		bResult &= Presentation->UnregisterSource(Source);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeDirectionalSelectionTest,
								 "ElementSandbox.WorldObjects.Tree.DirectionalSelection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeDirectionalSelectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPresentationViewSource Source;
	Source.SubjectLocation = FVector::ZeroVector;
	Source.Forward = FVector::ForwardVector;
	const TArray<FPresentationViewSource> Sources{Source};
	const double LocalRadiusSquared = FMath::Square(100.0);
	const double ForwardDotThreshold = FMath::Cos(FMath::DegreesToRadians(90.0));
	TestTrue(
		TEXT("Local 100m 内无视方向保持 360 度"),
		IsSettlementTreeCandidateDesired(FVector(-99.0, 0.0, 0.0), Sources, LocalRadiusSquared, ForwardDotThreshold));
	TestFalse(
		TEXT("Local 外正后方不进入 180 度 Active"),
		IsSettlementTreeCandidateDesired(FVector(-101.0, 0.0, 0.0), Sources, LocalRadiusSquared, ForwardDotThreshold));
	TestTrue(
		TEXT("Local 外前方进入 180 度 Active"),
		IsSettlementTreeCandidateDesired(FVector(1000.0, 0.0, 0.0), Sources, LocalRadiusSquared, ForwardDotThreshold));
	TestTrue(TEXT("80 度方向仍在 180 度半平面内"),
			 IsSettlementTreeCandidateDesired(FVector(1000.0, 5671.3, 0.0), Sources, LocalRadiusSquared,
											  ForwardDotThreshold));
	TestFalse(TEXT("100 度方向已离开 180 度半平面"),
			  IsSettlementTreeCandidateDesired(FVector(-1000.0, 5671.3, 0.0), Sources, LocalRadiusSquared,
											   ForwardDotThreshold));
	TestTrue(TEXT("一秒转 180 度命中快速甩镜头阈值"),
			 ComputeSettlementTreeHorizontalAngularSpeedDegreesPerSecond(
				 FVector::ForwardVector, -FVector::ForwardVector, 1.0) >= 180.0 - UE_KINDA_SMALL_NUMBER);
	TestTrue(TEXT("快速转向后 200ms 内保持稳定等待"), IsSettlementTreeRapidRotationSettling(10.199, 10.0, 0.2));
	TestFalse(TEXT("200ms 后允许收敛新 Revision"), IsSettlementTreeRapidRotationSettling(10.201, 10.0, 0.2));
	TestTrue(TEXT("90度 FOV 的动态重校准阈值为35度"),
			 FMath::IsNearlyEqual(ComputeSettlementTreeRecenterThresholdDegrees(180.0, 90.0, 10.0, 5.0), 35.0));
	const double LocalCoverage = ComputeSettlementTreeLocalCoverageRadius(10000.0, 2500.0, 10.0);
	TestTrue(TEXT("Local 缓存半径约144m并覆盖25m移动安全区"),
			 LocalCoverage > 14390.0 && LocalCoverage < 14410.0 && LocalCoverage >= 12500.0);
	TestTrue(TEXT("24.99m 主体与相机移动复用覆盖"),
			 IsSettlementTreePositionCoverageReusable(FVector::ZeroVector, FVector::ZeroVector,
													  FVector(2499.0, 0.0, 0.0), FVector(2499.0, 0.0, 0.0), 2500.0));
	TestFalse(TEXT("超过25m只应触发一次新目标"),
			  IsSettlementTreePositionCoverageReusable(FVector::ZeroVector, FVector::ZeroVector,
													   FVector(2501.0, 0.0, 0.0), FVector::ZeroVector, 2500.0));
	const auto FarReusable = [](const FVector& CurrentForward)
	{
		return IsSettlementTreeFarCoverageReusable(FVector::ZeroVector, FVector::ZeroVector, FVector::ForwardVector,
												   90.0f, 16.0f / 9.0f, FIntPoint(1280, 720), FVector::ZeroVector,
												   FVector::ZeroVector, CurrentForward, 90.0f, 16.0f / 9.0f,
												   FIntPoint(1280, 720), 2500.0, 180.0, 10.0, 5.0);
	};
	TestTrue(TEXT("34度水平转动复用 ActiveFar"), FarReusable(FRotator(0.0, 34.0, 0.0).Vector()));
	TestFalse(TEXT("36度水平转动失效一次"), FarReusable(FRotator(0.0, 36.0, 0.0).Vector()));
	TestTrue(TEXT("Pitch 不参与方向失效"), FarReusable(FRotator(45.0, 0.0, 0.0).Vector()));
	TestTrue(TEXT("360度 Resident 覆盖忽略180度镜头转向"),
			 IsSettlementTreeFarCoverageReusable(
				 FVector::ZeroVector, FVector::ZeroVector, FVector::ForwardVector, 90.0f, 16.0f / 9.0f,
				 FIntPoint(1280, 720), FVector::ZeroVector, FVector::ZeroVector, -FVector::ForwardVector, 90.0f,
				 16.0f / 9.0f, FIntPoint(1280, 720), 2500.0, 360.0, 10.0, 5.0));
	TestTrue(TEXT("360度 Resident 覆盖也忽略位置、FOV与窗口变化"),
			 IsSettlementTreeFarCoverageReusable(
				 FVector::ZeroVector, FVector::ZeroVector, FVector::ForwardVector, 90.0f, 16.0f / 9.0f,
				 FIntPoint(1280, 720), FVector(100000.0, -50000.0, 0.0), FVector(80000.0, 30000.0, 0.0),
				 FVector::RightVector, 120.0f, 4.0f / 3.0f, FIntPoint(2560, 1440), 2500.0, 360.0, 10.0, 5.0));
	const FBox BoundaryCrown(FVector(-40.0, 960.0, 0.0), FVector(20.0, 1040.0, 600.0));
	TestTrue(TEXT("树冠 Bounds 跨越180度边界时仍保留"),
			 DoesSettlementTreeBoundsIntersectHorizontalSector(BoundaryCrown, FVector::ZeroVector,
															   FVector::ForwardVector, 90.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeIdleObservationTest, "ElementSandbox.WorldObjects.Tree.IdleObservation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeIdleObservationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("SettlementTreeIdleObservation"), nullptr, true);
	if (!TestNotNull(TEXT("创建行为驱动树表现测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	USettlementTreePresentationWorldSubsystem* Trees = World->GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	bool bResult = TestTrue(TEXT("Presentation 与独立 Tree Presentation 可用"), Presentation && Trees);
	if (Presentation && Trees)
	{
		FPresentationViewSource View;
		View.ViewLocation = FVector::ZeroVector;
		View.SubjectLocation = FVector::ZeroVector;
		View.Forward = FVector::ForwardVector;
		View.Right = FVector::RightVector;
		View.Up = FVector::UpVector;
		View.HorizontalFOVDegrees = 90.0f;
		View.AspectRatio = 16.0f / 9.0f;
		View.ViewportSize = FIntPoint(1280, 720);
		View.Revision = 1;
		const FPresentationSourceHandle Handle = Presentation->RegisterSource(View);
		bResult &= TestTrue(TEXT("注册观察源"), Handle.IsSet());

		const auto PumpUntil = [Trees](const TFunctionRef<bool()> Predicate, const double TimeoutSeconds)
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (!Predicate() && FPlatformTime::Seconds() < Deadline)
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				Trees->Tick(0.01f);
				FPlatformProcess::Sleep(0.01f);
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			return Predicate();
		};
		bResult &= TestTrue(TEXT("初始空 Catalog 选择与0.5秒晋升可稳定"),
							PumpUntil([Trees]() { return Trees->IsIdle(); }, 3.0));
		const FSettlementTreePresentationStats Stable = Trees->GetStats();
		for (int32 Index = 0; Index < 300; ++Index)
		{
			View.Revision = static_cast<uint64>(Index + 2);
			bResult &= Presentation->UpdateSource(Handle, View);
		}
		const FSettlementTreePresentationStats AfterRevisionOnly = Trees->GetStats();
		bResult &= TestEqual(TEXT("连续300帧仅通用 Revision 变化不增加 Local Pass"),
							 AfterRevisionOnly.LocalSelectionPassCount, Stable.LocalSelectionPassCount);
		bResult &= TestEqual(TEXT("连续300帧仅通用 Revision 变化不增加 Far Pass"),
							 AfterRevisionOnly.FarSelectionPassCount, Stable.FarSelectionPassCount);
		bResult &= TestEqual(TEXT("连续300帧仅通用 Revision 变化不派发 Worker"), AfterRevisionOnly.WorkerDispatchCount,
							 Stable.WorkerDispatchCount);
		bResult &= TestEqual(TEXT("连续300帧仅通用 Revision 变化不测试候选"), AfterRevisionOnly.CandidateTestCount,
							 Stable.CandidateTestCount);

		View.SubjectLocation = FVector(2499.0, 0.0, 0.0);
		View.ViewLocation = FVector(2499.0, 0.0, 0.0);
		++View.Revision;
		bResult &= Presentation->UpdateSource(Handle, View);
		bResult &= TestEqual(TEXT("24.99m 移动复用且不派发"), Trees->GetStats().WorkerDispatchCount,
							 Stable.WorkerDispatchCount);
		View.SubjectLocation = FVector::ZeroVector;
		View.ViewLocation = FVector::ZeroVector;
		View.Forward = FRotator(0.0, 34.0, 0.0).Vector();
		FPlatformProcess::Sleep(0.21f);
		++View.Revision;
		bResult &= Presentation->UpdateSource(Handle, View);
		bResult &= TestEqual(TEXT("34度水平转动复用且不派发"), Trees->GetStats().WorkerDispatchCount,
							 Stable.WorkerDispatchCount);
		View.Forward = -FVector::ForwardVector;
		FPlatformProcess::Sleep(0.02f);
		++View.Revision;
		bResult &= Presentation->UpdateSource(Handle, View);
		bResult &= TestEqual(TEXT("360度 Resident 覆盖下180度转向也不派发"), Trees->GetStats().WorkerDispatchCount,
								 Stable.WorkerDispatchCount);
		View.SubjectLocation = FVector(2501.0, 0.0, 0.0);
		View.ViewLocation = FVector(2501.0, 0.0, 0.0);
		++View.Revision;
		bResult &= Presentation->UpdateSource(Handle, View);
		bResult &= TestEqual(TEXT("超过25m移动只派发一次新目标"), Trees->GetStats().WorkerDispatchCount,
								 Stable.WorkerDispatchCount + 1);
		for (int32 Index = 0; Index < 100; ++Index)
		{
			++View.Revision;
			bResult &= Presentation->UpdateSource(Handle, View);
		}
		bResult &= TestEqual(TEXT("相同新目标连续更新不重复派发"), Trees->GetStats().WorkerDispatchCount,
							 Stable.WorkerDispatchCount + 1);
		bResult &= TestTrue(TEXT("新位置目标最终稳定"), PumpUntil([Trees]() { return Trees->IsIdle(); }, 3.0));
		bResult &= Presentation->UnregisterSource(Handle);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeDeferredHISMBuildTest,
									 "ElementSandbox.WorldObjects.Tree.DeferredHISMBuild",
									 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeDeferredHISMBuildTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	bool bResult = true;
	UWorld* World =
		UWorld::CreateWorld(EWorldType::Game, false, TEXT("SettlementTreeDeferredHISMBuild"), nullptr, true);
	if (!TestNotNull(TEXT("创建 HISM 延迟构建测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	AActor* Host = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(Host);
	Host->AddInstanceComponent(Root);
	Host->SetRootComponent(Root);
	Root->RegisterComponent();
	UDeferredHISMComponent* HISM = NewObject<UDeferredHISMComponent>(Host);
	HISM->SetupAttachment(Root);
	HISM->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	HISM->SetNumCustomDataFloats(2);
	Host->AddInstanceComponent(HISM);
	HISM->RegisterComponent();
	int32 TreeBuiltBroadcastCount = 0;
	TUniquePtr<FRenderCommandFence> TreeBuiltFence;
	const FDelegateHandle TreeBuiltHandle =
		FHierarchicalInstancedStaticMeshDelegates::OnTreeBuilt.AddLambda(
			[HISM, &TreeBuiltBroadcastCount, &TreeBuiltFence](
				UHierarchicalInstancedStaticMeshComponent* Component,
				const bool bWasAsyncBuild)
			{
				(void)bWasAsyncBuild;
				if (Component == HISM)
				{
					++TreeBuiltBroadcastCount;
					TreeBuiltFence = MakeUnique<FRenderCommandFence>();
					TreeBuiltFence->BeginFence();
				}
			});
	HISM->BeginBulkEdit();
	HISM->AddInstance(FTransform::Identity);
	HISM->AddInstance(FTransform(FVector(100.0, 0.0, 0.0)));
	HISM->EndBulkEdit(10.0, true);
	bResult &= TestEqual(TEXT("AddInstances 返回时尚未广播 Cluster Tree 完成"),
		TreeBuiltBroadcastCount, 0);
	bResult &= TestFalse(TEXT("静默 250ms 前不建 Cluster Tree"),
		HISM->TryStartDeferredTreeBuild(10.1, 0.25, 1.0));
	HISM->BeginBulkEdit();
	HISM->AddInstance(FTransform(FVector(200.0, 0.0, 0.0)));
	HISM->EndBulkEdit(10.15, true);
	bResult &= TestFalse(TEXT("连续编辑会重置静默时刻"),
		HISM->TryStartDeferredTreeBuild(10.39, 0.25, 1.0));
	bResult &= TestTrue(TEXT("静默期满后只启动一次异步 BuildTree"),
		HISM->TryStartDeferredTreeBuild(10.41, 0.25, 1.0));
	bResult &= TestEqual(TEXT("多次编辑合并为一次 BuildTree"), HISM->GetTreeBuildCount(), 1ull);
	bResult &= TestTrue(TEXT("记录被合并的 BuildTree 请求"), HISM->GetCoalescedTreeBuildCount() > 0);
	bResult &= TestFalse(TEXT("未 BeginPlay 的首次 Cluster Tree 按 UE 规则同步建立"), HISM->IsAsyncBuilding());
	bResult &= TestEqual(TEXT("同步 Cluster Tree 只广播一次完成通知"),
		TreeBuiltBroadcastCount, 1);
	bResult &= TestNotNull(TEXT("完成通知会建立 RenderThread Fence"), TreeBuiltFence.Get());
	if (TreeBuiltFence)
	{
		TreeBuiltFence->Wait();
		bResult &= TestTrue(TEXT("同步树对应的 RenderThread Fence 可完成"),
			TreeBuiltFence->IsFenceComplete());
	}

	// 首次同步树建立后再追加实例，此时 UE 会按正常 Runtime 路径异步重建。
	HISM->BeginBulkEdit();
	HISM->AddInstance(FTransform(FVector(300.0, 0.0, 0.0)));
	HISM->EndBulkEdit(10.42, true);
	bResult &= TestTrue(TEXT("已有基线树后的编辑启动异步 BuildTree"),
		HISM->TryStartDeferredTreeBuild(10.68, 0.25, 1.0));
	bResult &= TestTrue(TEXT("第二轮 BuildTree 正在后台执行"), HISM->IsAsyncBuilding());
	bResult &= TestEqual(TEXT("异步 Cluster Tree 在途时不提前广播新树完成"),
		TreeBuiltBroadcastCount, 1);
	HISM->BeginBulkEdit();
	HISM->AddInstance(FTransform(FVector(400.0, 0.0, 0.0)));
	HISM->EndBulkEdit(10.70, true);
	bResult &= TestTrue(TEXT("异步建树期间的新增先记录为延迟请求"), HISM->HasPendingTreeBuild());

	// 精确复现崩溃前的竞态：Builder 已复制旧数组后，表现层又更新 Custom Data。
	// UE 会丢弃旧结果并直接再次调用虚函数 BuildTreeAsync；该内部重试不能被
	// 外层静默预算吞掉，否则 Cluster Tree 与实例数据会永久失配。
	const float UpdatedCustomData[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
	bResult &= TestTrue(TEXT("异步建树期间接受连续 Custom Data 更新"),
		HISM->SetCustomDataRange(0, 3, MakeArrayView(UpdatedCustomData)));
	const double BuildDeadline = FPlatformTime::Seconds() + 5.0;
	while ((HISM->IsAsyncBuilding() || HISM->GetTreeBuildCount() < 3)
		&& FPlatformTime::Seconds() < BuildDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		// HISM 后台任务通过 ExecuteOnGameThread 投递到 CoreTicker；同步 Automation
		// RunTest 会阻塞常规 Engine Tick，因此测试需显式推进该完成队列。
		FTSTicker::GetCoreTicker().Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	bResult &= TestFalse(TEXT("并发编辑后的内部重试最终完成"), HISM->IsAsyncBuilding());
	bResult &= TestTrue(TEXT("重试完成后 Cluster Tree 与最新实例数据一致"), HISM->IsTreeFullyBuilt());
	bResult &= TestEqual(TEXT("UE 内部重试未被外层预算门禁吞掉"), HISM->GetTreeBuildCount(), 3ull);
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(UpdatedCustomData); ++Index)
	{
		bResult &= TestEqual(TEXT("重试保留最新 Custom Data"),
			HISM->PerInstanceSMCustomData[Index], UpdatedCustomData[Index]);
	}
	HISM->NotifyAsyncBuildObservedComplete();
	bResult &= TestFalse(TEXT("内部重试覆盖最新实例后清除重复外层请求"), HISM->HasPendingTreeBuild());
	bResult &= TestEqual(TEXT("内部重试只为最终一致的 Cluster Tree 广播完成"),
		TreeBuiltBroadcastCount, 2);
	bResult &= TestNotNull(TEXT("最终树完成通知会替换为最新 RenderThread Fence"),
		TreeBuiltFence.Get());
	if (TreeBuiltFence)
	{
		TreeBuiltFence->Wait();
		bResult &= TestTrue(TEXT("最终树对应的 RenderThread Fence 可完成"),
			TreeBuiltFence->IsFenceComplete());
	}
	FHierarchicalInstancedStaticMeshDelegates::OnTreeBuilt.Remove(TreeBuiltHandle);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeInterleavedRemoveSwapTest,
								 "ElementSandbox.WorldObjects.Tree.InterleavedRemoveSwap",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeInterleavedRemoveSwapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World =
		UWorld::CreateWorld(EWorldType::Game, false, TEXT("SettlementTreeInterleavedRemoveSwap"), nullptr, true);
	if (!TestNotNull(TEXT("创建 Tree Remove-Swap 测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	USettlementTreePresentationWorldSubsystem* Trees = World->GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	USettlementTreeWorldSubsystem* Catalog = World->GetSubsystem<USettlementTreeWorldSubsystem>();
	UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	bool bResult = TestTrue(TEXT("Remove-Swap 测试依赖可用"), Presentation && Trees && Catalog && WorldObjects);
	if (Presentation && Trees && Catalog && WorldObjects)
	{
		const FVector Locations[] = {
			FVector(50000.0, -1000.0, 0.0), FVector(150000.0, -1000.0, 0.0), FVector(51000.0, 0.0, 0.0),
			FVector(151000.0, 0.0, 0.0),	FVector(52000.0, 1000.0, 0.0),	 FVector(152000.0, 1000.0, 0.0),
		};
		TArray<FWorldObjectEntityHandle> Entities;
		for (const FVector& Location : Locations)
		{
			FWorldObjectCreateDesc Desc;
			Desc.Definition = Catalog->GetDefinition();
			Desc.WorldTransform = FTransform(Location);
			Desc.MotionState = EWorldObjectMotionState::Dormant;
			Entities.Add(WorldObjects->CreateEntity(Desc));
		}
		Catalog->Tick(0.0f);

		FPresentationViewSource View;
		View.ViewLocation = FVector::ZeroVector;
		View.SubjectLocation = FVector::ZeroVector;
		View.Forward = FVector::ForwardVector;
		View.Right = FVector::RightVector;
		View.Up = FVector::UpVector;
		View.HorizontalFOVDegrees = 90.0f;
		View.AspectRatio = 16.0f / 9.0f;
		View.ViewportSize = FIntPoint(1280, 720);
		View.Revision = 1;
		const FPresentationSourceHandle Source = Presentation->RegisterSource(View);
		const auto PumpUntil = [Trees](const TFunctionRef<bool()> Predicate, const double TimeoutSeconds)
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (!Predicate() && FPlatformTime::Seconds() < Deadline)
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				Trees->Tick(0.01f);
				FPlatformProcess::Sleep(0.01f);
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			return Predicate();
		};
		bResult &=
			TestTrue(TEXT("六棵树跨两个 Cell 完整实例化"),
					 PumpUntil([Trees]() { return Trees->GetStats().InstanceCount == 6 && Trees->IsIdle(); }, 5.0));
		FString MappingError;
		const bool bInitialMappingValid = Trees->ValidateRenderMappingsForAutomation(MappingError);
		bResult &= TestTrue(TEXT("初始双向 Owner/Index/Transform 映射正确"), bInitialMappingValid);
		if (!bInitialMappingValid)
		{
			AddError(MappingError);
		}

		const FSettlementTreePresentationStats BeforeBurn = Trees->GetStats();
		bResult &= TestTrue(TEXT("已实例化树接受烧黑值更新"), Catalog->CommitBurnAmount(Entities.Last(), 1.0f));
		Catalog->Tick(0.0f);
		bResult &= TestTrue(
			TEXT("烧黑值经增量队列写入已有 HISM 实例"),
			PumpUntil(
				[Trees, ExpectedUpdates = BeforeBurn.HISMCustomDataUpdateCount + 1]()
				{
					return Trees->GetStats().HISMCustomDataUpdateCount == ExpectedUpdates && Trees->IsIdle();
				},
				3.0));
		const FSettlementTreePresentationStats AfterBurn = Trees->GetStats();
		bResult &= TestEqual(TEXT("单树烧黑只更新一次 Custom Data"),
			AfterBurn.HISMCustomDataUpdateCount - BeforeBurn.HISMCustomDataUpdateCount, 1ll);
		bResult &= TestEqual(TEXT("烧黑不重建 HISM 实例"), AfterBurn.HISMAddCount, BeforeBurn.HISMAddCount);
		bResult &= TestEqual(TEXT("烧黑不移除 HISM 实例"), AfterBurn.HISMRemoveCount, BeforeBurn.HISMRemoveCount);

		// 按 Cell A/B/A/B 交错进入同一 PendingRemove 队列；每个 Cell
		// 最后一棵必须保留。
		const int32 RemovedIndices[] = {0, 1, 2, 3};
		for (const int32 Index : RemovedIndices)
		{
			bResult &= TestTrue(TEXT("交错 GameplayDestroy 成功"), WorldObjects->DestroyEntity(Entities[Index]));
		}
		Catalog->Tick(0.0f);
		bResult &= TestTrue(TEXT("交错 Remove-Swap 后只剩两棵树"),
							PumpUntil(
								[Trees]()
								{
									const FSettlementTreePresentationStats Stats = Trees->GetStats();
									return Stats.InstanceCount == 2 && Stats.PendingCount == 0;
								},
								5.0));
		MappingError.Reset();
		const bool bFinalMappingValid = Trees->ValidateRenderMappingsForAutomation(MappingError);
		bResult &= TestTrue(TEXT("交错 Remove-Swap 后映射仍正确"), bFinalMappingValid);
		if (!bFinalMappingValid)
		{
			AddError(MappingError);
		}
		bResult &= TestEqual(TEXT("只提交四次真实 HISM Remove"), Trees->GetStats().HISMRemoveCount, 4ll);
		bResult &=
			TestEqual(TEXT("合法 Catalog 删除不得计作可见选择误删"), Trees->GetStats().InvalidVisibleRemovalCount, 0ll);
		bResult &= Presentation->UnregisterSource(Source);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeGenerationReuseRetirementTest,
									 "ElementSandbox.WorldObjects.Tree.GenerationReuseRetirement",
									 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeGenerationReuseRetirementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game, false, TEXT("SettlementTreeGenerationReuseRetirement"), nullptr, true);
	if (!TestNotNull(TEXT("创建 Tree Generation 复用测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	USettlementTreePresentationWorldSubsystem* Trees = World->GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	USettlementTreeWorldSubsystem* Catalog = World->GetSubsystem<USettlementTreeWorldSubsystem>();
	USettlementTreeCollisionWorldSubsystem* Collision = World->GetSubsystem<USettlementTreeCollisionWorldSubsystem>();
	UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	bool bResult = TestTrue(
		TEXT("Generation 复用测试依赖可用"),
		Presentation && Trees && Catalog && Collision && WorldObjects);
	USettlementTreeSettings* Settings = GetMutableDefault<USettlementTreeSettings>();
	const double SavedCollisionGrace = Settings->CollisionGraceSeconds;
	Settings->CollisionGraceSeconds = 0.0;
	if (Presentation && Trees && Catalog && Collision && WorldObjects)
	{
		const FVector TreeLocation(1000.0, 0.0, 0.0);
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Catalog->GetDefinition();
		Desc.WorldTransform = FTransform(TreeLocation);
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		const FWorldObjectEntityHandle Original = WorldObjects->CreateEntity(Desc);
		bResult &= TestTrue(TEXT("创建旧 Generation 树"), Original.IsSet());
		Catalog->Tick(0.0f);

		FPresentationViewSource View;
		View.ViewLocation = FVector::ZeroVector;
		View.SubjectLocation = FVector::ZeroVector;
		View.Forward = FVector::ForwardVector;
		View.Right = FVector::RightVector;
		View.Up = FVector::UpVector;
		View.HorizontalFOVDegrees = 90.0f;
		View.AspectRatio = 16.0f / 9.0f;
		View.ViewportSize = FIntPoint(1280, 720);
		View.Revision = 1;
		const FPresentationSourceHandle PresentationSource = Presentation->RegisterSource(View);

		FSettlementTreeCollisionSource CollisionView;
		CollisionView.SubjectLocation = TreeLocation;
		CollisionView.ViewLocation = TreeLocation;
		CollisionView.ViewDirection = FVector::ForwardVector;
		CollisionView.ImmediateBounds = FBox(TreeLocation - FVector(500.0), TreeLocation + FVector(500.0));
		CollisionView.PrefetchBounds = CollisionView.ImmediateBounds;
		CollisionView.RetentionBounds = CollisionView.ImmediateBounds;
		CollisionView.Revision = 1;
		const FSettlementTreeCollisionSourceHandle CollisionSource = Collision->RegisterSource(CollisionView);
		Collision->FlushImmediateCollisionChanges();

		const auto PumpUntil = [Trees, Collision](const TFunctionRef<bool()> Predicate, const double TimeoutSeconds)
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (!Predicate() && FPlatformTime::Seconds() < Deadline)
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				Trees->Tick(0.01f);
				Collision->Tick(0.01f);
				FPlatformProcess::Sleep(0.01f);
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			return Predicate();
		};
		bResult &= TestTrue(
			TEXT("旧 Generation 树已建立 HISM 与碰撞实例"),
			PumpUntil(
				[Trees, Collision]()
				{
					const FSettlementTreePresentationStats TreeStats = Trees->GetStats();
					const FSettlementTreeCollisionStats CollisionStats = Collision->GetStats();
					return TreeStats.InstanceCount == 1 && TreeStats.PendingCount == 0
						&& CollisionStats.CollisionInstanceCount == 1
						&& CollisionStats.PendingAddCount == 0 && CollisionStats.PendingRemoveCount == 0;
				},
				5.0));

		bResult &= TestTrue(TEXT("旧树 GameplayDestroy 成功"), WorldObjects->DestroyEntity(Original));
		const FWorldObjectEntityHandle Replacement = WorldObjects->CreateEntity(Desc);
		bResult &= TestTrue(TEXT("立即创建替代树"), Replacement.IsSet());
		bResult &= TestEqual(TEXT("替代树立即复用相同数字 Slot"), Replacement.GetSlot(), Original.GetSlot());
		bResult &= TestNotEqual(
			TEXT("替代树使用新 Generation"), Replacement.GetGeneration(), Original.GetGeneration());

		// 此次发布会让新 Generation 入场，而旧 Generation 的预算化 HISM/Collision
		// Remove 尚未执行，曾在 Presentation::Acquire 中触发断言。
		Catalog->Tick(0.0f);
		bResult &= TestTrue(
			TEXT("新旧 Generation 重叠退场后只保留替代树实例"),
			PumpUntil(
				[Trees, Collision]()
				{
					const FSettlementTreePresentationStats TreeStats = Trees->GetStats();
					const FSettlementTreeCollisionStats CollisionStats = Collision->GetStats();
					return TreeStats.InstanceCount == 1 && TreeStats.PendingCount == 0
						&& CollisionStats.CollisionInstanceCount == 1
						&& CollisionStats.PendingAddCount == 0 && CollisionStats.PendingRemoveCount == 0;
				},
				5.0));
		bResult &= TestEqual(TEXT("旧 HISM 实例只移除一次"), Trees->GetStats().HISMRemoveCount, 1ll);
		bResult &= TestEqual(TEXT("替代树只新增一个 HISM 实例"), Trees->GetStats().HISMAddCount, 2ll);
		FString MappingError;
		const bool bMappingValid = Trees->ValidateRenderMappingsForAutomation(MappingError);
		bResult &= TestTrue(TEXT("Generation 复用后双向 HISM 映射正确"), bMappingValid);
		if (!bMappingValid)
		{
			AddError(MappingError);
		}
		bResult &= Presentation->UnregisterSource(PresentationSource);
		bResult &= Collision->UnregisterSource(CollisionSource);
	}
	Settings->CollisionGraceSeconds = SavedCollisionGrace;
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettlementTreeCollisionBudgetTest, "ElementSandbox.WorldObjects.Tree.CollisionBudget",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeCollisionBudgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("SettlementTreeCollisionBudget"), nullptr, true);
	if (!TestNotNull(TEXT("创建树碰撞测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	USettlementTreeWorldSubsystem* Catalog = World->GetSubsystem<USettlementTreeWorldSubsystem>();
	USettlementTreeCollisionWorldSubsystem* Collision = World->GetSubsystem<USettlementTreeCollisionWorldSubsystem>();
	UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	bool bResult = TestTrue(TEXT("独立 Tree Collision 与 Catalog 可用"), Catalog && Collision && WorldObjects);
	USettlementTreeSettings* Settings = GetMutableDefault<USettlementTreeSettings>();
	const double SavedGrace = Settings->CollisionGraceSeconds;
	const int32 SavedAdds = Settings->PredictiveAddsPerFrame;
	const int32 SavedRemoves = Settings->RemovesPerFrame;
	Settings->CollisionGraceSeconds = 0.0;
	Settings->PredictiveAddsPerFrame = 16;
	Settings->RemovesPerFrame = 32;
	if (Catalog && Collision && WorldObjects)
	{
		for (int32 Index = 0; Index < 41; ++Index)
		{
			FWorldObjectCreateDesc Desc;
			Desc.Definition = Catalog->GetDefinition();
			Desc.WorldTransform = FTransform(FVector(Index == 0 ? 0.0 : 1000.0 + Index * 100.0, 0.0, 0.0));
			Desc.MotionState = EWorldObjectMotionState::Dormant;
			bResult &= TestTrue(TEXT("创建碰撞测试树"), WorldObjects->CreateEntity(Desc).IsSet());
		}
		Catalog->Tick(0.0f);
		FSettlementTreeCollisionSource Source;
		Source.SubjectLocation = FVector::ZeroVector;
		Source.ViewLocation = FVector::ZeroVector;
		Source.ViewDirection = FVector::ForwardVector;
		Source.ImmediateBounds = FBox(FVector(-500.0, -500.0, -100.0), FVector(500.0, 500.0, 1000.0));
		Source.PrefetchBounds = FBox(FVector(-500.0, -500.0, -100.0), FVector(10000.0, 500.0, 1000.0));
		Source.RetentionBounds = Source.PrefetchBounds.ExpandBy(300.0);
		Source.Revision = 1;
		const FSettlementTreeCollisionSourceHandle Handle = Collision->RegisterSource(Source);
		bResult &= TestTrue(TEXT("注册独立树碰撞 Source"), Handle.IsSet());
		Collision->FlushImmediateCollisionChanges();
		bResult &=
			TestEqual(TEXT("4m 内树干立即建立，不等待预测帧预算"), Collision->GetStats().CollisionInstanceCount, 1);
		bResult &= TestEqual(TEXT("Dirty Source 只执行一次 Retention Catalog Query"),
							 Collision->GetStats().CatalogQueryCount, 1ll);
		bResult &=
			TestEqual(TEXT("单次查询结果统一分类且每棵候选只测试一次"), Collision->GetStats().CandidateTestCount, 41ll);
		Collision->Tick(1.0f / 60.0f);
		bResult &= TestEqual(TEXT("预测新增严格限制为每帧 16"), Collision->GetStats().CollisionInstanceCount, 17);
		const int64 StableQueryCount = Collision->GetStats().CatalogQueryCount;
		const int64 StableSubmitCount = Collision->GetStats().SourceSubmitCount;
		++Source.Revision;
		bResult &=
			TestTrue(TEXT("只改变通用 Revision 的相同 Source 可被接受"), Collision->UpdateSource(Handle, Source));
		Collision->Tick(1.0f / 60.0f);
		bResult &= TestEqual(TEXT("相同 Source 不产生 Catalog Query"), Collision->GetStats().CatalogQueryCount,
							 StableQueryCount);
		bResult &=
			TestEqual(TEXT("相同 Source 不计为行为提交"), Collision->GetStats().SourceSubmitCount, StableSubmitCount);
		FWorldObjectCreateDesc FarSameCell;
		FarSameCell.Definition = Catalog->GetDefinition();
		FarSameCell.WorldTransform = FTransform(FVector(90000.0, 90000.0, 0.0));
		FarSameCell.MotionState = EWorldObjectMotionState::Dormant;
		bResult &=
			TestTrue(TEXT("在同一 1km Cell 的 Retention 外注入远树"), WorldObjects->CreateEntity(FarSameCell).IsSet());
		Catalog->Tick(0.0f);
		Collision->Tick(1.0f / 60.0f);
		bResult &= TestEqual(TEXT("同 Cell 远处内容变化不重复查询近场碰撞"), Collision->GetStats().CatalogQueryCount,
							 StableQueryCount);
		Source.SubjectLocation = FVector(1000000.0, 0.0, 0.0);
		Source.ImmediateBounds = FBox(Source.SubjectLocation - FVector(100.0), Source.SubjectLocation + FVector(100.0));
		Source.PrefetchBounds = Source.ImmediateBounds;
		Source.RetentionBounds = Source.ImmediateBounds;
		++Source.Revision;
		bResult &= TestTrue(TEXT("Source Revision 可更新"), Collision->UpdateSource(Handle, Source));
		Collision->Tick(1.0f / 60.0f);
		Collision->Tick(1.0f / 60.0f);
		bResult &= TestEqual(TEXT("Retention 外且 Grace 到期后按移除预算归零"),
							 Collision->GetStats().CollisionInstanceCount, 0);
		bResult &= TestEqual(TEXT("阈值越界只增加一次 Catalog Query"), Collision->GetStats().CatalogQueryCount,
							 StableQueryCount + 1);
		bResult &= TestTrue(TEXT("Source 可注销"), Collision->UnregisterSource(Handle));
		bResult &= TestFalse(TEXT("旧 Generation Handle 不可再次更新"), Collision->UpdateSource(Handle, Source));
	}
	Settings->CollisionGraceSeconds = SavedGrace;
	Settings->PredictiveAddsPerFrame = SavedAdds;
	Settings->RemovesPerFrame = SavedRemoves;
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bResult;
}

#endif
