#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "BuildingCatalogWorldSubsystem.h"
#include "BuildingWorldSubsystem.h"
#include "Collision/BuildCollisionHost.h"
#include "Collision/BuildCollisionTypes.h"
#include "Containers/Ticker.h"
#include "Door/DoorBuildingDefinition.h"
#include "Door/DoorProcessor.h"
#include "Door/DoorStateFragment.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "MeshPoolRenderHost.h"
#include "Misc/AutomationTest.h"
#include "WorldStorageSubsystem.h"
#include "PresentationWorldSubsystem.h"
#include "Rendering/BuildRenderTypes.h"
#include "Spatial/BuildSpatialIndex.h"

namespace ElementSandbox::BuildingCatalog::Door::Tests
{
	FBuildCollisionSource MakeDoorCollisionSource(
		const FVector& SubjectLocation,
		const uint64 Revision)
	{
		FBuildCollisionSource Source;
		Source.SubjectLocation = SubjectLocation;
		Source.ImmediateBounds = FBox::BuildAABB(
			SubjectLocation,
			FVector(400.0));
		Source.PrefetchBounds = Source.ImmediateBounds;
		Source.CameraBounds = Source.ImmediateBounds;
		Source.RetentionBounds = FBox::BuildAABB(
			SubjectLocation,
			FVector(700.0));
		Source.Revision = Revision;
		return Source;
	}

	void RunBuildingPostActorTick(
		UWorld& World,
		const float DeltaSeconds =
			static_cast<float>(UWorldStorageSubsystem::AuthorityTickIntervalSeconds))
	{
		FWorldDelegates::OnWorldPostActorTick.Broadcast(
			&World,
			LEVELTICK_All,
			DeltaSeconds);
	}

	double GetBuildingAuthorityTimeSeconds(UWorld& World)
	{
		const UWorldStorageSubsystem* Storage = World.GetSubsystem<UWorldStorageSubsystem>();
		return Storage
			? static_cast<double>(Storage->GetWorldSimulationTimeMilliseconds()) / 1000.0
				: World.GetTimeSeconds();
	}

	bool DrainPresentationTreeBuilds(UPresentationWorldSubsystem* Presentation)
	{
		AMeshPoolRenderHost* Host = Presentation ? Presentation->GetRenderHost() : nullptr;
		if (!Host)
		{
			return Presentation != nullptr;
		}

		// HISM 的树构建是异步且全局串行的。测试必须等待前一边界真正排空，
		// 否则低性能机器会把下一次门状态迁移合并进仍在执行的旧树构建。
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (Host->HasDeferredTreeBuilds() && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			Host->ProcessDeferredTreeBuilds(FPlatformTime::Seconds(), 0.0, 0.0, true);
			FTSTicker::GetCoreTicker().Tick(0.005f);
			FPlatformProcess::Sleep(0.005f);
		}
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.0f);
		return !Host->HasDeferredTreeBuilds();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorProcessorAdvancesStableStateTest,
	"ElementSandbox.BuildingCatalog.Door.ProcessorAdvancesStableState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorProcessorAdvancesStableStateTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::BuildingCatalog::Door::Tests;
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("DoorProcessor"),
		nullptr,
		true);
	TestNotNull(TEXT("创建 Door Processor 测试 World"), World);
	if (!World)
	{
		return false;
	}

	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* BuildingSubsystem =
		World->GetSubsystem<UBuildingWorldSubsystem>();
	UBuildingCatalogWorldSubsystem* CatalogSubsystem =
		World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	UDoorBuildingDefinition* Definition =
		BuildingSubsystem
			? Cast<UDoorBuildingDefinition>(
				BuildingSubsystem->FindDefinition(TEXT("Door")))
			: nullptr;
	const FBuildEntityHandle Door = BuildingSubsystem
		? BuildingSubsystem->CreateEntity(
			*Definition,
			FTransform::Identity,
			EBuildSpatialMobility::Static)
		: FBuildEntityHandle();
	TestTrue(TEXT("创建保守 Static Door Entity"), Door.IsSet());
		TestTrue(TEXT("Catalog 注册 Authority Door Processor"),
			CatalogSubsystem && CatalogSubsystem->HasAuthorityDoorProcessor());
		if (!BuildingSubsystem || !CatalogSubsystem || !Door.IsSet())
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}
	UPresentationWorldSubsystem* Presentation =
		World->GetSubsystem<UPresentationWorldSubsystem>();
	FPresentationViewSource View;
		View.ViewLocation = FVector(-20000.0, 0.0, 0.0);
		View.SubjectLocation = View.ViewLocation;
	View.Forward = FVector::ForwardVector;
	View.Right = FVector::RightVector;
	View.Up = FVector::UpVector;
	View.Revision = 1;
	const FPresentationSourceHandle PresentationSource = Presentation
		? Presentation->RegisterSource(View)
		: FPresentationSourceHandle();
	TestTrue(TEXT("注册远场相机 Source"), PresentationSource.IsSet());

	TestTrue(TEXT("初次投影 Door 表现"), BuildingSubsystem->FlushRenderChanges());
	TestTrue(TEXT("初次 Door HISM Tree Build 已排空"),
		DrainPresentationTreeBuilds(Presentation));
	const FBuildCollisionSourceHandle CollisionSource =
		BuildingSubsystem->RegisterCollisionSource(
			MakeDoorCollisionSource(FVector::ZeroVector, 1));
	TestTrue(TEXT("为 Door 注册局部 Collision Source"),
			CollisionSource.IsSet());
	TestTrue(TEXT("初次投影 Door Collision"),
		BuildingSubsystem->FlushCollisionChanges());
	TestEqual(TEXT("Door 只投影一个门扇和三个门框 Body"),
		BuildingSubsystem->GetActiveCollisionBodyCount(), 4);
	FBuildCollisionInstanceHandle LeafCollision;
	FBuildCollisionInstanceHandle LeftFrameCollision;
	TestTrue(TEXT("读取门扇 Collision Handle"),
		BuildingSubsystem->TryGetPartCollisionInstance(Door, 0, LeafCollision));
	TestTrue(TEXT("读取门框 Collision Handle"),
		BuildingSubsystem->TryGetPartCollisionInstance(Door, 1, LeftFrameCollision));
	FTransform ClosedLeafCollisionTransform;
	FTransform ClosedFrameCollisionTransform;
	ABuildCollisionHost* CollisionHost = BuildingSubsystem->GetCollisionHost();
	TestTrue(TEXT("读取关闭门扇 Collision Transform"),
		CollisionHost
		&& CollisionHost->TryGetInstanceTransform(
			LeafCollision,
			ClosedLeafCollisionTransform));
	TestTrue(TEXT("读取门框 Collision Transform"),
		CollisionHost
		&& CollisionHost->TryGetInstanceTransform(
			LeftFrameCollision,
			ClosedFrameCollisionTransform));
	TestTrue(TEXT("Door 初始 Static Delta 可进入 Snapshot"),
		BuildingSubsystem->GetSpatialIndex().RebuildDirtyStaticChunks() > 0);
	TestEqual(TEXT("Door 动画前 Static Snapshot 已干净"),
		BuildingSubsystem->GetSpatialIndex().GetDirtyStaticChunkCount(), 0);
	const uint64 TreeBuildsBeforeOpening = BuildingSubsystem->GetHierarchicalTreeBuildRequestCount();
	FBuildEntityRegistry& Registry = BuildingSubsystem->GetRegistry();
	FBox ClosedBounds(ForceInit);
	TestTrue(TEXT("读取关闭时 Bounds"),
		BuildingSubsystem->GetSpatialIndex().TryGetBounds(Door, ClosedBounds));

	TestTrue(TEXT("稳定关闭态向 Door 活跃队列提交一次请求"),
		CatalogSubsystem->RequestDoorInteraction(Door));
	TestFalse(TEXT("同一 Door 活跃期间拒绝重复请求"),
		CatalogSubsystem->RequestDoorInteraction(Door));
		RunBuildingPostActorTick(*World);
		TestTrue(TEXT("开门晋升 HISM Tree Build 已排空"),
			DrainPresentationTreeBuilds(Presentation));
		FBuildDoorStateFragment* DoorState =
			Registry.FindMutableFragment<FBuildDoorStateFragment>(Door);
		TestTrue(TEXT("关闭态进入 Opening"),
			DoorState && DoorState->State == EBuildDoorState::Opening);
	TestTrue(TEXT("开门运动开始时钉住 Hot ISM"),
		BuildingSubsystem->IsPresentationMotionActive(Door));
		TestFalse(TEXT("开门首帧姿态未变化时不重复提交 Collision Body"),
			BuildingSubsystem->HasPendingCollisionWork());
	const uint64 TreeBuildsAfterOpeningPromotion = BuildingSubsystem->GetHierarchicalTreeBuildRequestCount();
		TestTrue(TEXT("开门晋升边界每个运动 Part 至多对应一次 HISM 树请求"),
			TreeBuildsAfterOpeningPromotion - TreeBuildsBeforeOpening <= 4);
	EBuildRenderStorageClass MovingStorage = EBuildRenderStorageClass::StaticHISM;
	EBuildRenderStorageClass FrameStorage = EBuildRenderStorageClass::HotISM;
	TestTrue(TEXT("运动门扇已晋升 Hot ISM"),
		BuildingSubsystem->TryGetPartRenderStorageClass(Door, 0, MovingStorage)
		&& MovingStorage == EBuildRenderStorageClass::HotISM);
	TestTrue(TEXT("门框始终留在 Static HISM"),
		BuildingSubsystem->TryGetPartRenderStorageClass(Door, 1, FrameStorage)
		&& FrameStorage == EBuildRenderStorageClass::StaticHISM);

	if (DoorState)
	{
		DoorState->TransitionStartServerTimeSeconds = GetBuildingAuthorityTimeSeconds(*World) - 0.2;
	}
	RunBuildingPostActorTick(*World);
	const FBuildPartTransformFragment* PartTransforms =
		Registry.FindFragment<FBuildPartTransformFragment>(Door);
	const FTransform QuarterOpenTransform = Definition->MeshParts[0].LocalTransform
		* FBuildDoorProcessor::CalculateDoorMotion(0.25f);
	TestTrue(TEXT("门扇开度由权威切换时间派生"),
		PartTransforms
		&& PartTransforms->LocalTransforms[0].Equals(QuarterOpenTransform));
	FTransform QuarterOpenLeafCollisionTransform;
	FTransform MidAnimationFrameCollisionTransform;
	TestTrue(TEXT("Chaos 门扇 Body 跟随当前真实 Part Transform"),
		CollisionHost
		&& CollisionHost->TryGetInstanceTransform(
			LeafCollision,
			QuarterOpenLeafCollisionTransform)
		&& !QuarterOpenLeafCollisionTransform.Equals(
			ClosedLeafCollisionTransform));
	TestTrue(TEXT("门框 Body 在动画中保持不变"),
		CollisionHost
		&& CollisionHost->TryGetInstanceTransform(
			LeftFrameCollision,
			MidAnimationFrameCollisionTransform)
		&& MidAnimationFrameCollisionTransform.Equals(
			ClosedFrameCollisionTransform));
	TestEqual(TEXT("动画中段仍只更新门扇一个 Collision Body"),
		BuildingSubsystem->GetLastChangedCollisionPartCount(), 1);
	TestEqual(TEXT("开门动画中段不触发 HISM Tree Build"),
		BuildingSubsystem->GetHierarchicalTreeBuildRequestCount(),
		TreeBuildsAfterOpeningPromotion);

	DoorState = Registry.FindMutableFragment<FBuildDoorStateFragment>(Door);
	if (DoorState)
	{
		DoorState->TransitionStartServerTimeSeconds = GetBuildingAuthorityTimeSeconds(*World) - 1.0;
	}
	RunBuildingPostActorTick(*World);
	DoorState = Registry.FindMutableFragment<FBuildDoorStateFragment>(Door);
		TestTrue(TEXT("开门结束落入 Open 稳定态"),
			DoorState && DoorState->State == EBuildDoorState::Open);
	TestFalse(TEXT("开门结束解除运动钉住"),
		BuildingSubsystem->IsPresentationMotionActive(Door));
	TestTrue(TEXT("8Hz 边界提交开门后的 Cold HISM 迁移"),
		BuildingSubsystem->FlushRenderChanges());
	TestTrue(TEXT("开门降级 HISM Tree Build 已排空"),
		DrainPresentationTreeBuilds(Presentation));
	TestEqual(TEXT("有近场 Chunk 引用时解除 Pin 仍保留四个 Body"),
		BuildingSubsystem->GetActiveCollisionBodyCount(), 4);
	const uint64 TreeBuildsAfterOpeningDemotion = BuildingSubsystem->GetHierarchicalTreeBuildRequestCount();
		TestTrue(TEXT("开门降级边界只为实际目标 HISM Cluster 建树"),
			TreeBuildsAfterOpeningDemotion > TreeBuildsAfterOpeningPromotion
				&& TreeBuildsAfterOpeningDemotion - TreeBuildsAfterOpeningPromotion <= 4);
	TestTrue(TEXT("开门稳定后远处门扇回到 Cold HISM"),
		BuildingSubsystem->TryGetPartRenderStorageClass(Door, 0, MovingStorage)
		&& MovingStorage == EBuildRenderStorageClass::ColdPromotableHISM);

	PartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Door);
	TestTrue(TEXT("门扇随铰链旋转"),
		PartTransforms
		&& !PartTransforms->LocalTransforms[0].Equals(
			Definition->MeshParts[0].LocalTransform));
	TestTrue(TEXT("门扇向背离默认玩家站位的本地 +X 开启"),
		PartTransforms
		&& PartTransforms->LocalTransforms[0].GetLocation().X > 0.0);
	TestTrue(TEXT("门框保持原始局部 Transform"),
		PartTransforms
		&& PartTransforms->LocalTransforms[1].Equals(
			Definition->MeshParts[1].LocalTransform));
	FBox OpenBounds(ForceInit);
	TestTrue(TEXT("读取打开时 Bounds"),
		BuildingSubsystem->GetSpatialIndex().TryGetBounds(Door, OpenBounds));
	TestTrue(TEXT("Swept BVH Bounds 不随门扇动画变化"),
		OpenBounds.Equals(ClosedBounds));
	TestEqual(TEXT("Door 动画不改变 BVH Static Version/Dirty 状态"),
		BuildingSubsystem->GetSpatialIndex().GetDirtyStaticChunkCount(), 0);
	TestEqual(TEXT("动画不重建或丢失 Door 的七个 Instance"),
		BuildingSubsystem->GetRenderedInstanceCount(),
		7);

	TestTrue(TEXT("Open 稳定态接收关门请求"),
		CatalogSubsystem->RequestDoorInteraction(Door));
	RunBuildingPostActorTick(*World);
	TestTrue(TEXT("关门晋升 HISM Tree Build 已排空"),
		DrainPresentationTreeBuilds(Presentation));
	DoorState = Registry.FindMutableFragment<FBuildDoorStateFragment>(Door);
		TestTrue(TEXT("Open 请求进入 Closing"),
			DoorState && DoorState->State == EBuildDoorState::Closing);
	const uint64 TreeBuildsAfterClosingPromotion = BuildingSubsystem->GetHierarchicalTreeBuildRequestCount();
		TestTrue(TEXT("关门晋升边界空 HISM 可直接回收且不重复建树"),
			TreeBuildsAfterClosingPromotion - TreeBuildsAfterOpeningDemotion <= 4);

	if (DoorState)
	{
		DoorState->TransitionStartServerTimeSeconds = GetBuildingAuthorityTimeSeconds(*World) - 0.4;
	}
	RunBuildingPostActorTick(*World);
	PartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Door);
	const FTransform HalfOpenTransform = Definition->MeshParts[0].LocalTransform
		* FBuildDoorProcessor::CalculateDoorMotion(0.5f);
	TestTrue(TEXT("Closing 半程同样只派生门扇 Transform"),
		PartTransforms
		&& PartTransforms->LocalTransforms[0].Equals(HalfOpenTransform));
	TestEqual(TEXT("关门动画中段不触发 HISM Tree Build"),
		BuildingSubsystem->GetHierarchicalTreeBuildRequestCount(),
		TreeBuildsAfterClosingPromotion);

	DoorState = Registry.FindMutableFragment<FBuildDoorStateFragment>(Door);
	if (DoorState)
	{
		DoorState->TransitionStartServerTimeSeconds = GetBuildingAuthorityTimeSeconds(*World) - 1.0;
	}
	RunBuildingPostActorTick(*World);
	DoorState = Registry.FindMutableFragment<FBuildDoorStateFragment>(Door);
	PartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Door);
		TestTrue(TEXT("关门结束落入 Closed 稳定态"),
			DoorState && DoorState->State == EBuildDoorState::Closed);
	TestTrue(TEXT("关闭后门扇恢复定义 Transform"),
		PartTransforms
			&& PartTransforms->LocalTransforms[0].Equals(
				Definition->MeshParts[0].LocalTransform));
	TestTrue(TEXT("8Hz 边界提交关门后的 Cold HISM 迁移"),
		BuildingSubsystem->FlushRenderChanges());
	TestTrue(TEXT("关门降级 HISM Tree Build 已排空"),
		DrainPresentationTreeBuilds(Presentation));
	const uint64 TreeBuildsAfterClosingDemotion = BuildingSubsystem->GetHierarchicalTreeBuildRequestCount();
		TestTrue(TEXT("关门降级边界只为实际目标 HISM Cluster 建树"),
			TreeBuildsAfterClosingDemotion > TreeBuildsAfterClosingPromotion
				&& TreeBuildsAfterClosingDemotion - TreeBuildsAfterClosingPromotion <= 4);
	TestEqual(TEXT("完整开关动画始终只有四个 Door Body"),
			BuildingSubsystem->GetActiveCollisionBodyCount(), 4);
	TestTrue(TEXT("注销 Door Collision Source"),
			BuildingSubsystem->UnregisterCollisionSource(CollisionSource));
	TestTrue(TEXT("注销 Source 后开始 Door Collision Grace"),
		BuildingSubsystem->FlushCollisionChanges(0.0));
	TestTrue(TEXT("Door Collision Grace 期间仍可 Flush"),
		BuildingSubsystem->FlushCollisionChanges(2.9));
	TestEqual(TEXT("2.9 秒 Grace 内仍保留 Door Body"),
		BuildingSubsystem->GetActiveCollisionBodyCount(),
		4);
	TestTrue(TEXT("3.1 秒后批量卸载 Door Collision"),
		BuildingSubsystem->FlushCollisionChanges(3.1));
	TestEqual(TEXT("无 Source 后 Door Body 全部卸载"),
			BuildingSubsystem->GetActiveCollisionBodyCount(), 0);
	if (Presentation && PresentationSource.IsSet())
	{
		Presentation->UnregisterSource(PresentationSource);
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorProcessorFarStateCreatesCollisionOnApproachTest,
	"ElementSandbox.BuildingCatalog.Door.FarStateCreatesCollisionOnApproach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorProcessorFarStateCreatesCollisionOnApproachTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::BuildingCatalog::Door::Tests;
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("DoorFarStateCollision"),
		nullptr,
		true);
	if (!World)
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* Building =
		World->GetSubsystem<UBuildingWorldSubsystem>();
	UBuildingCatalogWorldSubsystem* Catalog =
		World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	UPresentationWorldSubsystem* Presentation =
		World->GetSubsystem<UPresentationWorldSubsystem>();
	UDoorBuildingDefinition* Definition = Building
		? Cast<UDoorBuildingDefinition>(Building->FindDefinition(TEXT("Door")))
		: nullptr;
	if (!Building || !Catalog || !Presentation || !Definition)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	FPresentationViewSource View;
	View.ViewLocation = FVector(-20000.0, 0.0, 0.0);
	View.SubjectLocation = View.ViewLocation;
	View.Forward = FVector::ForwardVector;
	View.Right = FVector::RightVector;
	View.Up = FVector::UpVector;
	View.Revision = 1;
	const FPresentationSourceHandle PresentationSource = Presentation->RegisterSource(View);
	const FBuildEntityHandle Door = Building->CreateEntity(
		*Definition,
		FTransform::Identity,
		EBuildSpatialMobility::Static);
	TestTrue(TEXT("创建远场 Door"), Door.IsSet() && PresentationSource.IsSet());
	TestEqual(TEXT("没有 Collision Source 时远场 Door 零 Chaos Body"),
		Building->GetActiveCollisionBodyCount(), 0);

	TestTrue(TEXT("远场 Door 接收开门请求"), Catalog->RequestDoorInteraction(Door));
	RunBuildingPostActorTick(*World);
	FBuildDoorStateFragment* State =
		Building->GetRegistry().FindMutableFragment<FBuildDoorStateFragment>(Door);
	if (State)
	{
		State->TransitionStartServerTimeSeconds = GetBuildingAuthorityTimeSeconds(*World) - 1.0;
	}
	RunBuildingPostActorTick(*World);
	State = Building->GetRegistry().FindMutableFragment<FBuildDoorStateFragment>(Door);
	TestTrue(TEXT("远场 Door 只在 ECS 中完成 Open 状态"),
		State && State->State == EBuildDoorState::Open);
	TestEqual(TEXT("远场开门全过程仍然零 Chaos Body"),
		Building->GetActiveCollisionBodyCount(), 0);

	const FBuildCollisionSourceHandle CollisionSource =
		Building->RegisterCollisionSource(
			MakeDoorCollisionSource(FVector::ZeroVector, 1));
	TestTrue(TEXT("玩家靠近已打开 Door 时注册局部 Source"),
		CollisionSource.IsSet() && Building->FlushCollisionChanges(1.0));
	TestEqual(TEXT("靠近后只创建 Door 当前四个碰撞 Part"),
		Building->GetActiveCollisionBodyCount(), 4);
	FBuildCollisionInstanceHandle LeafInstance;
	FTransform ProjectedLeafTransform;
	FTransform ExpectedLeafTransform;
	const FBuildPartTransformFragment* PartTransforms =
		Building->GetRegistry().FindFragment<FBuildPartTransformFragment>(Door);
	TestTrue(TEXT("靠近后门扇 Body 直接使用当前 Open 姿态"),
		PartTransforms
		&& Definition->TryCalculateCollisionPartWorldTransform(
			0,
			FTransform::Identity,
			PartTransforms->LocalTransforms,
			ExpectedLeafTransform)
		&& Building->TryGetPartCollisionInstance(Door, 0, LeafInstance)
		&& Building->GetCollisionHost()->TryGetInstanceTransform(
			LeafInstance,
			ProjectedLeafTransform)
		&& ProjectedLeafTransform.Equals(ExpectedLeafTransform));

	Building->UnregisterCollisionSource(CollisionSource);
	Presentation->UnregisterSource(PresentationSource);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
		return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSettlementDoorPersistentPresentationStateTest,
	"ElementSandbox.BuildingCatalog.Door.SettlementPersistentPresentationState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementDoorPersistentPresentationStateTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::BuildingCatalog::Door::Tests;
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
			TEXT("SettlementDoorPersistent"),
		nullptr,
		true);
	if (!World)
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* Building =
		World->GetSubsystem<UBuildingWorldSubsystem>();
	UBuildingCatalogWorldSubsystem* Catalog =
		World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	UDoorBuildingDefinition* Definition = Building
		? Cast<UDoorBuildingDefinition>(
			Building->FindDefinition(TEXT("Settlement.Door")))
		: nullptr;
		TestNotNull(TEXT("复用 Catalog 注册的聚落伴生门 Definition"), Definition);
		const FBuildEntityHandle Door = Building && Definition
			? Building->CreateEntity(*Definition, FTransform::Identity)
			: FBuildEntityHandle();
		TestTrue(TEXT("聚落伴生门作为普通持久化 Building 创建"), Door.IsSet());
		if (Building && Door.IsSet())
		{
			TestTrue(TEXT("伴生门接受同一权威互动命令"),
				Catalog && Catalog->RequestDoorInteraction(Door));
			RunBuildingPostActorTick(*World);
		FBox ClosedBounds(ForceInit);
		TestTrue(TEXT("读取聚落伴生门保守 Bounds"),
			Building->GetSpatialIndex().TryGetBounds(Door, ClosedBounds));
		FBuildPartTransformFragment* PartTransforms =
			Building->GetRegistry().FindMutableFragment<FBuildPartTransformFragment>(Door);
		if (PartTransforms)
		{
			const int32 MovingPartIds[] = {0, 4, 5, 6};
			const FTransform Motion = FBuildDoorProcessor::CalculateDoorMotion(0.5f);
			for (const int32 PartId : MovingPartIds)
			{
				PartTransforms->LocalTransforms[PartId] =
					Definition->MeshParts[PartId].LocalTransform * Motion;
			}
				TestTrue(TEXT("保守 Bounds 允许伴生门提交运动 Part"),
					Building->CommitPartTransformChange(Door, MovingPartIds));
			}
			TestTrue(TEXT("伴生门允许燃烧 Custom Data Dirty"),
				Building->CommitRenderCustomDataChange(Door));
			TestTrue(TEXT("伴生门允许动画期 Motion Pin"),
			Building->SetPresentationMotionActive(Door, true));
		FBox AnimatedBounds(ForceInit);
		TestTrue(TEXT("动画后仍可读取聚落伴生门 Bounds"),
			Building->GetSpatialIndex().TryGetBounds(Door, AnimatedBounds));
			TestTrue(TEXT("伴生门动画不改锚点静态空间 Bounds"),
				AnimatedBounds.Equals(ClosedBounds));
			TestTrue(TEXT("聚落伴生门与普通 Building 一样允许 GameplayDestroy"),
				Building->DestroyEntity(Door));
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FDoorProcessorDropsDestroyedEntityTest,
	"ElementSandbox.BuildingCatalog.Door.DropsDestroyedEntity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorProcessorDropsDestroyedEntityTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::BuildingCatalog::Door::Tests;
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("DoorProcessorDestroyed"),
		nullptr,
		true);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* Building = World->GetSubsystem<UBuildingWorldSubsystem>();
	UBuildingCatalogWorldSubsystem* Catalog =
		World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	UDoorBuildingDefinition* Definition = Building
		? Cast<UDoorBuildingDefinition>(Building->FindDefinition(TEXT("Door")))
		: nullptr;
		const FBuildEntityHandle OldDoor = Building->CreateEntity(
			*Definition,
			FTransform::Identity,
			EBuildSpatialMobility::Static);
			TestTrue(TEXT("销毁前请求已入队"), Catalog->RequestDoorInteraction(OldDoor));
			RunBuildingPostActorTick(*World);
			TestTrue(TEXT("销毁活跃队列中的 Door"), Building->DestroyEntity(OldDoor));
	const FBuildEntityHandle Replacement = Building->CreateEntity(
		*Definition,
		FTransform(FVector(500.0, 0.0, 0.0)),
		EBuildSpatialMobility::Static);
	TestEqual(TEXT("新 Door 复用旧 Slot"), Replacement.GetIndex(), OldDoor.GetIndex());
	TestNotEqual(TEXT("新 Door 使用新 Generation"),
		Replacement.GetGeneration(), OldDoor.GetGeneration());
	RunBuildingPostActorTick(*World);
	const FBuildDoorStateFragment* ReplacementState =
		Building->GetRegistry().FindFragment<FBuildDoorStateFragment>(Replacement);
	TestTrue(TEXT("旧工作项不会命中新 Generation"),
		ReplacementState && ReplacementState->State == EBuildDoorState::Closed);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorProcessorStablePopulationRemainsIdleTest,
	"ElementSandbox.BuildingCatalog.Door.StablePopulationRemainsIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorProcessorStablePopulationRemainsIdleTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::BuildingCatalog::Door::Tests;
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("DoorProcessorStablePopulation"),
		nullptr,
		true);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* Building =
		World->GetSubsystem<UBuildingWorldSubsystem>();
	UBuildingCatalogWorldSubsystem* Catalog =
		World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	if (!Building || !Catalog)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	FBuildDoorStateFragment StableDoorState;
	for (int32 DoorIndex = 0; DoorIndex < 10000; ++DoorIndex)
	{
		const FBuildEntityHandle Entity = Building->GetRegistry().CreateEntity();
		TestTrue(TEXT("为静止门规模测试添加 DoorState"),
			Building->GetRegistry().AddFragment(Entity, StableDoorState));
	}

	FBuildProcessorStats Stats;
	TestTrue(TEXT("读取 Authority Door Processor 统计"),
		Catalog->TryGetDoorProcessorStats(Stats));
	TestEqual(TEXT("一万稳定 DoorState 不会主动执行 Processor"),
		Stats.ExecutionCount, static_cast<uint64>(0));
	RunBuildingPostActorTick(*World);
	TestTrue(TEXT("空闲帧后仍可读取 Door Processor 统计"),
		Catalog->TryGetDoorProcessorStats(Stats));
	TestEqual(TEXT("Scheduler 不扫描一万 DoorState"),
		Stats.ExecutionCount, static_cast<uint64>(0));

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorProcessorNotRegisteredOnPureClientTest,
	"ElementSandbox.BuildingCatalog.Door.NotRegisteredOnPureClient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorProcessorNotRegisteredOnPureClientTest::RunTest(
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
		TEXT("DoorProcessorPureClient"),
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
	World->SetPlayInEditorInitialNetMode(NM_Client);
	World->InitWorld(InitializationValues);
	World->UpdateWorldComponents(true, false);
	UBuildingCatalogWorldSubsystem* Catalog =
		World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	TestNotNull(TEXT("纯客户端仍创建 Catalog Subsystem"), Catalog);
	TestEqual(TEXT("测试 World 确认为纯 Client"), World->GetNetMode(), NM_Client);
	TestFalse(TEXT("纯客户端不注册 Authority Door Processor"),
		Catalog && Catalog->HasAuthorityDoorProcessor());
		TestFalse(TEXT("纯客户端不能向本地 ECS 冒充提交权威 Door 命令"),
			Catalog && Catalog->RequestDoorInteraction(FBuildEntityHandle()));

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif

#endif
