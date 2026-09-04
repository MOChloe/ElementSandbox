#if WITH_DEV_AUTOMATION_TESTS

#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildWorldIdentityFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MeshPoolRenderHost.h"
#include "PresentationSettings.h"
#include "PresentationWorldSubsystem.h"
#include "Rendering/BuildPresentationIndex.h"
#include "Rendering/BuildRenderDirtySet.h"
#include "Rendering/BuildRenderProcessor.h"
#include "Tests/BuildEntityTestTypes.h"

namespace ElementSandbox::Building::Tests
{
	struct FBuildPresentationTestWorld final
	{
		FBuildPresentationTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
			if (Presentation)
			{
				Layer = Presentation->RegisterMeshLayer(TEXT("BuildingRenderTest"));
			}
		}

		~FBuildPresentationTestWorld()
		{
			if (Presentation && Layer.IsSet())
			{
				Presentation->UnregisterMeshLayer(Layer);
			}
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* World = nullptr;
		UPresentationWorldSubsystem* Presentation = nullptr;
		FMeshPoolLayerHandle Layer;
	};

	UBuildTestDefinition* MakePresentationDefinition(
		UObject* Outer,
		UStaticMesh& Mesh,
		const EBuildMeshPartPresentationPolicy Policy =
			EBuildMeshPartPresentationPolicy::Static)
	{
		UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>(Outer);
		FBuildMeshPartDefinition Part;
		Part.Mesh = &Mesh;
		Part.PresentationPolicy = Policy;
		Definition->MeshParts.Add(Part);
		return Definition;
	}

	FBuildEntityHandle AddStaticEntity(
		FBuildEntityRegistry& Registry,
		UBuildTestDefinition& Definition,
		const FVector& Location,
		const uint64 LayoutIndex)
	{
		const FBuildEntityHandle Entity =
			Definition.CreateEntity(Registry, FTransform(Location));
		FBuildWorldIdentityFragment Identity;
		Identity.WorldEntityId = FWorldEntityId(LayoutIndex + 1);
		return Entity.IsSet() && Registry.AddFragment(Entity, Identity)
			? Entity
			: FBuildEntityHandle();
	}

		FPresentationViewSource MakeView(
		const FVector& Location,
		const FVector& Forward,
		const uint64 Revision = 1)
	{
		FPresentationViewSource View;
		View.ViewLocation = Location;
		View.SubjectLocation = Location;
		View.Forward = Forward.GetSafeNormal();
		View.Right = FVector::CrossProduct(FVector::UpVector, View.Forward).GetSafeNormal();
		View.Up = FVector::CrossProduct(View.Forward, View.Right).GetSafeNormal();
		View.HorizontalFOVDegrees = 60.0f;
		View.AspectRatio = 16.0f / 9.0f;
		View.ViewportSize = FIntPoint(1920, 1080);
		View.Revision = Revision;
			return View;
		}

		bool MarkPackedStaticCreated(
				FBuildEntityRegistry& Registry,
				FBuildRenderDirtySet& Dirty,
				const UBuildTestDefinition& Definition,
				const FBuildEntityHandle Entity)
			{
			(void)Registry;
			(void)Definition;
			return Dirty.MarkRebuild(Entity, true);
			}
	}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationMultiSourceResidencyTest,
	"ElementSandbox.Building.Presentation.MultiSourceUnionAndOffscreenExclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationMultiSourceResidencyTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Presentation Subsystem"), Harness.Presentation)
		|| !TestTrue(TEXT("MeshPool Layer"), Harness.Layer.IsSet())
		|| !TestNotNull(TEXT("测试 Mesh"), Mesh))
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 6;
	Config.ForwardCoverageAngleDegrees = 180.0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle Center = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 0);
	const FBuildEntityHandle East = AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 1);
	const FBuildEntityHandle North = AddStaticEntity(Registry, *Definition, FVector(0.0, 10000.0, 0.0), 2);
	const FBuildEntityHandle West = AddStaticEntity(Registry, *Definition, FVector(-20000.0, 0.0, 0.0), 3);
	TestTrue(TEXT("创建四栋静态基线建筑"),
		Center.IsSet() && East.IsSet() && North.IsSet() && West.IsSet());
	Dirty.MarkRebuild(Center);
	Dirty.MarkRebuild(East);
	Dirty.MarkRebuild(North);
	Dirty.MarkRebuild(West);
	TestTrue(TEXT("构建轻量表现索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::RightVector));
	Views.Revision = 1;
	TestTrue(TEXT("多 Source 取并集"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("批量提交驻留结果"), Harness.Presentation->FlushNow());
	TestEqual(TEXT("两个 Source 各取独立预算后并集包含共享本地建筑"), Processor.GetRenderedEntityCount(), 3);

	FMeshPoolInstanceHandle Instance;
	TestTrue(TEXT("共享本地建筑分配驻留句柄"), Processor.TryGetInstanceHandle(Center, 0, Instance));
	TestTrue(TEXT("东侧建筑分配驻留句柄"), Processor.TryGetInstanceHandle(East, 0, Instance));
	TestTrue(TEXT("北侧建筑分配驻留句柄"), Processor.TryGetInstanceHandle(North, 0, Instance));
	TestFalse(TEXT("视锥外建筑不分配句柄"), Processor.TryGetInstanceHandle(West, 0, Instance));
	const FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestTrue(TEXT("Packed BVH 执行节点优先遍历"), Stats.CandidateNodeCount > 0);
	const FMeshPoolStats PoolBeforeNoOpProjection = Harness.Presentation->GetMeshPoolStats();
	Views.Sources[0].Priority = 1;
	Views.Revision = 2;
	TestTrue(TEXT("Source 优先级变化会重选同一驻留集合"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("重选后的空差量可以提交"), Harness.Presentation->FlushNow());
	const FMeshPoolStats PoolAfterNoOpProjection = Harness.Presentation->GetMeshPoolStats();
	TestEqual(TEXT("驻留集合与 Backend 未变时不重复触碰实例"),
		PoolAfterNoOpProjection.TotalFlushedInstanceCount,
		PoolBeforeNoOpProjection.TotalFlushedInstanceCount);
	TestEqual(TEXT("空差量重选不重复触发 HISM Tree"),
		PoolAfterNoOpProjection.HierarchicalTreeBuildRequests,
		PoolBeforeNoOpProjection.HierarchicalTreeBuildRequests);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationMutableChunkCacheTest,
	"ElementSandbox.Building.Presentation.MutableChunkCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationMutableChunkCacheTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.GameplayChunkSize = 5000.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(
		Harness.World,
		*Mesh,
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	const FBuildEntityHandle A = Definition->CreateEntity(
		Registry, FTransform(FVector(10000.0, 0.0, 0.0)));
	const FBuildEntityHandle B = Definition->CreateEntity(
		Registry, FTransform(FVector(12000.0, 0.0, 0.0)));
	const FBuildEntityHandle C = Definition->CreateEntity(
		Registry, FTransform(FVector(17000.0, 0.0, 0.0)));
	Dirty.MarkRebuild(A, false);
	Dirty.MarkRebuild(B, false);
	Dirty.MarkRebuild(C, false);
	TestTrue(TEXT("三栋 Movable Building 写入动态表现 Chunk"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("同一 50m 动态 Chunk 复用缓存，远端建筑进入第二 Chunk"),
		Stats.MutableChunkCount, 2);
	TestEqual(TEXT("显式 Movable Building 不进入静态 Cell"), Stats.StaticCellCount, 0);
	const uint64 RevisionBeforeMove = Stats.IndexRevision;

	FBuildTransformFragment* Transform = Registry.FindMutableFragment<FBuildTransformFragment>(A);
	if (!TestNotNull(TEXT("可修改普通建筑 Transform"), Transform))
	{
		return false;
	}
	Transform->WorldTransform.SetLocation(FVector(18000.0, 0.0, 0.0));
	Dirty.MarkRebuild(A);
	TestTrue(TEXT("跨 Chunk 更新刷新源/目标缓存"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("移动后两个非空 Chunk 都被保留"), Stats.MutableChunkCount, 2);
	TestTrue(TEXT("跨 Chunk 更新推进索引 Revision"), Stats.IndexRevision > RevisionBeforeMove);

	TestTrue(TEXT("销毁旧 Chunk 最后一栋普通建筑"), Registry.DestroyEntity(B));
	Dirty.MarkRebuild(B);
	TestTrue(TEXT("删除差量刷新可变 Chunk"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("空可变 Chunk 被回收"), Processor.GetSelectionStats().MutableChunkCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationStableHotMigrationTest,
	"ElementSandbox.Building.Presentation.HotMigrationKeepsStableHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationStableHotMigrationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 10000.0;
	Config.LocalResidentTargetMeshParts = 8;
	Config.StableResidentTargetMeshParts = 10;
	Config.SourceMovementThreshold = 1.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(
		Harness.World,
		*Mesh,
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	const FBuildEntityHandle Entity = Definition->CreateEntity(
		Registry,
		FTransform(FVector(5000.0, 0.0, 0.0)));
	Dirty.MarkRebuild(Entity);
	TestTrue(TEXT("索引 Entity"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot NearViews;
	NearViews.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	NearViews.Revision = 1;
	TestTrue(TEXT("近场投影到即时 ISM"),
		Processor.Project(Registry, NearViews, *Harness.Presentation, Harness.Layer));
	FMeshPoolInstanceHandle Before;
	TestTrue(TEXT("取得 Hot 句柄"), Processor.TryGetInstanceHandle(Entity, 0, Before));
	EBuildRenderStorageClass Storage = EBuildRenderStorageClass::StaticHISM;
	TestTrue(TEXT("近场为 Hot ISM"),
		Processor.TryGetPartStorageClass(Entity, 0, Storage)
			&& Storage == EBuildRenderStorageClass::HotISM);

	FPresentationViewSnapshot FarViews;
	FarViews.Sources.Add(MakeView(FVector(-20000.0, 0.0, 0.0), FVector::ForwardVector, 2));
	FarViews.Revision = 2;
	TestTrue(TEXT("远离后迁回 HISM"),
		Processor.Project(Registry, FarViews, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("提交迁移"), Harness.Presentation->FlushNow());
	FMeshPoolInstanceHandle After;
	TestTrue(TEXT("迁移后句柄仍存在"), Processor.TryGetInstanceHandle(Entity, 0, After));
	TestTrue(TEXT("迁移保持逻辑句柄"), Before == After);
	TestTrue(TEXT("远场为 Cold HISM"),
		Processor.TryGetPartStorageClass(Entity, 0, Storage)
			&& Storage == EBuildRenderStorageClass::ColdPromotableHISM);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationDirectionCacheTest,
	"ElementSandbox.Building.Presentation.DirectionCacheKeepsStableHandles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationDirectionCacheTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 3;
	Config.ForwardCoverageAngleDegrees = 180.0;
	Config.EvictionGraceSeconds = 5.0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.5;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle Center = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 100);
	const FBuildEntityHandle East = AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 101);
	const FBuildEntityHandle West = AddStaticEntity(Registry, *Definition, FVector(-10000.0, 0.0, 0.0), 102);
	Dirty.MarkRebuild(Center);
	Dirty.MarkRebuild(East);
	Dirty.MarkRebuild(West);
	TestTrue(TEXT("构建方向缓存测试索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("初始加载前向半平面"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("提交初始方向"), Harness.Presentation->FlushNow());
	TestEqual(TEXT("本地中心与东侧为当前需求"), Processor.GetRenderedEntityCount(), 2);
	Processor.SetResidencyTimeSecondsForTesting(0.5);
	TestTrue(TEXT("初始方向停稳后晋升 Active"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FMeshPoolInstanceHandle EastHandle;
	TestTrue(TEXT("东侧句柄已创建"), Processor.TryGetInstanceHandle(East, 0, EastHandle));
	const FMeshPoolStats BeforeTurn = Harness.Presentation->GetMeshPoolStats();

	Processor.SetResidencyTimeSecondsForTesting(0.6);
	Views.Sources[0] = MakeView(FVector::ZeroVector, -FVector::ForwardVector, 2);
	TestTrue(TEXT("旋转 180 度只增加新方向"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("提交反向新增"), Harness.Presentation->FlushNow());
	const FBuildPresentationSelectionStats TurnedStats = Processor.GetSelectionStats();
	TestEqual(TEXT("旧方向仍由 ActiveFar 保护"), TurnedStats.ActiveFarMeshPartCount, 1);
	TestEqual(TEXT("新方向进入独立 TransitionFar"), TurnedStats.TransitionFarMeshPartCount, 1);
	TestEqual(TEXT("三栋全部驻留"), TurnedStats.ResidentEntityCount, 3);
	FMeshPoolInstanceHandle StableEastHandle;
	TestTrue(TEXT("东侧旧方向句柄保持稳定"),
		Processor.TryGetInstanceHandle(East, 0, StableEastHandle) && StableEastHandle == EastHandle);
	const FMeshPoolStats AfterTurn = Harness.Presentation->GetMeshPoolStats();
	TestEqual(TEXT("转向 Flush 只有一条 Add、没有 Remove"),
		AfterTurn.TotalFlushedInstanceCount - BeforeTurn.TotalFlushedInstanceCount,
		static_cast<uint64>(1));

	Processor.SetResidencyTimeSecondsForTesting(0.7);
	Views.Sources[0] = MakeView(FVector::ZeroVector, FVector::ForwardVector, 3);
	TestTrue(TEXT("转回旧方向复用缓存"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("转回后只清理未晋升的 Transition"), Harness.Presentation->FlushNow());
	TestEqual(TEXT("转回旧 Active 不重复 Add，只移除一条废弃预取"),
		Harness.Presentation->GetMeshPoolStats().TotalFlushedInstanceCount - AfterTurn.TotalFlushedInstanceCount,
		static_cast<uint64>(1));
	AMeshPoolRenderHost* Host = Harness.Presentation->GetRenderHost();
	if (!TestNotNull(TEXT("存在 MeshPool Host"), Host))
	{
		return false;
	}
	Host->ClearLayer(Harness.Layer);
	TestTrue(TEXT("制造缓存 Layer 的物理映射失败"),
		Harness.Presentation->QueueUpdate(EastHandle, FTransform(FVector(10000.0, 0.0, 0.0))));
	TestFalse(TEXT("丢失物理映射时 Flush 失败并请求重投影"), Harness.Presentation->FlushNow());
	TestTrue(TEXT("完整重投影保留 ResidentCache 逻辑集合"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("失败恢复后 Active 逻辑集合不丢失"),
		Processor.GetSelectionStats().ActiveFarMeshPartCount, 1);
	TestEqual(TEXT("失败恢复后仍有本地与 Active 两栋驻留"), Processor.GetRenderedEntityCount(), 2);
	TestTrue(TEXT("提交完整缓存重投影"), Harness.Presentation->FlushNow());

	TestTrue(TEXT("真实销毁 Entity"), Registry.DestroyEntity(East));
	Dirty.MarkRebuild(East);
	TestTrue(TEXT("真实销毁立即移除表现"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestFalse(TEXT("销毁不等待缓存 Grace"), Processor.TryGetInstanceHandle(East, 0, StableEastHandle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationDensityAdaptiveBoundaryTest,
	"ElementSandbox.Building.Presentation.DensityControlsResidentBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationDensityAdaptiveBoundaryTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 4;
	Config.StableResidentTargetMeshParts = 4;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	TArray<FBuildEntityHandle> Dense;
	TArray<FBuildEntityHandle> Sparse;
	const FVector SparseOrigin(200000.0, 0.0, 0.0);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Dense.Add(AddStaticEntity(
			Registry, *Definition, FVector((Index + 1) * 1000.0, 0.0, 0.0), 200 + Index));
		Sparse.Add(AddStaticEntity(
			Registry, *Definition, SparseOrigin + FVector((Index + 1) * 5000.0, 0.0, 0.0), 300 + Index));
		Dirty.MarkRebuild(Dense.Last());
		Dirty.MarkRebuild(Sparse.Last());
	}
	TestTrue(TEXT("构建密度区域索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("在密集区域按 360 度最近距离扩展"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	const double DenseBoundary = Processor.GetSelectionStats().LocalResidentBoundary;
	TestTrue(TEXT("密集区域四个 Part 的动态边界接近 40m"), DenseBoundary < 5000.0);

	Views.Sources[0] = MakeView(SparseOrigin, FVector::ForwardVector, 2);
	TestTrue(TEXT("移动到稀疏区域重新计算本地边界"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	const double SparseBoundary = Processor.GetSelectionStats().LocalResidentBoundary;
	TestTrue(TEXT("相同 Part 预算下，密集方向的驻留边界自然更近"),
		DenseBoundary < SparseBoundary);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationCameraSubjectSeparationTest,
	"ElementSandbox.Building.Presentation.CameraAndSubjectAreIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationCameraSubjectSeparationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 1;
	Config.ResidentHardWatermarkMeshParts = 2;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle SubjectNear = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 700);
	const FBuildEntityHandle CameraNear = AddStaticEntity(Registry, *Definition, FVector(-50000.0, 0.0, 0.0), 701);
	Dirty.MarkRebuild(SubjectNear);
	Dirty.MarkRebuild(CameraNear);
	TestTrue(TEXT("构建相机/主体分离索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	FPresentationViewSource View = MakeView(FVector(-50000.0, 0.0, 0.0), FVector::ForwardVector);
	View.SubjectLocation = FVector::ZeroVector;
	Views.Sources.Add(View);
	TestTrue(TEXT("使用 Subject 位置选择本地集合"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FMeshPoolInstanceHandle Instance;
	TestTrue(TEXT("Pawn 附近建筑驻留"), Processor.TryGetInstanceHandle(SubjectNear, 0, Instance));
	TestFalse(TEXT("相机附近但非当前需求的建筑不被本地预算误选"),
		Processor.TryGetInstanceHandle(CameraNear, 0, Instance));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationLargeBoundsSurfacePriorityTest,
	"ElementSandbox.Building.Presentation.LargeBoundsUseNearestSurfacePriority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationLargeBoundsSurfacePriorityTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 2;
	Config.ForwardCoverageAngleDegrees = 180.0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;

	UBuildTestDefinition* RegularDefinition =
		MakePresentationDefinition(Harness.World, *Mesh);
	UBuildTestDefinition* GiantDefinition =
		MakePresentationDefinition(Harness.World, *Mesh);
	GiantDefinition->MeshParts[0].LocalTransform = FTransform(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(2000.0, 1.0, 1.0));

	const FBuildEntityHandle Local = AddStaticEntity(
		Registry,
		*RegularDefinition,
		FVector::ZeroVector,
		710);
	const FBuildEntityHandle SmallerButFartherSurface = AddStaticEntity(
		Registry,
		*RegularDefinition,
		FVector(30000.0, 0.0, 0.0),
		711);
	const FBuildEntityHandle GiantNearSurface = AddStaticEntity(
		Registry,
		*GiantDefinition,
		FVector(120000.0, 0.0, 0.0),
		712);
	TestTrue(TEXT("创建本地、小型远处和中心极远的巨大建筑"),
		Local.IsSet() && SmallerButFartherSurface.IsSet() && GiantNearSurface.IsSet());
	Dirty.MarkRebuild(Local);
	Dirty.MarkRebuild(SmallerButFartherSurface);
	Dirty.MarkRebuild(GiantNearSurface);
	TestTrue(TEXT("构建巨大 Bounds 表现索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("按最近表面执行前方预算选择"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FMeshPoolInstanceHandle Instance;
	TestTrue(TEXT("本地预算保留原点建筑"),
		Processor.TryGetInstanceHandle(Local, 0, Instance));
	TestTrue(TEXT("巨大建筑中心虽远但近表面优先进入前方预算"),
		Processor.TryGetInstanceHandle(GiantNearSurface, 0, Instance));
	TestFalse(TEXT("表面更远的小型建筑不抢占巨大近表面建筑"),
		Processor.TryGetInstanceHandle(SmallerButFartherSurface, 0, Instance));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationMinimumRadiusAndColdExpansionTest,
	"ElementSandbox.Building.Presentation.MinimumRadiusAndColdLocalExpansion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationMinimumRadiusAndColdExpansionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 10000.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 1;
	Config.ResidentHardWatermarkMeshParts = 8;
	Config.HotPromotionRadius = 1000.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(
		Harness.World,
		*Mesh,
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	TArray<FBuildEntityHandle> Mandatory;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Mandatory.Add(AddStaticEntity(Registry, *Definition, FVector(Index * 4000.0, 0.0, 0.0), 720 + Index));
		Dirty.MarkRebuild(Mandatory.Last());
	}
	const FBuildEntityHandle Outside = AddStaticEntity(Registry, *Definition, FVector(20000.0, 0.0, 0.0), 730);
	Dirty.MarkRebuild(Outside);
	TestTrue(TEXT("构建最小半径索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("最小 100m 半径不受一 Part 目标截断"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("半径内三栋全部成为 Required"), Processor.GetSelectionStats().RequiredEntityCount, 3);
	FMeshPoolInstanceHandle Instance;
	TestFalse(TEXT("半径外建筑未被超额本地集合加载"), Processor.TryGetInstanceHandle(Outside, 0, Instance));
	EBuildRenderStorageClass Storage = EBuildRenderStorageClass::HotISM;
	TestTrue(TEXT("动态本地半径内但 Hot 半径外仍使用 Cold HISM"),
		Processor.TryGetPartStorageClass(Mandatory[2], 0, Storage)
			&& Storage == EBuildRenderStorageClass::ColdPromotableHISM);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationEvictionGraceAndFrequencyTest,
	"ElementSandbox.Building.Presentation.EvictionGraceFrequencyAndZeroSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationEvictionGraceAndFrequencyTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 1;
	Config.ResidentHardWatermarkMeshParts = 2;
	Config.EvictionGraceSeconds = 5.0;
	Config.EvictionFrequencyHz = 2.0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	TArray<FBuildEntityHandle> Entities;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Entities.Add(AddStaticEntity(Registry, *Definition, FVector(Index * 100000.0, 0.0, 0.0), 750 + Index));
		Dirty.MarkRebuild(Entities.Last());
	}
	TestTrue(TEXT("构建淘汰索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	Views.Sources.Add(MakeView(FVector(0.0, 0.0, 0.0), FVector::ForwardVector));
	TestTrue(TEXT("加载区域 A"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Processor.SetResidencyTimeSecondsForTesting(0.5);
	Views.Sources[0] = MakeView(FVector(100000.0, 0.0, 0.0), FVector::ForwardVector, 2);
	TestTrue(TEXT("加载区域 B"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Processor.SetResidencyTimeSecondsForTesting(1.0);
	Views.Sources[0] = MakeView(FVector(200000.0, 0.0, 0.0), FVector::ForwardVector, 3);
	TestTrue(TEXT("加载区域 C 并超过高水位"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("Grace 内允许三栋短暂超限"), Processor.GetRenderedEntityCount(), 3);
	TestEqual(TEXT("两个历史方向被 Grace 阻塞"),
		Processor.GetSelectionStats().EvictionGraceBlockedCount, 2);

	Processor.SetResidencyTimeSecondsForTesting(5.6);
	TestTrue(TEXT("A 离开需求满五秒后执行第一轮淘汰"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("只有 A 满足 Grace，B 继续缓存"), Processor.GetRenderedEntityCount(), 2);
	Processor.SetResidencyTimeSecondsForTesting(6.0);
	TestTrue(TEXT("不足 0.5 秒不重复执行淘汰"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("2Hz 周期内不删除 B"), Processor.GetRenderedEntityCount(), 2);
	Processor.SetResidencyTimeSecondsForTesting(6.2);
	TestTrue(TEXT("下一次 2Hz 周期淘汰 B"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("缓存回落到当前 Required"), Processor.GetRenderedEntityCount(), 1);

	Processor.SetResidencyTimeSecondsForTesting(6.3);
	Views.Sources.Reset();
	TestTrue(TEXT("零 Source 先保留最后一栋"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("零 Source 同样从离开需求时开始计算 Grace"), Processor.GetRenderedEntityCount(), 1);
	Processor.SetResidencyTimeSecondsForTesting(11.4);
	TestTrue(TEXT("零 Source 超过 Grace 后淘汰非运动缓存"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("零 Source 缓存最终清空"), Processor.GetRenderedEntityCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationHotAndMotionPinTest,
	"ElementSandbox.Building.Presentation.HotAndMotionPinsBlockEviction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationHotAndMotionPinTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 1;
	Config.ResidentHardWatermarkMeshParts = 1;
	Config.EvictionGraceSeconds = 0.0;
	Config.EvictionFrequencyHz = 2.0;
	Config.HotPromotionRadius = 2000.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(
		Harness.World,
		*Mesh,
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	const FBuildEntityHandle A = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 780);
	const FBuildEntityHandle B = AddStaticEntity(Registry, *Definition, FVector(1000.0, 0.0, 0.0), 781);
	const FBuildEntityHandle C = AddStaticEntity(Registry, *Definition, FVector(100000.0, 0.0, 0.0), 782);
	Dirty.MarkRebuild(A);
	Dirty.MarkRebuild(B);
	Dirty.MarkRebuild(C);
	TestTrue(TEXT("构建 Pin 测试索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("初始加载 A"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Processor.SetResidencyTimeSecondsForTesting(1.0);
	Views.Sources[0] = MakeView(FVector(1000.0, 0.0, 0.0), FVector::ForwardVector, 2);
	TestTrue(TEXT("加载 B 后 A 位于 Hot Pin 内"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("Hot Pin 阻止 A 在超水位时被淘汰"), Processor.GetRenderedEntityCount(), 2);

	TestTrue(TEXT("激活 A 的 Motion Pin"), Processor.SetPresentationMotionActive(A, true));
	Processor.SetResidencyTimeSecondsForTesting(2.0);
	Views.Sources[0] = MakeView(FVector(100000.0, 0.0, 0.0), FVector::ForwardVector, 3);
	TestTrue(TEXT("移动远离 Hot 范围并加载 C"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FMeshPoolInstanceHandle Instance;
	TestTrue(TEXT("Motion Pin 保留 A"), Processor.TryGetInstanceHandle(A, 0, Instance));
	TestFalse(TEXT("未 Pin 的 B 被低频批量淘汰"), Processor.TryGetInstanceHandle(B, 0, Instance));
	TestTrue(TEXT("当前 C 保持 Required"), Processor.TryGetInstanceHandle(C, 0, Instance));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationWholeSubtreeAcceptanceTest,
	"ElementSandbox.Building.Presentation.WholeSubtreeAcceptance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationWholeSubtreeAcceptanceTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 64;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const FBuildEntityHandle Entity = AddStaticEntity(
			Registry,
			*Definition,
			FVector(10000.0 + Index * 100.0, (Index - 8) * 50.0, 0.0),
			400 + Index);
		Dirty.MarkRebuild(Entity);
	}
	TestTrue(TEXT("构建整树接收测试索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	Views.Sources[0].HorizontalFOVDegrees = 90.0f;
	TestTrue(TEXT("投影完全位于视锥内的 BVH"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	const FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("完整接收全部 Part"), Stats.ResidentMeshPartCount, 16);
	TestTrue(TEXT("预算足够时直接接受连续子树范围"), Stats.AcceptedSubtreeCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationPackedRemovalTombstoneTest,
	"ElementSandbox.Building.Presentation.PackedRemovalUsesVersionedTombstones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationPackedRemovalTombstoneTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.World || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	FBuildPresentationIndex Index(Config);
	FBuildEntityRegistry Registry;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	constexpr int32 EntryCount = 300;
	TArray<FBuildEntityHandle> Entities;
	Entities.Reserve(EntryCount);
	for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
	{
		const FVector Location(10000.0 + EntryIndex * 10.0, 10000.0, 0.0);
		const FBuildEntityHandle Entity = AddStaticEntity(Registry, *Definition, Location, 50000 + EntryIndex);
		Entities.Add(Entity);
		TestTrue(
			TEXT("静态 Entry 写入增量索引"),
			Index.UpsertEntry(Entity, FBox::BuildAABB(Location, FVector(5.0)), 1, 0, true));
	}
	Index.ProcessAsyncPackedBuildWork(true);
	TestEqual(TEXT("初始三百条只构建一次 Packed BVH"), Index.GetStaticBVHBuildCount(), static_cast<uint64>(1));

	// 先删除 256 条，再更新一条、删除其余旧版本；RemoveEntry 不得同步重扫或重建 1km Cell。
	for (int32 EntryIndex = 0; EntryIndex < 256; ++EntryIndex)
	{
		Index.RemoveEntry(Entities[EntryIndex]);
	}
	const FVector UpdatedLocation(20000.0, 12000.0, 0.0);
	TestTrue(
		TEXT("同一 Entity 的新静态版本写入独立 Serial"),
		Index.UpsertEntry(
			Entities[256], FBox::BuildAABB(UpdatedLocation, FVector(5.0)), 1, 0, true));
	for (int32 EntryIndex = 257; EntryIndex < EntryCount; ++EntryIndex)
	{
		Index.RemoveEntry(Entities[EntryIndex]);
	}
	TestEqual(
		TEXT("批量删除与更新期间没有同步重建 Packed BVH"),
		Index.GetStaticBVHBuildCount(),
		static_cast<uint64>(1));

	FBuildFarSelectionRequest Request;
	Request.ViewLocation = FVector::ZeroVector;
	Request.SubjectLocation = FVector::ZeroVector;
	Request.Forward = FVector2D(1.0, 0.0);
	Request.HorizontalFOVDegrees = 90.0;
	Request.CoverageAngleDegrees = 360.0;
	Request.FOVSafetyAngleDegrees = 0.0;
	Request.TargetFarMeshParts = EntryCount;
	TArray<FBuildPresentationSelectorEntry> ExistingTransitionEntries;
	FBuildPresentationSelectorEntry& RetainedTransition = ExistingTransitionEntries.AddDefaulted_GetRef();
	RetainedTransition.Entity = Entities[256];
	RetainedTransition.Bounds = FBox::BuildAABB(UpdatedLocation, FVector(5.0));
	RetainedTransition.MeshPartCost = 1;
	FBuildPresentationSelectorEntry& SupersededTransition = ExistingTransitionEntries.AddDefaulted_GetRef();
	SupersededTransition.Entity = Entities[0];
	SupersededTransition.Bounds = FBox::BuildAABB(FVector(10000.0, 10000.0, 0.0), FVector(5.0));
	SupersededTransition.MeshPartCost = 1;
	Request.ExistingTransitionEntries =
		MakeShared<TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>(
			MoveTemp(ExistingTransitionEntries));
	Index.CaptureSelectionSources(Request);
	const FBuildFarSelectionResult BeforeCompaction =
		FBuildPresentationResidencySelector::Select(MoveTemp(Request));
	TestEqual(TEXT("旧快照墓碑只留下一个当前版本"), BeforeCompaction.OrderedTargetEntries.Num(), 1);
	TestEqual(TEXT("Worker 同时产出可直接接管的目标集合"), BeforeCompaction.TargetEntities.Num(), 1);
	TestEqual(
		TEXT("Worker 只发布旧 Transition 与新目标的精确差集"),
		BeforeCompaction.SupersededTransitionEntities.Num(),
		1);
	if (BeforeCompaction.SupersededTransitionEntities.Num() == 1)
	{
		TestEqual(
			TEXT("仍在新目标中的 Transition 不会被回收"),
			BeforeCompaction.SupersededTransitionEntities[0],
			Entities[0]);
	}
	if (BeforeCompaction.OrderedTargetEntries.Num() == 1)
	{
		TestTrue(TEXT("目标集合包含选择结果"), BeforeCompaction.TargetEntities.Contains(Entities[256]));
		TestEqual(
			TEXT("选择器保留的是更新后的 Entity"),
			BeforeCompaction.OrderedTargetEntries[0].Entity,
			Entities[256]);
		TestTrue(
			TEXT("旧快照 Bounds 不会覆盖新版本 Bounds"),
			BeforeCompaction.OrderedTargetEntries[0].Bounds.GetCenter().Equals(UpdatedLocation));
	}

	Index.ProcessAsyncPackedBuildWork(true);
	const FBuildPresentationIndexStats Stats = Index.GetStats();
	TestEqual(TEXT("达到阈值后后台压实为一个当前 Entry"), Stats.StaticPackedEntryCount, 1);
	TestEqual(TEXT("压实后没有残留 Delta"), Stats.StaticDeltaEntryCount, 0);
	TestEqual(TEXT("墓碑压实只新增一次 Packed BVH Build"), Stats.StaticBVHBuildCount, static_cast<uint64>(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationIncrementalPackedCellTest,
	"ElementSandbox.Building.Presentation.IncrementalPackedCell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationIncrementalPackedCellTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 200.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 1;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	FPresentationViewSnapshot Views;

	constexpr int32 EntryCount = 875;
	constexpr int32 BatchSize = 25;
	for (int32 BatchStart = 0; BatchStart < EntryCount; BatchStart += BatchSize)
	{
		FBuildEntityHandle LastEntity;
		FVector LastLocation = FVector::ZeroVector;
		for (int32 EntryIndex = BatchStart;
			EntryIndex < FMath::Min(BatchStart + BatchSize, EntryCount);
			++EntryIndex)
		{
			LastLocation = FVector(
				10000.0 + (EntryIndex % 30) * 1000.0,
				10000.0 + (EntryIndex / 30) * 1000.0,
				0.0);
			LastEntity = AddStaticEntity(
				Registry,
				*Definition,
				LastLocation,
				1000 + EntryIndex);
			Dirty.MarkRebuild(LastEntity);
		}
		TestTrue(
			FString::Printf(TEXT("表现增量批次 %d 写入索引"), BatchStart / BatchSize),
			Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
		Views.Sources.Reset();
		Views.Sources.Add(MakeView(
			LastLocation,
			FVector::ForwardVector,
			1 + BatchStart / BatchSize));
		++Views.Revision;
		TestTrue(
			FString::Printf(TEXT("表现增量批次 %d 完成驻留选择"), BatchStart / BatchSize),
			Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
		FMeshPoolInstanceHandle Instance;
		TestTrue(TEXT("尚未固化的最新 Delta Entity 也能进入近场驻留"),
			Processor.TryGetInstanceHandle(LastEntity, 0, Instance));
	}

	const FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("几何增长阈值只发布已固化的不可变快照"),
		Stats.StaticPackedEntryCount, static_cast<int64>(600));
	TestEqual(TEXT("未达到当前 600 条阈值的尾部继续作为可查询 Delta"),
		Stats.StaticDeltaEntryCount, static_cast<int64>(275));
	TestEqual(TEXT("35 个 25 条小批次只构建三次表现 Packed BVH"),
		Stats.StaticBVHBuildCount, static_cast<uint64>(3));
	TestEqual(TEXT("35 次移动各新增一栋，历史本地集合进入缓存而不立即删除"),
		Stats.ResidentEntityCount, EntryCount / BatchSize);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationBudgetRemainderPruningTest,
	"ElementSandbox.Building.Presentation.BudgetRemainderPruning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationBudgetRemainderPruningTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 10;
	Config.StableResidentTargetMeshParts = 10;
	Config.SourceMovementThreshold = 0.0;
	Config.MinimumRecenterAngleDegrees = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildMeshPartDefinition AdditionalPart = Definition->MeshParts[0];
	Definition->MeshParts.Add(AdditionalPart);
	Definition->MeshParts.Add(AdditionalPart);
	for (int32 EntryIndex = 0; EntryIndex < 1024; ++EntryIndex)
	{
		const FBuildEntityHandle Entity = AddStaticEntity(
			Registry,
			*Definition,
			FVector(
				50000.0 + (EntryIndex % 32) * 500.0,
				10000.0 + (EntryIndex / 32) * 500.0,
				0.0),
			2000 + EntryIndex);
		Dirty.MarkRebuild(Entity);
	}
	TestTrue(TEXT("构建预算余量剪枝索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	Views.Sources[0].HorizontalFOVDegrees = 90.0f;
	TestTrue(TEXT("执行预算余量驻留选择"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	const FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("10 Part 预算最多装入三栋三 Part 建筑"), Stats.ResidentMeshPartCount, 9);
	TestTrue(TEXT("剩余 1 Part 时按节点最小 3 Part 成本停止尾扫"), Stats.CandidateNodeCount < 256);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationDynamicRecenterThresholdTest,
	"ElementSandbox.Building.Presentation.DynamicRecenterThresholdIgnoresPitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationDynamicRecenterThresholdTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 4;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 100000.0;
	Config.MinimumRecenterAngleDegrees = 5.0;
	Config.FOVSafetyAngleDegrees = 10.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle Local = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 6100);
	const FBuildEntityHandle East = AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 6101);
	const FBuildEntityHandle NorthEast = AddStaticEntity(Registry, *Definition, FVector(10000.0, 10000.0, 0.0), 6102);
	Dirty.MarkRebuild(Local);
	Dirty.MarkRebuild(East);
	Dirty.MarkRebuild(NorthEast);
	TestTrue(TEXT("构建动态重校准索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	Views.Sources[0].HorizontalFOVDegrees = 90.0f;
	TestTrue(TEXT("初始方向执行一次选择"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("初始选择次数"), Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));

	Processor.SetResidencyTimeSecondsForTesting(0.1);
	Views.Sources[0] = MakeView(FVector::ZeroVector, FRotator(0.0, 34.0, 0.0).Vector(), 2);
	Views.Sources[0].HorizontalFOVDegrees = 90.0f;
	TestTrue(TEXT("90 度 FOV 下转动 34 度仍复用 Active"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("35 度阈值内不重选"), Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));

	Processor.SetResidencyTimeSecondsForTesting(0.2);
	Views.Sources[0] = MakeView(FVector::ZeroVector, FRotator(45.0, 34.0, 0.0).Vector(), 3);
	Views.Sources[0].HorizontalFOVDegrees = 90.0f;
	TestTrue(TEXT("只改变俯仰不重选远景"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("俯仰不计入水平中线"), Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));

	Processor.SetResidencyTimeSecondsForTesting(0.3);
	Views.Sources[0] = MakeView(FVector::ZeroVector, FRotator(0.0, 36.0, 0.0).Vector(), 4);
	Views.Sources[0].HorizontalFOVDegrees = 90.0f;
	TestTrue(TEXT("越过 35 度阈值校准一次"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("越界只新增一次选择"), Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(2));
	Processor.SetResidencyTimeSecondsForTesting(0.4);
	TestTrue(TEXT("校准后的同方向继续复用"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("同方向不重复发布选择"), Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationRapidTurnFreezeTest,
	"ElementSandbox.Building.Presentation.RapidTurnFreezesFarTransition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationRapidTurnFreezeTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 1000.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 2;
	Config.TransitionReserveMeshParts = 1;
	Config.ResidentHardWatermarkMeshParts = 3;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 100000.0;
	Config.FarSettleSeconds = 0.20;
	Config.PromotionStableSeconds = 0.5;
	Config.UnstablePromotionLockSeconds = 2.0;
	Config.RapidRotationThresholdDegreesPerSecond = 180.0;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle Local = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 6200);
	const FBuildEntityHandle East = AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 6201);
	const FBuildEntityHandle North = AddStaticEntity(Registry, *Definition, FVector(0.0, 10000.0, 0.0), 6202);
	Dirty.MarkRebuild(Local);
	Dirty.MarkRebuild(East);
	Dirty.MarkRebuild(North);
	TestTrue(TEXT("构建快速转向索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	Views.Sources[0].HorizontalFOVDegrees = 90.0f;
	TestTrue(TEXT("初始方向预取"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Processor.SetResidencyTimeSecondsForTesting(0.2);
	TestTrue(TEXT("初始方向停稳后开始预取"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Processor.SetResidencyTimeSecondsForTesting(0.7);
	TestTrue(TEXT("初始方向稳定后晋升"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("测试前置：一 Part 本地集合"), Processor.GetSelectionStats().RequiredMeshPartCount, 2);
	TestEqual(TEXT("测试前置：一 Part 旧方向已晋升 Active"),
		Processor.GetSelectionStats().ActiveFarMeshPartCount, 1);
	FMeshPoolInstanceHandle EastHandle;
	TestTrue(TEXT("旧 Active 句柄存在"), Processor.TryGetInstanceHandle(East, 0, EastHandle));
	const uint64 SelectionBeforeRapid = Processor.GetSelectionStats().SelectionPassCount;

	for (const TPair<double, double>& Step : {
		TPair<double, double>(0.8, 90.0),
		TPair<double, double>(0.9, 0.0),
		TPair<double, double>(1.0, 90.0)})
	{
		Processor.SetResidencyTimeSecondsForTesting(Step.Key);
		Views.Sources[0] = MakeView(FVector::ZeroVector, FRotator(0.0, Step.Value, 0.0).Vector());
		Views.Sources[0].HorizontalFOVDegrees = 90.0f;
		TestTrue(TEXT("高速往复转向周期"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
		const FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
		TestEqual(TEXT("高速旋转期间不发布远景任务"), Stats.SelectionPassCount, SelectionBeforeRapid);
		TestEqual(TEXT("高速旋转期间不删除旧方向"), Stats.LastCycleRemovedMeshPartCount, 0);
		TestTrue(TEXT("高速旋转状态可观测"), Stats.RapidRotationFrozenSourceCount > 0);
		FMeshPoolInstanceHandle StableHandle;
		TestTrue(TEXT("高速旋转期间旧 Active 句柄稳定"),
			Processor.TryGetInstanceHandle(East, 0, StableHandle) && StableHandle == EastHandle);
	}

	Processor.SetResidencyTimeSecondsForTesting(1.25);
	TestTrue(TEXT("停止后的第一帧开始 Settling"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Processor.SetResidencyTimeSecondsForTesting(1.44);
	TestTrue(TEXT("停稳不足 200ms 仍不选择"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("停稳窗口内选择次数不变"),
		Processor.GetSelectionStats().SelectionPassCount, SelectionBeforeRapid);
	Processor.SetResidencyTimeSecondsForTesting(1.45);
	TestTrue(TEXT("停稳 200ms 后只选择最终方向"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("最终方向只发布一次"), Stats.SelectionPassCount, SelectionBeforeRapid + 1);
	TestEqual(TEXT("新方向先进入 Transition"), Stats.TransitionFarMeshPartCount, 1);
	TestEqual(TEXT("旧 Active 与新 Transition 同时驻留"), Stats.ResidentEntityCount, 3);
	TestEqual(TEXT("补可见核心时仍未删除旧方向"), Stats.LastCycleRemovedMeshPartCount, 0);

	Processor.SetResidencyTimeSecondsForTesting(2.99);
	TestTrue(TEXT("不稳定锁定两秒内不晋升"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("锁定期间旧方向仍为 Active"), Processor.GetSelectionStats().ActiveFarMeshPartCount, 1);
	Processor.SetResidencyTimeSecondsForTesting(3.0);
	TestTrue(TEXT("锁定期结束后晋升并清理"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("最终方向成为 Active"), Stats.ActiveFarMeshPartCount, 1);
	TestEqual(TEXT("晋升后旧方向垃圾完成回收"), Stats.ResidentEntityCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationSharedWorkBudgetTest,
	"ElementSandbox.Building.Presentation.AddRemoveShareDynamicWorkBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationSharedWorkBudgetTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 6;
	Config.TransitionReserveMeshParts = 3;
	Config.ResidentHardWatermarkMeshParts = 9;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 100000.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 4;
	Config.MinimumMeshPoolWorkBudgetParts = 4;
	Config.MaximumMeshPoolWorkBudgetParts = 4;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildMeshPartDefinition AdditionalPart = Definition->MeshParts[0];
	Definition->MeshParts.Add(AdditionalPart);
	Definition->MeshParts.Add(AdditionalPart);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FBuildEntityHandle East = AddStaticEntity(
			Registry, *Definition, FVector(10000.0 + Index * 2000.0, 0.0, 0.0), 6300 + Index);
		const FBuildEntityHandle West = AddStaticEntity(
			Registry, *Definition, FVector(-10000.0 - Index * 2000.0, 0.0, 0.0), 6310 + Index);
		Dirty.MarkRebuild(East);
		Dirty.MarkRebuild(West);
	}
	TestTrue(TEXT("构建共享工作预算索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("第一周期按 Part 填满共享预算"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("第一周期 Add 精确受四 Part 预算约束"),
		Processor.GetSelectionStats().LastCycleAddedMeshPartCount, 4);
	TestEqual(TEXT("第二栋半装填后仍有两个 Required Part 待提交"),
		Processor.GetSelectionStats().PendingRequiredMeshPartCount, 2);
	Processor.SetResidencyTimeSecondsForTesting(0.1);
	TestTrue(TEXT("第二周期补齐旧方向"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("旧方向稳定为六 Part"), Processor.GetSelectionStats().ActiveFarMeshPartCount, 6);

	Processor.SetResidencyTimeSecondsForTesting(1.0);
	Views.Sources[0] = MakeView(FVector::ZeroVector, -FVector::ForwardVector, 2);
	TestTrue(TEXT("换向第一周期先加入一个新方向 Entity"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestTrue(TEXT("换向周期 Add/Remove 共用四 Part 预算"),
		Stats.LastCycleAddedMeshPartCount + Stats.LastCycleRemovedMeshPartCount <= 4);
	TestEqual(TEXT("先 Add 后未提前删除旧 Active"), Stats.LastCycleRemovedMeshPartCount, 0);

	Processor.SetResidencyTimeSecondsForTesting(1.1);
	TestTrue(TEXT("硬水位阻塞时只释放最小旧数据"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestTrue(TEXT("Reclaim 与 Add 仍不能分别吃满预算"),
		Stats.LastCycleAddedMeshPartCount + Stats.LastCycleRemovedMeshPartCount <= 4);
	TestEqual(TEXT("本周期只回收一栋三 Part 旧建筑"), Stats.LastCycleRemovedMeshPartCount, 3);
	Processor.SetResidencyTimeSecondsForTesting(1.2);
	TestTrue(TEXT("下一周期再补齐新方向"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestTrue(TEXT("补齐周期仍遵守共享预算"),
		Stats.LastCycleAddedMeshPartCount + Stats.LastCycleRemovedMeshPartCount <= 4);
	Processor.SetResidencyTimeSecondsForTesting(1.3);
	TestTrue(TEXT("半速空闲清理完成旧方向垃圾"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("清理后回到稳定六 Part"), Processor.GetSelectionStats().ResidentMeshPartCount, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationOversizedEntityBudgetTest,
	"ElementSandbox.Building.Presentation.LargeEntityHasHardPartSliceLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationOversizedEntityBudgetTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}
	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 40;
	Config.TransitionReserveMeshParts = 0;
	Config.ResidentHardWatermarkMeshParts = 40;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 20;
	Config.MinimumMeshPoolWorkBudgetParts = 20;
	Config.MaximumMeshPoolWorkBudgetParts = 20;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildMeshPartDefinition AdditionalPart = Definition->MeshParts[0];
	for (int32 Index = 1; Index < 40; ++Index)
	{
		Definition->MeshParts.Add(AdditionalPart);
	}
	const FBuildEntityHandle Oversized = AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 6400);
	Dirty.MarkRebuild(Oversized);
	TestTrue(TEXT("构建超预算 Entity 索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	TestTrue(TEXT("超预算 Entity 第一周期只提交预算内 Part"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("第一周期即使预算为二十也只提交一个十六 Part Slice"),
		Stats.LastCycleAddedMeshPartCount, 16);
	TestEqual(TEXT("第一周期保留十六个已提交 Part"), Stats.ResidentMeshPartCount, 16);
	TestEqual(TEXT("半装填 Entity 仍标记为等待完整驻留"), Stats.PendingRequiredEntityCount, 1);
	TestEqual(TEXT("第一周期还差二十四个 Part"), Stats.PendingRequiredMeshPartCount, 24);
	Processor.SetResidencyTimeSecondsForTesting(0.1);
	TestTrue(TEXT("第二周期继续同一 Entity 的 Cursor"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("第二周期仍只提交十六 Part Slice"), Stats.LastCycleAddedMeshPartCount, 16);
	TestEqual(TEXT("第二周期还差八个 Part"), Stats.PendingRequiredMeshPartCount, 8);
	Processor.SetResidencyTimeSecondsForTesting(0.2);
	TestTrue(TEXT("第三周期完成剩余 Slice"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("第三周期只提交剩余八个 Part"), Stats.LastCycleAddedMeshPartCount, 8);
	TestEqual(TEXT("三周期后完整驻留四十 Part"), Stats.ResidentMeshPartCount, 40);
	TestEqual(TEXT("完整驻留后不再有 Pending Entity"), Stats.PendingRequiredEntityCount, 0);
	TestEqual(TEXT("完整驻留后不再有 Pending Part"), Stats.PendingRequiredMeshPartCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationLocalMovementDoubleBufferTest,
	"ElementSandbox.Building.Presentation.LocalMovementUsesAsyncDoubleBuffer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationLocalMovementDoubleBufferTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 2;
	Config.StableResidentTargetMeshParts = 2;
	Config.TransitionReserveMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 4;
	Config.EmergencyOverflowMeshParts = 0;
		Config.HotPromotionRadius = 1000.0;
		Config.SourceMovementThreshold = 1000.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
		Config.InitialMeshPoolWorkBudgetParts = 1;
		Config.MinimumMeshPoolWorkBudgetParts = 1;
		Config.MaximumMeshPoolWorkBudgetParts = 1;
		Config.LocalTransitionPublishBudgetEntitiesPerCycle = 2;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle A = AddStaticEntity(Registry, *Definition, FVector(0.0, 0.0, 0.0), 6500);
	const FBuildEntityHandle B = AddStaticEntity(Registry, *Definition, FVector(100.0, 0.0, 0.0), 6501);
	const FBuildEntityHandle C = AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 6502);
	const FBuildEntityHandle D = AddStaticEntity(Registry, *Definition, FVector(10100.0, 0.0, 0.0), 6503);
	Dirty.MarkRebuild(A);
	Dirty.MarkRebuild(B);
	Dirty.MarkRebuild(C);
	Dirty.MarkRebuild(D);
	TestTrue(TEXT("构建 Local 移动双缓冲测试索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("初始 Local 第一周期只补一栋"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("初始 Local 只发布一次纯值选择"), Stats.LocalSelectionPassCount, static_cast<uint64>(1));
	TestEqual(TEXT("未补齐前 TargetLocal 可观测"), Stats.TransitionLocalMeshPartCount, 2);
	TestEqual(TEXT("初始第一周期只应用一个 Part"), Stats.LastCycleAddedMeshPartCount, 1);
	TestEqual(TEXT("增量统计记录一个待补齐 Required Part"), Stats.PendingRequiredMeshPartCount, 1);
	TestTrue(TEXT("初始 Local 第二周期补齐并提交刷新点"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("初始提交后只有两 Part 为 Required"), Stats.RequiredMeshPartCount, 2);
	TestEqual(TEXT("补齐后增量 Pending 计数归零"), Stats.PendingRequiredMeshPartCount, 0);
		TestEqual(TEXT("初始提交清空 TargetLocal"), Stats.TransitionLocalMeshPartCount, 0);
		TestEqual(TEXT("初始 Local 的两个 Entity 都由 Hot 引用计数持有"), Stats.HotPinnedEntityCount, 2);

	Views.Sources[0] = MakeView(FVector(500.0, 0.0, 0.0), FVector::ForwardVector, 2);
	TestTrue(TEXT("阈值内移动复用当前 Local"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
		Stats = Processor.GetSelectionStats();
		TestEqual(TEXT("阈值内不发布新 Local 选择"), Stats.LocalSelectionPassCount, static_cast<uint64>(1));
		TestEqual(TEXT("阈值内稳定周期不扫描任何 Local Hot Entity"), Stats.HotPinMaintenanceEntityCount, 0);

	Views.Sources[0] = MakeView(FVector(10000.0, 0.0, 0.0), FVector::ForwardVector, 3);
	TestTrue(TEXT("越过阈值异步选择并先补新 Local"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("越界只新增一次 Local 选择"), Stats.LocalSelectionPassCount, static_cast<uint64>(2));
	TestEqual(TEXT("补齐前旧 Local 与完整 TargetLocal 同时持有逻辑引用"), Stats.RequiredMeshPartCount, 4);
		TestEqual(TEXT("补齐前旧 Local 不提前释放"), Stats.LastCycleRemovedMeshPartCount, 0);
		TestEqual(TEXT("新 Local 目标保持双缓冲可观测"), Stats.TransitionLocalMeshPartCount, 2);
		TestEqual(TEXT("越界准备严格受两 Entity 发布预算限制"), Stats.HotPinMaintenanceEntityCount, 2);
		TestEqual(TEXT("双缓冲期间新旧两组 Hot 引用同时有效"), Stats.HotPinnedEntityCount, 4);
	TestTrue(TEXT("下一周期补齐新 Local 后提交"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Stats = Processor.GetSelectionStats();
		TestEqual(TEXT("提交后逻辑 Required 回到 Local 预算"), Stats.RequiredMeshPartCount, 2);
		TestEqual(TEXT("提交后 TargetLocal 清空"), Stats.TransitionLocalMeshPartCount, 0);
		TestEqual(TEXT("旧 Local Hot 释放也严格受发布预算限制"), Stats.HotPinMaintenanceEntityCount, 2);
		TestEqual(TEXT("提交后只保留新 Local 的 Hot 引用"), Stats.HotPinnedEntityCount, 2);

	Views.Sources[0] = MakeView(FVector(10500.0, 0.0, 0.0), FVector::ForwardVector, 4);
	TestTrue(TEXT("相对新刷新点阈值内继续复用"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
		Stats = Processor.GetSelectionStats();
		TestEqual(TEXT("提交会把刷新点重置到任务请求位置"), Stats.LocalSelectionPassCount, static_cast<uint64>(2));
		TestEqual(TEXT("相对新刷新点阈值内仍然不维护 Hot 集合"), Stats.HotPinMaintenanceEntityCount, 0);
	Views.Sources[0] = MakeView(FVector(11100.0, 0.0, 0.0), FVector::ForwardVector, 5);
	TestTrue(TEXT("相对新刷新点再次越界"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("第二次越界只新增一次 Local 选择"),
		Processor.GetSelectionStats().LocalSelectionPassCount, static_cast<uint64>(3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationLocalResultPublishBudgetTest,
	"ElementSandbox.Building.Presentation.LocalResultPublishIsIncremental",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationLocalResultPublishBudgetTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 4;
	Config.StableResidentTargetMeshParts = 4;
	Config.TransitionReserveMeshParts = 4;
	Config.ResidentHardWatermarkMeshParts = 8;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 1000.0;
	Config.SourceMovementThreshold = 1000.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 4;
	Config.MinimumMeshPoolWorkBudgetParts = 4;
	Config.MaximumMeshPoolWorkBudgetParts = 4;
	Config.LocalTransitionPublishBudgetEntitiesPerCycle = 1;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FBuildEntityHandle Entity =
			AddStaticEntity(Registry, *Definition, FVector(Index * 100.0, 0.0, 0.0), 6550 + Index);
		TestTrue(TEXT("创建增量发布测试 Entity"),
			Entity.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Entity));
	}
	TestTrue(TEXT("构建增量发布测试索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	for (int32 Cycle = 0; Cycle < 4; ++Cycle)
	{
		TestTrue(TEXT("按发布预算推进 Local Target"),
			Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
		const FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
		TestEqual(TEXT("每周期只认领一个 Local Entity"), Stats.LastCycleAddedMeshPartCount, 1);
		TestEqual(TEXT("未发布 Local Entity 数按一递减"),
			Stats.PendingLocalPreparationEntityCount, 3 - Cycle);
			TestEqual(TEXT("TargetLocal 成本只随已发布部分增长"),
				Stats.TransitionLocalMeshPartCount, Cycle + 1);
			TestEqual(TEXT("Hot 发布与 Local 发布共用每周期一个 Entity 的预算"),
				Stats.HotPinMaintenanceEntityCount, 1);
			TestEqual(TEXT("Hot 引用只随已发布 Target 前缀增长"), Stats.HotPinnedEntityCount, Cycle + 1);
		}
	TestTrue(TEXT("发布完成后的下一周期原子提交刷新点"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	const FBuildPresentationSelectionStats FinalStats = Processor.GetSelectionStats();
	TestEqual(TEXT("完整发布后 TargetLocal 清空"), FinalStats.TransitionLocalMeshPartCount, 0);
	TestEqual(TEXT("完整发布后四个 Entity 都是 Required"), FinalStats.RequiredMeshPartCount, 4);
	TestEqual(TEXT("空旧 Local 提交周期不扫描 Hot Entity"), FinalStats.HotPinMaintenanceEntityCount, 0);
	TestEqual(TEXT("完整发布后四个 Hot 引用全部生效"), FinalStats.HotPinnedEntityCount, 4);
	TestEqual(TEXT("整个过程只运行一次 Local Worker 选择"),
		FinalStats.LocalSelectionPassCount, static_cast<uint64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationLocalCellInvalidationTest,
	"ElementSandbox.Building.Presentation.LocalCellInvalidationIsSourceScoped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationLocalCellInvalidationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 2;
	Config.StableResidentTargetMeshParts = 2;
	Config.TransitionReserveMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 4;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 1000.0;
	Config.GameplayChunkSize = 5000.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 8;
	Config.MinimumMeshPoolWorkBudgetParts = 8;
	Config.MaximumMeshPoolWorkBudgetParts = 8;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle A = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 6600);
	const FBuildEntityHandle B = AddStaticEntity(Registry, *Definition, FVector(4000.0, 0.0, 0.0), 6601);
	TestTrue(TEXT("创建初始 Local 静态基线"),
		A.IsSet() && B.IsSet()
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, A)
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, B));
	TestTrue(TEXT("批量写入初始 Local 索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("建立玩家 Local 驻留"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("提交初始玩家 Local"), Harness.Presentation->FlushNow());
	TestEqual(TEXT("初始只做一次 Local 选择"),
		Processor.GetSelectionStats().LocalSelectionPassCount, static_cast<uint64>(1));
	FMeshPoolInstanceHandle OriginalAHandle;
	TestTrue(TEXT("记录重合 Entity 的稳定实例句柄"), Processor.TryGetInstanceHandle(A, 0, OriginalAHandle));

	const FBuildEntityHandle Far = AddStaticEntity(Registry, *Definition, FVector(100000.0, 0.0, 0.0), 6602);
	TestTrue(TEXT("远处建筑登记到自己的 Cell"),
		Far.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Far));
	TestTrue(TEXT("写入远处静态批次"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("远处批次后继续投影"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("远处 Cell 变化不冲掉玩家 Local 缓存"),
		Processor.GetSelectionStats().LocalSelectionPassCount, static_cast<uint64>(1));
	TestEqual(TEXT("远处 Cell 变化不产生 Local 实例工作"),
		Processor.GetSelectionStats().LastCycleAddedMeshPartCount, 0);

	const FBuildEntityHandle Near = AddStaticEntity(Registry, *Definition, FVector(1000.0, 0.0, 0.0), 6603);
	TestTrue(TEXT("附近建筑登记到玩家覆盖 Cell"),
		Near.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Near));
	TestTrue(TEXT("写入附近静态批次"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("附近 Cell 命中后刷新 Local"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("附近 Cell 只触发一次新 Local 选择"),
		Processor.GetSelectionStats().LocalSelectionPassCount, static_cast<uint64>(2));
	TestEqual(TEXT("新 Local 仍保持两 Part 预算"),
		Processor.GetSelectionStats().RequiredMeshPartCount, 2);
	FMeshPoolInstanceHandle StableAHandle;
	TestTrue(TEXT("重合 Entity 不释放重加且句柄稳定"),
		Processor.TryGetInstanceHandle(A, 0, StableAHandle) && StableAHandle == OriginalAHandle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationLocalAppendOnlyResultTest,
	"ElementSandbox.Building.Presentation.LocalAppendOnlyResultAvoidsDoubleBufferRebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationLocalAppendOnlyResultTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 4;
	Config.StableResidentTargetMeshParts = 4;
	Config.TransitionReserveMeshParts = 4;
	Config.ResidentHardWatermarkMeshParts = 8;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 1000.0;
	Config.GameplayChunkSize = 5000.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 8;
	Config.MinimumMeshPoolWorkBudgetParts = 8;
	Config.MaximumMeshPoolWorkBudgetParts = 8;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle A = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 6700);
	const FBuildEntityHandle B = AddStaticEntity(Registry, *Definition, FVector(1000.0, 0.0, 0.0), 6701);
	TestTrue(TEXT("创建未填满预算的初始 Local"),
		A.IsSet() && B.IsSet()
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, A)
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, B));
	TestTrue(TEXT("写入初始追加测试索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("提交未填满的初始 Local"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FMeshPoolInstanceHandle OriginalAHandle;
	TestTrue(TEXT("记录追加前句柄"), Processor.TryGetInstanceHandle(A, 0, OriginalAHandle));

	const FBuildEntityHandle C = AddStaticEntity(Registry, *Definition, FVector(2000.0, 0.0, 0.0), 6702);
	const FBuildEntityHandle D = AddStaticEntity(Registry, *Definition, FVector(3000.0, 0.0, 0.0), 6703);
	TestTrue(TEXT("创建向外延伸的追加建筑"),
		C.IsSet() && D.IsSet()
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, C)
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, D));
	TestTrue(TEXT("写入向外追加批次"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("追加结果直接扩充 ActiveLocal"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	const FBuildPresentationSelectionStats Stats = Processor.GetSelectionStats();
	TestEqual(TEXT("追加批次只新增一次选择"), Stats.LocalSelectionPassCount, static_cast<uint64>(2));
	TestEqual(TEXT("追加后填满四 Part"), Stats.RequiredMeshPartCount, 4);
	TestEqual(TEXT("纯追加不建立全量 TargetLocal"), Stats.TransitionLocalMeshPartCount, 0);
	TestEqual(TEXT("纯追加只创建两个新实例"), Stats.LastCycleAddedMeshPartCount, 2);
	FMeshPoolInstanceHandle StableAHandle;
	TestTrue(TEXT("纯追加保持旧实例句柄"),
		Processor.TryGetInstanceHandle(A, 0, StableAHandle) && StableAHandle == OriginalAHandle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationFarCellInvalidationTest,
	"ElementSandbox.Building.Presentation.FarCellInvalidationUsesSectorBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationFarCellInvalidationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 2;
	Config.TransitionReserveMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 4;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 1000.0;
	Config.GameplayChunkSize = 5000.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 8;
	Config.MinimumMeshPoolWorkBudgetParts = 8;
	Config.MaximumMeshPoolWorkBudgetParts = 8;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle Local = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 6800);
	const FBuildEntityHandle InitialFar = AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 6801);
	TestTrue(TEXT("创建初始 Local/Far 目标"),
		Local.IsSet() && InitialFar.IsSet()
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, Local)
			&& MarkPackedStaticCreated(Registry, Dirty, *Definition, InitialFar));
	TestTrue(TEXT("写入初始 Far 订阅索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("建立已提交 Far 扇区"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("初始只做一次 Far 选择"),
		Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));
	TestEqual(TEXT("初始 Far 已晋升"), Processor.GetSelectionStats().ActiveFarMeshPartCount, 1);

	const FBuildEntityHandle Behind = AddStaticEntity(Registry, *Definition, FVector(-10000.0, 0.0, 0.0), 6802);
	TestTrue(TEXT("创建扇区背后的建筑"),
		Behind.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Behind));
	TestTrue(TEXT("写入扇区背后批次"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("背后批次后继续投影"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("扇区外 Cell 不触发 Far 重选"),
		Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));

	const FBuildEntityHandle BeyondBoundary =
		AddStaticEntity(Registry, *Definition, FVector(100000.0, 0.0, 0.0), 6803);
	TestTrue(TEXT("创建扇区内但边界外的建筑"),
		BeyondBoundary.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, BeyondBoundary));
	TestTrue(TEXT("写入 Far 边界外批次"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("边界外批次后继续投影"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("已填满 Far 的边界外 Cell 不触发重选"),
		Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));

	const FBuildEntityHandle InsideBoundary =
		AddStaticEntity(Registry, *Definition, FVector(5000.0, 0.0, 0.0), 6804);
	TestTrue(TEXT("创建可能改写 Far 目标的建筑"),
		InsideBoundary.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, InsideBoundary));
	TestTrue(TEXT("写入 Far 边界内批次"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("边界内批次刷新 Far"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("边界内 Cell 只触发一次 Far 重选"),
		Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(2));
	TestEqual(TEXT("刷新后 Far 仍保持一 Part"), Processor.GetSelectionStats().ActiveFarMeshPartCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationFarStreamingSettleTest,
	"ElementSandbox.Building.Presentation.FarStreamingWaitsForContentSettle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationFarStreamingSettleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 2;
	Config.TransitionReserveMeshParts = 2;
	Config.ResidentHardWatermarkMeshParts = 4;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 1000.0;
	Config.GameplayChunkSize = 5000.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.20;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 8;
	Config.MinimumMeshPoolWorkBudgetParts = 8;
	Config.MaximumMeshPoolWorkBudgetParts = 8;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle Local = AddStaticEntity(Registry, *Definition, FVector::ZeroVector, 6850);
	const FBuildEntityHandle InitialFar =
		AddStaticEntity(Registry, *Definition, FVector(10000.0, 0.0, 0.0), 6851);
	TestTrue(TEXT("创建流式停稳测试的 Local/Far"),
		Local.IsSet() && InitialFar.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Local) &&
			MarkPackedStaticCreated(Registry, Dirty, *Definition, InitialFar));
	TestTrue(TEXT("写入初始索引"), Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("先发布 Local"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	Processor.SetResidencyTimeSecondsForTesting(0.20);
	TestTrue(TEXT("初始内容停稳后发布 Far"), Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("提交初始稳定实例"), Harness.Presentation->FlushNow());
	TestEqual(TEXT("初始 Far 只选择一次"), Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));
	TestEqual(TEXT("初始 Far 已稳定"), Processor.GetSelectionStats().ActiveFarMeshPartCount, 1);
	FMeshPoolInstanceHandle InitialFarHandle;
	TestTrue(TEXT("记录初始 Far 稳定句柄"), Processor.TryGetInstanceHandle(InitialFar, 0, InitialFarHandle));
	const uint64 FlushCountBeforeStreaming = Harness.Presentation->GetMeshPoolStats().TotalFlushedInstanceCount;

	FBuildEntityHandle ClosestFar;
	const TPair<double, double> StreamingSteps[] = {
		{0.30, 7500.0},
		{0.40, 6500.0},
		{0.50, 5500.0},
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(StreamingSteps); ++Index)
	{
		Processor.SetResidencyTimeSecondsForTesting(StreamingSteps[Index].Key);
		const FBuildEntityHandle Added = AddStaticEntity(
			Registry, *Definition, FVector(StreamingSteps[Index].Value, 0.0, 0.0), 6852 + Index);
		ClosestFar = Added;
		TestTrue(TEXT("持续流式批次进入相关 Far Cell"),
			Added.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Added));
		TestTrue(TEXT("持续流式批次写入表现索引"),
			Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
		TestTrue(TEXT("内容静默窗内继续投影"),
			Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
		TestTrue(TEXT("静默窗内 MeshPool 保持空提交"), Harness.Presentation->FlushNow());
		TestEqual(TEXT("持续变化期间不重复 Far 选择"),
			Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(1));
		FMeshPoolInstanceHandle StableHandle;
		TestTrue(TEXT("持续变化期间旧 Far 不消失不换句柄"),
			Processor.TryGetInstanceHandle(InitialFar, 0, StableHandle) && StableHandle == InitialFarHandle);
		TestEqual(TEXT("持续变化期间没有实例 Add/Remove churn"),
			Harness.Presentation->GetMeshPoolStats().TotalFlushedInstanceCount, FlushCountBeforeStreaming);
	}

	Processor.SetResidencyTimeSecondsForTesting(0.71);
	TestTrue(TEXT("最后变化静默 200ms 后只刷新一次 Far"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("提交收敛后的 Far 换代"), Harness.Presentation->FlushNow());
	TestEqual(TEXT("三次相关批次最终只增加一次 Far 选择"),
		Processor.GetSelectionStats().SelectionPassCount, static_cast<uint64>(2));
	FMeshPoolInstanceHandle ClosestHandle;
	TestTrue(TEXT("最终选择接管最近 Far"), Processor.TryGetInstanceHandle(ClosestFar, 0, ClosestHandle));
	TestFalse(TEXT("收敛后旧 Far 才退出"), Processor.TryGetInstanceHandle(InitialFar, 0, InitialFarHandle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationLiveHotInsertBypassesLocalBacklogTest,
	"ElementSandbox.Building.Presentation.LiveHotInsertBypassesLocalBacklog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationLiveHotInsertBypassesLocalBacklogTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	UPresentationSettings* Settings = GetMutableDefault<UPresentationSettings>();
	const int32 PreviousBatchSize = Settings->MaximumNativeInstanceBatchSize;
	const double PreviousTimeBudget = Settings->InstanceApplyTargetMilliseconds;
	Settings->MaximumNativeInstanceBatchSize = 1;
	Settings->InstanceApplyTargetMilliseconds = 0.0;
	ON_SCOPE_EXIT
	{
		Settings->MaximumNativeInstanceBatchSize = PreviousBatchSize;
		Settings->InstanceApplyTargetMilliseconds = PreviousTimeBudget;
	};
	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 10000.0;
	Config.LocalResidentTargetMeshParts = 32;
	Config.StableResidentTargetMeshParts = 32;
	Config.TransitionReserveMeshParts = 8;
	Config.ResidentHardWatermarkMeshParts = 64;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 1000.0;
	Config.SourceMovementThreshold = 1000.0;
	Config.GameplayChunkSize = 5000.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 1;
	Config.MinimumMeshPoolWorkBudgetParts = 1;
	Config.MaximumMeshPoolWorkBudgetParts = 1;
	Config.LocalTransitionPublishBudgetEntitiesPerCycle = 8;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	for (uint64 Index = 0; Index < 32; ++Index)
	{
		const FBuildEntityHandle Entity =
			AddStaticEntity(Registry, *Definition, FVector(2000.0 + static_cast<double>(Index) * 500.0, 0.0, 0.0),
				7000 + Index);
		if (!TestTrue(TEXT("创建初始 Local backlog"),
			Entity.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Entity)))
		{
			return false;
		}
	}
	TestTrue(TEXT("写入初始 Local backlog"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("只推进一个初始 Local Entity"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("初始 Local 仍有准备 backlog"),
		Processor.GetSelectionStats().PendingLocalPreparationEntityCount > 0);

	FMeshPoolClusterKey BackgroundCluster;
	BackgroundCluster.Layer = Harness.Layer;
	BackgroundCluster.Mesh = Mesh;
	BackgroundCluster.Cell = FIntVector(10, 0, 0);
	BackgroundCluster.Backend = EMeshPoolBackend::HierarchicalStatic;
	TArray<FMeshPoolInstanceHandle> Background;
	for (int32 Index = 0; Index < 256; ++Index)
	{
		Background.Add(Harness.Presentation->QueueAdd(BackgroundCluster, FTransform(FVector(Index * 100.0, 0.0, 0.0))));
	}
	const FBuildEntityHandle LiveHot = AddStaticEntity(Registry, *Definition, FVector(100.0, 0.0, 0.0), 7100);
	TestTrue(TEXT("插入玩家脚边的 Live Building"),
		LiveHot.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, LiveHot));
	TestTrue(TEXT("Live Building 写入表现索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("Local backlog 未排空时继续投影"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	FMeshPoolInstanceHandle LiveHandle;
	TestTrue(TEXT("脚边 Live Building 不等待全量 Local 重选即可获得实例"),
		Processor.TryGetInstanceHandle(LiveHot, 0, LiveHandle));
	TestTrue(TEXT("验证时全量 Local backlog 仍存在"),
		Processor.GetSelectionStats().PendingLocalPreparationEntityCount > 0);
	TestFalse(TEXT("排队后尚未实际提交"), Harness.Presentation->IsInstancePhysicallyResident(LiveHandle));
	Harness.Presentation->Tick(1.0f / 60.0f);
	TestTrue(TEXT("只有一个提交预算时新建墙先于双层积压实际进入组件"),
		Harness.Presentation->IsInstancePhysicallyResident(LiveHandle));
	TestFalse(TEXT("背景装填仍在等待，未通过清空队列伪造插队"),
		Harness.Presentation->IsInstancePhysicallyResident(Background[0]));
	TestEqual(TEXT("插队仍遵守每帧实例预算"),
		Harness.Presentation->GetMeshPoolStats().LastFlushInstanceCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationResidencyTombstoneDuringTargetPreparationTest,
	"ElementSandbox.Building.Presentation.ResidencyTombstoneDuringTargetPreparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationResidencyTombstoneDuringTargetPreparationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	constexpr int32 EntityCount = 512;
	constexpr int32 DestroyStride = 4;
	constexpr int32 DestroyedCount = EntityCount / DestroyStride;
	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 5000.0;
	Config.LocalResidentTargetMeshParts = EntityCount;
	Config.StableResidentTargetMeshParts = EntityCount;
	Config.TransitionReserveMeshParts = EntityCount;
	Config.ResidentHardWatermarkMeshParts = EntityCount * 2;
	Config.EmergencyOverflowMeshParts = 0;
	Config.HotPromotionRadius = 0.0;
	Config.SourceMovementThreshold = 1000.0;
	Config.GameplayChunkSize = 5000.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	Config.InitialMeshPoolWorkBudgetParts = 64;
	Config.MinimumMeshPoolWorkBudgetParts = 64;
	Config.MaximumMeshPoolWorkBudgetParts = 64;
	Config.LocalTransitionPublishBudgetEntitiesPerCycle = 8;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	Processor.SetResidencyTimeSecondsForTesting(0.0);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	TArray<FBuildEntityHandle> Entities;
	Entities.Reserve(EntityCount);
	for (int32 EntityIndex = 0; EntityIndex < EntityCount; ++EntityIndex)
	{
		const FVector Location(
			static_cast<double>(EntityIndex % 32) * 100.0,
			static_cast<double>(EntityIndex / 32) * 100.0,
			0.0);
		const FBuildEntityHandle Entity = AddStaticEntity(Registry, *Definition, Location, 910000 + EntityIndex);
		Entities.Add(Entity);
		if (!TestTrue(TEXT("创建 TargetLocal 压力 Entity"),
			Entity.IsSet() && MarkPackedStaticCreated(Registry, Dirty, *Definition, Entity)))
		{
			return false;
		}
	}
	TestTrue(TEXT("批量建立 TargetLocal 压力索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("第一周期只发布 TargetLocal 前缀"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("销毁前 TargetLocal 仍在 preparation 中"),
		Processor.GetSelectionStats().PendingLocalPreparationEntityCount > 0);

	for (int32 EntityIndex = 0; EntityIndex < EntityCount; EntityIndex += DestroyStride)
	{
		TestTrue(TEXT("TargetLocal preparation 中批量销毁 Entity"), Registry.DestroyEntity(Entities[EntityIndex]));
		Dirty.MarkRebuild(Entities[EntityIndex]);
	}
	TestTrue(TEXT("批量 Tombstone 即时退出表现与全局引用"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	for (int32 EntityIndex = 0; EntityIndex < EntityCount; EntityIndex += DestroyStride)
	{
		FMeshPoolInstanceHandle Instance;
		TestFalse(TEXT("Tombstone 同步调用栈内立即移除已投影实例"),
			Processor.TryGetInstanceHandle(Entities[EntityIndex], 0, Instance));
	}

	for (int32 EntityIndex = 0; EntityIndex < EntityCount; EntityIndex += DestroyStride)
	{
		Dirty.MarkRebuild(Entities[EntityIndex]);
	}
	TestTrue(TEXT("重复 Tombstone 幂等"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));

	constexpr int32 MaximumConvergenceCycles = 256;
	int32 Cycle = 0;
	for (; Cycle < MaximumConvergenceCycles && Processor.HasPendingProjectionWork(); ++Cycle)
	{
		Processor.SetResidencyTimeSecondsForTesting(0.01 * static_cast<double>(Cycle + 1));
		if (!TestTrue(TEXT("Tombstone 后继续预算化发布直到收敛"),
			Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer)))
		{
			return false;
		}
	}
	TestFalse(TEXT("TargetLocal Tombstone 后在有界周期内收敛"), Processor.HasPendingProjectionWork());
	const FBuildPresentationSelectionStats FinalStats = Processor.GetSelectionStats();
	TestEqual(TEXT("收敛后只保留存活 Entity 的 Required 引用"),
		FinalStats.RequiredEntityCount, EntityCount - DestroyedCount);
	TestEqual(TEXT("收敛后 Required 成本与存活 Entity 数一致"),
		FinalStats.RequiredMeshPartCount, EntityCount - DestroyedCount);
	TestEqual(TEXT("收敛后 TargetLocal 清空"), FinalStats.TransitionLocalMeshPartCount, 0);
	TestEqual(TEXT("无 Hot 配置时引用计数保持为零"), FinalStats.HotPinnedEntityCount, 0);
	TestEqual(TEXT("Tombstone 后执行一次完整 Local 快照换代"),
		FinalStats.LocalSelectionPassCount, static_cast<uint64>(2));
	TestEqual(TEXT("所有存活 Entity 都已完成逻辑表现投影"),
		FinalStats.ResidentEntityCount, EntityCount - DestroyedCount);
	for (int32 EntityIndex = 0; EntityIndex < EntityCount; EntityIndex += DestroyStride)
	{
		FMeshPoolInstanceHandle Instance;
		TestFalse(TEXT("销毁 Entity 不会被 stale selection 重新投影"),
			Processor.TryGetInstanceHandle(Entities[EntityIndex], 0, Instance));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPresentationPhysicalRetirementCausalGateTest,
	"ElementSandbox.Building.Presentation.PhysicalRetirementPrecedesCausalRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPresentationPhysicalRetirementCausalGateTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Tests;
	FBuildPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Presentation || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	FBuildPresentationResidencyConfig Config;
	Config.MinimumLocalRadius = 0.0;
	Config.LocalResidentTargetMeshParts = 1;
	Config.StableResidentTargetMeshParts = 1;
	Config.ResidentHardWatermarkMeshParts = 1;
	Config.HotPromotionRadius = 0.0;
	Config.GameplayChunkPadding = 0.0;
	Config.FarSettleSeconds = 0.0;
	Config.PromotionStableSeconds = 0.0;
	Config.UnstablePromotionLockSeconds = 0.0;
	Config.RapidRotationThresholdDegreesPerSecond = 1.0e12;
	FBuildRenderProcessor Processor(Config);
	Processor.SetResidencySelectionSynchronousForTesting(true);
	FBuildEntityRegistry Registry;
	FBuildRenderDirtySet Dirty;
	UBuildTestDefinition* Definition = MakePresentationDefinition(Harness.World, *Mesh);
	const FBuildEntityHandle Entity = AddStaticEntity(
		Registry, *Definition, FVector::ZeroVector, 900001);
	Dirty.MarkRebuild(Entity);
	TestTrue(TEXT("建立源建筑表现索引"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	FPresentationViewSnapshot Views;
	Views.Sources.Add(MakeView(FVector::ZeroVector, FVector::ForwardVector));
	TestTrue(TEXT("选择源建筑为 Resident"),
		Processor.Project(Registry, Views, *Harness.Presentation, Harness.Layer));
	TestTrue(TEXT("提交源建筑物理实例"), Harness.Presentation->FlushNow());

	FMeshPoolInstanceHandle Instance;
	TestTrue(TEXT("取得已提交源实例"), Processor.TryGetInstanceHandle(Entity, 0, Instance));
	TestTrue(TEXT("销毁前实例确实物理驻留"), Harness.Presentation->IsInstancePhysicallyResident(Instance));
	const FDelegateHandle RetiredHandle = Harness.Presentation->OnInstanceRetired().AddLambda(
		[&Processor](const FMeshPoolInstanceHandle Retired)
		{
			Processor.NotifyInstanceRetired(Retired);
		});

	TestTrue(TEXT("销毁源 Entity"), Registry.DestroyEntity(Entity));
	Dirty.MarkRebuild(Entity);
	TestTrue(TEXT("逻辑删除只排队物理退休"),
		Processor.Execute(Registry, Dirty, *Harness.Presentation, Harness.Layer));
	TestEqual(TEXT("逻辑 Resident 记录立即退出"), Processor.GetRenderedPartCount(Entity), 0);
	TestTrue(TEXT("物理 Flush 前因果门仍保持关闭"), Processor.HasRetiringInstances(Entity));
	TestTrue(TEXT("物理 Flush 前旧实例仍可见"), Harness.Presentation->IsInstancePhysicallyResident(Instance));

	Harness.Presentation->Tick(0.0f);
	TestFalse(TEXT("退休事件后因果门才放行"), Processor.HasRetiringInstances(Entity));
	TestFalse(TEXT("退休事件对应旧实例已物理退出"), Harness.Presentation->IsInstancePhysicallyResident(Instance));
	Harness.Presentation->OnInstanceRetired().Remove(RetiredHandle);
	return true;
}

#endif
