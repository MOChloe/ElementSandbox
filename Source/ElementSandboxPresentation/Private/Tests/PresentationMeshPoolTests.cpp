#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MeshPoolRenderHost.h"
#include "PresentationSettings.h"
#include "PresentationWorldSubsystem.h"
#include "Rendering/MeshPoolHierarchicalInstancedStaticMeshComponent.h"

namespace ElementSandbox::Presentation::Tests
{
struct FPresentationTestWorld final
{
	FPresentationTestWorld()
	{
		// Game 世界会自动装入 Building 等领域 Subsystem，它们拥有自己的 Projector，
		// 会把领域续帧请求混进 Presentation 单元测试。GamePreview 只保留本模块支持的
		// 客户端表现上下文，让 Dirty/Tick 断言只观察测试注册的 Source 与 Projector。
		World = UWorld::CreateWorld(EWorldType::GamePreview, false, NAME_None, nullptr, true);
		check(World);
		GEngine->CreateNewWorldContext(EWorldType::GamePreview).SetCurrentWorld(World);
		Subsystem = World->GetSubsystem<UPresentationWorldSubsystem>();
		if (Subsystem)
		{
			Layer = Subsystem->RegisterMeshLayer(TEXT("MeshPoolTest"));
		}
	}

	~FPresentationTestWorld()
	{
		if (Subsystem && Layer.IsSet())
		{
			Subsystem->UnregisterMeshLayer(Layer);
		}
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	}

	FMeshPoolClusterKey MakeCluster(UStaticMesh& Mesh, const FIntVector Cell = FIntVector::ZeroValue,
									const EMeshPoolBackend Backend = EMeshPoolBackend::HierarchicalStatic,
									const int32 CustomDataCount = 0) const
	{
		FMeshPoolClusterKey Key;
		Key.Layer = Layer;
		Key.Cell = Cell;
		Key.Mesh = &Mesh;
		Key.Backend = Backend;
		Key.CustomDataFloatCount = CustomDataCount;
		return Key;
	}

	UWorld* World = nullptr;
	UPresentationWorldSubsystem* Subsystem = nullptr;
	FMeshPoolLayerHandle Layer;
};
} // namespace ElementSandbox::Presentation::Tests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPoolPendingCommandCoalescingTest,
								 "ElementSandbox.Presentation.MeshPool.PendingCommandsCoalesce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolPendingCommandCoalescingTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}
	const FMeshPoolClusterKey Cluster =
		Harness.MakeCluster(*Mesh, FIntVector::ZeroValue, EMeshPoolBackend::HierarchicalStatic, 1);
	const float InitialCustomData[] = {1.0f};
	const FMeshPoolInstanceHandle Instance =
		Harness.Subsystem->QueueAdd(Cluster, FTransform::Identity, InitialCustomData);
	TestTrue(TEXT("Pending Add 立即返回稳定句柄"), Instance.IsSet());
	TestEqual(TEXT("一个 Pending Slot 只计数一次"), Harness.Subsystem->GetMeshPoolStats().PendingInstanceCount, 1);
	TestTrue(TEXT("Pending 阶段可以覆盖 Transform"),
			 Harness.Subsystem->QueueUpdate(Instance, FTransform(FVector(100.0, 0.0, 0.0)),
											MakeArrayView(InitialCustomData)));
	const float FinalCustomData[] = {7.0f};
	TestTrue(
		TEXT("同一窗口多次 Update 只保留最终状态"),
		Harness.Subsystem->QueueUpdate(Instance, FTransform(FVector(200.0, 0.0, 0.0)), MakeArrayView(FinalCustomData)));
	FTransform Desired;
	TestTrue(TEXT("读取到最后一次 Update"), Harness.Subsystem->TryGetInstanceTransform(Instance, Desired) &&
												Desired.GetLocation().Equals(FVector(200.0, 0.0, 0.0)));
	TestTrue(TEXT("Add 后 Remove 在提交前抵消"), Harness.Subsystem->QueueRemove(Instance));
	TestFalse(TEXT("抵消后句柄失效"), Harness.Subsystem->IsValidInstance(Instance));
	TestEqual(TEXT("抵消后 Pending 计数立即归零"), Harness.Subsystem->GetMeshPoolStats().PendingInstanceCount, 0);
	TestTrue(TEXT("空提交成功"), Harness.Subsystem->FlushNow());
	const FMeshPoolStats Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("未创建物理实例"), Stats.ResidentInstanceCount, 0);
	TestEqual(TEXT("未留下空 Cluster"), Stats.ClusterCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPoolDirtyQueueAndSlotReuseTest,
								 "ElementSandbox.Presentation.MeshPool.DirtyQueueAndSlotReuse",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolDirtyQueueAndSlotReuseTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}
	const FMeshPoolClusterKey Cluster = Harness.MakeCluster(*Mesh);
	const FMeshPoolInstanceHandle Cancelled = Harness.Subsystem->QueueAdd(Cluster, FTransform::Identity);
	TestTrue(TEXT("待抵消句柄有效"), Cancelled.IsSet());
	TestTrue(TEXT("提交前抵消"), Harness.Subsystem->QueueRemove(Cancelled));
	const FMeshPoolInstanceHandle Reused = Harness.Subsystem->QueueAdd(Cluster, FTransform(FVector(100.0, 0.0, 0.0)));
	TestTrue(TEXT("释放 Slot 在同一窗口内复用但 Generation 前进"),
			 Reused.IsSet() && Reused.GetIndex() == Cancelled.GetIndex() &&
				 Reused.GetGeneration() != Cancelled.GetGeneration());
	TestTrue(TEXT("复用 Slot 提交成功"), Harness.Subsystem->FlushNow());
	FMeshPoolStats Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("复用没有产生重复物理提交"), Stats.ResidentInstanceCount, 1);
	TestEqual(TEXT("复用的队列项只访问一次"), Stats.LastVisitedDirtySlotCount, 1);

	TArray<FMeshPoolInstanceHandle> Instances;
	Instances.Reserve(4096);
	for (int32 Index = 0; Index < 4096; ++Index)
	{
		Instances.Add(Harness.Subsystem->QueueAdd(Cluster, FTransform(FVector(Index * 110.0 + 1000.0, 0.0, 0.0))));
	}
	TestTrue(TEXT("大量 Slot 一次提交"), Harness.Subsystem->FlushNow());
	const uint64 VisitsBeforeSingleUpdate = Harness.Subsystem->GetMeshPoolStats().TotalVisitedDirtySlotCount;
	TestTrue(TEXT("只修改一个已驻留实例"),
			 Harness.Subsystem->QueueUpdate(Instances[2048], FTransform(FVector(5000.0, 200.0, 0.0))));
	TestTrue(TEXT("单实例差量提交"), Harness.Subsystem->FlushNow());
	Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("大量已驻留 Slot 中只访问一个 Dirty Slot"),
			  Stats.TotalVisitedDirtySlotCount - VisitsBeforeSingleUpdate, static_cast<uint64>(1));
	const uint64 VisitsBeforeEmptyFlush = Stats.TotalVisitedDirtySlotCount;
	const uint64 EmptyFlushesBefore = Stats.EmptyFlushCount;
	TestTrue(TEXT("无命令 Flush 成功"), Harness.Subsystem->FlushNow());
	Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("空周期访问 Slot 数为零"), Stats.LastVisitedDirtySlotCount, 0);
	TestEqual(TEXT("空周期不增加累计 Slot 访问"), Stats.TotalVisitedDirtySlotCount, VisitsBeforeEmptyFlush);
	TestEqual(TEXT("空 Flush 计数递增"), Stats.EmptyFlushCount, EmptyFlushesBefore + 1);

	const FMeshPoolClusterKey HotCluster =
		Harness.MakeCluster(*Mesh, FIntVector(10, 0, 0), EMeshPoolBackend::ImmediateMovable);
	const FMeshPoolInstanceHandle HotInstance =
		Harness.Subsystem->QueueAdd(HotCluster, FTransform(FVector(10000.0, 0.0, 0.0)));
	TestTrue(TEXT("Hot Add 在 Queue 内即时提交"), HotInstance.IsSet());
	const uint64 VisitsBeforeHotUpdate = Harness.Subsystem->GetMeshPoolStats().TotalVisitedDirtySlotCount;
	TestTrue(TEXT("Hot Transform 在 Queue 内即时提交"),
			 Harness.Subsystem->QueueUpdate(HotInstance, FTransform(FVector(10100.0, 0.0, 0.0))));
	Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("Hot 即时 Flush 只消费对应 Dirty Slot"), Stats.TotalVisitedDirtySlotCount - VisitsBeforeHotUpdate,
			  static_cast<uint64>(1));
	const uint64 VisitsAfterHotUpdate = Stats.TotalVisitedDirtySlotCount;
	TestTrue(TEXT("Hot 后续全局 Flush 成功"), Harness.Subsystem->FlushNow());
	Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("Hot 已消费标记不会被全局 Flush 重复处理"), Stats.TotalVisitedDirtySlotCount, VisitsAfterHotUpdate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshPoolClusterBatchSchedulingTest,
	"ElementSandbox.Presentation.MeshPool.InterleavedClustersUseWholeBatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolClusterBatchSchedulingTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	UPresentationSettings* Settings = GetMutableDefault<UPresentationSettings>();
	const int32 PreviousBatchSize = Settings->MaximumNativeInstanceBatchSize;
	const double PreviousBudget = Settings->InstanceApplyTargetMilliseconds;
	ON_SCOPE_EXIT
	{
		Settings->MaximumNativeInstanceBatchSize = PreviousBatchSize;
		Settings->InstanceApplyTargetMilliseconds = PreviousBudget;
	};
	Settings->MaximumNativeInstanceBatchSize = 16;
	Settings->InstanceApplyTargetMilliseconds = 0.0;
	TArray<FMeshPoolInstanceHandle> Instances;
	for (int32 Round = 0; Round < 32; ++Round)
	{
		for (int32 ClusterIndex = 0; ClusterIndex < 6; ++ClusterIndex)
		{
			const FMeshPoolClusterKey Cluster = Harness.MakeCluster(*Mesh, FIntVector(ClusterIndex, 0, 0));
			Instances.Add(Harness.Subsystem->QueueAdd(Cluster,
				FTransform(FVector(Round * 100.0, ClusterIndex * 1000.0, 0.0))));
		}
	}
	Harness.Subsystem->Tick(0.0f);
	FMeshPoolStats Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("交错输入仍组成同一 Cluster 的完整原生批次"), Stats.LastFlushInstanceCount, 16);
	TestEqual(TEXT("零时间预算确定性地只推进一批"), Stats.LastFlushBatchCount, 1);
	TestEqual(TEXT("一个批次仅触及所选 Cluster"), Stats.LastFlushClusterCount, 1);
	TestTrue(TEXT("同 Cluster 的后续实例没有被其他 Cluster 隔断"),
		Harness.Subsystem->IsInstancePhysicallyResident(Instances[15 * 6]));
	Harness.Subsystem->Tick(0.0f);
	TestTrue(TEXT("下一批轮转到另一个 Cluster"),
		Harness.Subsystem->IsInstancePhysicallyResident(Instances[1]));
	TestFalse(TEXT("大 Cluster 没有独占后续所有批次"),
		Harness.Subsystem->IsInstancePhysicallyResident(Instances[16 * 6]));
	TestTrue(TEXT("尾部交互实例可以提升优先级"), Harness.Subsystem->PrioritizePendingInstance(Instances.Last()));
	Harness.Subsystem->Tick(0.0f);
	TestTrue(TEXT("Urgent 优先于普通 Cluster 装填"), Harness.Subsystem->IsInstancePhysicallyResident(Instances.Last()));
	Settings->InstanceApplyTargetMilliseconds = 1000.0;
	Harness.Subsystem->Tick(0.0f);
	Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("充裕预算下普通 Tick 排空当前待办"), Stats.PendingInstanceCount, 0);
	TestTrue(TEXT("单次调用上限不再成为每帧吞吐上限"), Stats.LastFlushInstanceCount > 16 && Stats.LastFlushBatchCount > 1);
	TestEqual(TEXT("普通 Tick 可以处理超过四个已存在或新建 Cluster"), Stats.LastFlushClusterCount, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPoolPendingClusterRegroupingTest,
	"ElementSandbox.Presentation.MeshPool.PendingMigrationAndCancellationRegroup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolPendingClusterRegroupingTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh) return false;
	const FMeshPoolClusterKey A = Harness.MakeCluster(*Mesh, FIntVector(0, 0, 0));
	const FMeshPoolClusterKey B = Harness.MakeCluster(*Mesh, FIntVector(1, 0, 0));
	const FMeshPoolClusterKey C = Harness.MakeCluster(*Mesh, FIntVector(2, 0, 0));
	const FMeshPoolInstanceHandle Moving = Harness.Subsystem->QueueAdd(A, FTransform::Identity);
	Harness.Subsystem->QueueAdd(B, FTransform::Identity);
	const FMeshPoolInstanceHandle Cancelled = Harness.Subsystem->QueueAdd(A, FTransform::Identity);
	TestTrue(TEXT("未提交实例改换目标 Cluster"), Harness.Subsystem->QueueMigrate(
		Moving, B, FTransform(FVector(300.0, 0.0, 0.0))));
	TestTrue(TEXT("取消实例先提升为 Urgent"), Harness.Subsystem->PrioritizePendingInstance(Cancelled));
	TestTrue(TEXT("取消时立即摘除两个队列的关联"), Harness.Subsystem->QueueRemove(Cancelled));
	const FMeshPoolInstanceHandle Reused = Harness.Subsystem->QueueAdd(C, FTransform::Identity);
	TestTrue(TEXT("复用同一 Slot 但不继承旧 Cluster/优先级"), Reused.GetIndex() == Cancelled.GetIndex()
		&& Reused.GetGeneration() != Cancelled.GetGeneration());
	TestTrue(TEXT("按最终目标提交"), Harness.Subsystem->FlushNow());
	TestNull(TEXT("原目标没有留下物理实例或组件"), Harness.Subsystem->GetClusterComponent(A));
	UInstancedStaticMeshComponent* TargetB = Harness.Subsystem->GetClusterComponent(B);
	UInstancedStaticMeshComponent* TargetC = Harness.Subsystem->GetClusterComponent(C);
	if (!TestNotNull(TEXT("迁移目标存在"), TargetB) || !TestNotNull(TEXT("复用后的目标存在"), TargetC)) return false;
	TestEqual(TEXT("迁移目标收到两条实例"), TargetB->GetInstanceCount(), 2);
	TestEqual(TEXT("复用实例只提交一次"), TargetC->GetInstanceCount(), 1);
	TestEqual(TEXT("只访问仍有效的三个待办"), Harness.Subsystem->GetMeshPoolStats().LastVisitedDirtySlotCount, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPoolBulkHISMBuildTest,
								 "ElementSandbox.Presentation.MeshPool.OneTreeBuildPerClusterPerFlush",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolBulkHISMBuildTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}
	const FMeshPoolClusterKey Cluster =
		Harness.MakeCluster(*Mesh, FIntVector::ZeroValue, EMeshPoolBackend::HierarchicalStatic, 3);
	const uint64 BuildsBefore = Harness.Subsystem->GetMeshPoolStats().HierarchicalTreeBuildRequests;
	for (int32 Index = 0; Index < 512; ++Index)
	{
		const FVector Location(Index * 110.0, 0.0, 0.0);
		const float CustomData[] = {static_cast<float>(Index), static_cast<float>(Index) + 0.25f,
									static_cast<float>(Index) + 0.5f};
		TestTrue(TEXT("QueueAdd"),
				 Harness.Subsystem->QueueAdd(Cluster, FTransform(Location), MakeArrayView(CustomData)).IsSet());
	}
	TestEqual(TEXT("提交前全部保存在 Pending"), Harness.Subsystem->GetMeshPoolStats().PendingInstanceCount, 512);
	TestTrue(TEXT("一次性提交整个 Pending 窗口"), Harness.Subsystem->FlushNow());
	const FMeshPoolStats Stats = Harness.Subsystem->GetMeshPoolStats();
	TestEqual(TEXT("全部实例一次驻留"), Stats.ResidentInstanceCount, 512);
	TestEqual(TEXT("同 Key 只创建一个 Cluster"), Stats.ClusterCount, 1);
	const UInstancedStaticMeshComponent* Component = Harness.Subsystem->GetClusterComponent(Cluster);
	TestNotNull(TEXT("批量提交创建 HISM Component"), Component);
	if (Component)
	{
		TestEqual(TEXT("建树前已写完整连续 Custom Data"), Component->PerInstanceSMCustomData.Num(), 512 * 3);
		TestEqual(TEXT("首个 Custom Data 正确"), Component->PerInstanceSMCustomData[0], 0.0f);
		TestEqual(TEXT("末个 Custom Data 正确"), Component->PerInstanceSMCustomData.Last(), 511.5f);
	}
	TestEqual(TEXT("同 Cluster 一次 Flush 只请求一次树构建"), Stats.HierarchicalTreeBuildRequests - BuildsBefore,
			  static_cast<uint64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshPoolFirstNormalFlushPublishesCompleteBaselineTest,
	"ElementSandbox.Presentation.MeshPool.FirstNormalFlushPublishesCompleteBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolFirstNormalFlushPublishesCompleteBaselineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	const FMeshPoolClusterKey Cluster = Harness.MakeCluster(
		*Mesh, FIntVector::ZeroValue, EMeshPoolBackend::HierarchicalStatic, 1);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const float CustomData[] = {static_cast<float>(Index)};
		TestTrue(TEXT("首批 QueueAdd"), Harness.Subsystem->QueueAdd(
			Cluster, FTransform(FVector(Index * 100.0, 0.0, 0.0)), CustomData).IsSet());
	}
	TestTrue(TEXT("普通 Tick 前 MeshPool 被 Pending 实例唤醒"), Harness.Subsystem->IsTickable());
	Harness.Subsystem->Tick(0.0f);
	TestEqual(TEXT("普通 Tick 提交首批全部逻辑实例"),
		Harness.Subsystem->GetMeshPoolStats().ResidentInstanceCount, 4);

	const UHierarchicalInstancedStaticMeshComponent* Component =
		Cast<UHierarchicalInstancedStaticMeshComponent>(
			Harness.Subsystem->GetClusterComponent(Cluster));
	if (!TestNotNull(TEXT("普通 Tick 创建 HISM"), Component))
	{
		return false;
	}
	TestEqual(TEXT("首批 HISM CPU 实例完整"), Component->GetInstanceCount(), 4);
	TestEqual(TEXT("首批 HISM 渲染实例完整"), Component->GetNumRenderInstances(), 4);
	TestTrue(TEXT("首批 HISM Cluster Tree 已发布"), Component->IsTreeFullyBuilt());

	const float LiveCustomData[] = {4.0f};
	TestTrue(TEXT("小 Cluster 实时追加实例"), Harness.Subsystem->QueueAdd(
		Cluster, FTransform(FVector(400.0, 0.0, 0.0)), LiveCustomData).IsSet());
	Harness.Subsystem->Tick(0.0f);
	TestEqual(TEXT("实时追加进入 HISM CPU 数据"), Component->GetInstanceCount(), 5);
	TestEqual(TEXT("小 Cluster 实时追加同帧发布渲染实例"), Component->GetNumRenderInstances(), 5);
	TestTrue(TEXT("小 Cluster 实时追加同帧更新 Cluster Tree"), Component->IsTreeFullyBuilt());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshPoolDeferredHISMAddPublishesDrawStateTest,
	"ElementSandbox.Presentation.MeshPool.DeferredHISMAddPublishesDrawState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FMeshPoolDeferredHISMAddPublishesDrawStateTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	UPresentationSettings* Settings = GetMutableDefault<UPresentationSettings>();
	const int32 PreviousSynchronousLimit = Settings->HISMSynchronousBuildMaxInstances;
	const float PreviousQuietSeconds = Settings->HISMTreeBuildQuietSeconds;
	const float PreviousMaxDeferralSeconds = Settings->HISMTreeBuildMaxDeferralSeconds;
	Settings->HISMSynchronousBuildMaxInstances = 64;
	Settings->HISMTreeBuildQuietSeconds = 60.0f;
	Settings->HISMTreeBuildMaxDeferralSeconds = 60.0f;
	ON_SCOPE_EXIT
	{
		Settings->HISMSynchronousBuildMaxInstances = PreviousSynchronousLimit;
		Settings->HISMTreeBuildQuietSeconds = PreviousQuietSeconds;
		Settings->HISMTreeBuildMaxDeferralSeconds = PreviousMaxDeferralSeconds;
	};

	// 玩家建造件与种子构件共享 Cluster。先建立超过同步阈值的旧树，再跨帧追加，
	// 避免小 Cluster 的同步建树把缺失的 SceneProxy 发布掩盖掉。
	constexpr int32 BaselineInstanceCount = 80;
	const FMeshPoolClusterKey Cluster = Harness.MakeCluster(
		*Mesh, FIntVector::ZeroValue, EMeshPoolBackend::HierarchicalStatic, 1);
	for (int32 Index = 0; Index < BaselineInstanceCount; ++Index)
	{
		TestTrue(TEXT("建立已有大 Cluster"), Harness.Subsystem->QueueAdd(
			Cluster, FTransform(FVector(Index * 100.0, 0.0, 0.0))).IsSet());
	}
	if (!TestTrue(TEXT("发布已有 Cluster Tree"), Harness.Subsystem->FlushNow()))
	{
		return false;
	}
	UMeshPoolHierarchicalInstancedStaticMeshComponent* Component =
		Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(
			Harness.Subsystem->GetClusterComponent(Cluster));
	if (!TestNotNull(TEXT("已有 Cluster 使用受管 HISM"), Component))
	{
		return false;
	}
	TestTrue(TEXT("旧树已构建"), Component->IsTreeFullyBuilt());
	TestEqual(TEXT("旧树覆盖全部基线实例"), Component->GetNumRenderInstances(), BaselineInstanceCount);
	TestTrue(TEXT("回归场景具有实际 Render State"), Component->IsRenderStateCreated());
	Harness.World->SendAllEndOfFrameUpdates();
	TestFalse(TEXT("基线 Render State Dirty 已被帧尾消费"), Component->IsRenderStateDirty());
	const uint64 BaselineBuildCount = Component->GetTreeBuildRequestCount();

	for (int32 AdditionIndex = 0; AdditionIndex < 2; ++AdditionIndex)
	{
		const FVector Location(10000.0 + AdditionIndex * 100.0, 0.0, 0.0);
		const FMeshPoolInstanceHandle Added = Harness.Subsystem->QueueAdd(Cluster, FTransform(Location));
		TestTrue(TEXT("实时追加取得稳定实例身份"), Added.IsSet());
		Harness.Subsystem->Tick(0.0f);
		TestTrue(TEXT("普通 Tick 已将新实例提交到 HISM"),
			Harness.Subsystem->IsInstancePhysicallyResident(Added));
		TestEqual(TEXT("CPU 实例包含追加项"), Component->GetInstanceCount(),
			BaselineInstanceCount + AdditionIndex + 1);
		TestEqual(TEXT("大 Cluster 保持延迟建树预算"), Component->GetTreeBuildRequestCount(), BaselineBuildCount);
		TestFalse(TEXT("新实例暂时处于旧树以外"), Component->IsTreeFullyBuilt());
		TestTrue(TEXT("追加实例必须刷新 SceneProxy 的未建树绘制范围"), Component->IsRenderStateDirty());
		Harness.World->SendAllEndOfFrameUpdates();
		TestFalse(TEXT("每次追加的 Render State 更新在帧尾消费"), Component->IsRenderStateDirty());
		const float BurnAmount[] = {0.5f};
		TestTrue(TEXT("追加后的材质数据仍可增量更新"), Harness.Subsystem->QueueCustomData(Added, BurnAmount));
		Harness.Subsystem->Tick(0.0f);
		TestFalse(TEXT("仅材质数据变化不重建 Render State"), Component->IsRenderStateDirty());
		Harness.World->SendAllEndOfFrameUpdates();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPoolHISMQuietPeriodTest,
									 "ElementSandbox.Presentation.MeshPool.HISMTreeQuietPeriod",
									 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolHISMQuietPeriodTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.World || !Mesh)
	{
		return false;
	}
	const auto MakeComponent = [&]()
	{
		AMeshPoolRenderHost* Owner = Harness.World->SpawnActor<AMeshPoolRenderHost>();
		UMeshPoolHierarchicalInstancedStaticMeshComponent* Component =
			Owner ? NewObject<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Owner) : nullptr;
		if (Component)
		{
			Component->SetupAttachment(Owner->GetRootComponent());
			Component->SetStaticMesh(Mesh);
			Component->SetNumCustomDataFloats(2);
			Owner->AddInstanceComponent(Component);
			Component->RegisterComponent();
			// 清除 SetStaticMesh/Register 产生的生命周期建树请求，从可注入时钟零点开始。
			Component->BeginBulkEdit();
			Component->CancelBulkEdit();
		}
		return Component;
	};

	UMeshPoolHierarchicalInstancedStaticMeshComponent* QuietComponent = MakeComponent();
	if (!TestNotNull(TEXT("创建静默期 HISM"), QuietComponent))
	{
		return false;
	}
	QuietComponent->BeginBulkEdit();
	QuietComponent->AddInstance(FTransform::Identity, false);
	QuietComponent->EndBulkEdit(0.0, true);
	TestFalse(TEXT("0ms 不建树"), QuietComponent->TryStartDeferredTreeBuild(0.0, 0.25, 1.0, false));
	TestFalse(TEXT("125ms 不建树"), QuietComponent->TryStartDeferredTreeBuild(0.125, 0.25, 1.0, false));
	TestTrue(TEXT("最后编辑后 250ms 启动一次最新树"),
			 QuietComponent->TryStartDeferredTreeBuild(0.25, 0.25, 1.0, false));
	TestEqual(TEXT("静默窗口只启动一次真实树"), QuietComponent->GetTreeBuildRequestCount(), static_cast<uint64>(1));
	TestFalse(TEXT("未 BeginPlay 的通用 HISM 首轮树按 UE 规则同步建立"), QuietComponent->IsAsyncBuilding());
	QuietComponent->BeginBulkEdit();
	QuietComponent->AddInstance(FTransform(FVector(100.0, 0.0, 0.0)), false);
	QuietComponent->EndBulkEdit(0.30, true);
	TestTrue(TEXT("通用 HISM 已有基线树后启动异步重建"),
		QuietComponent->TryStartDeferredTreeBuild(0.55, 0.25, 1.0, false));
	TestTrue(TEXT("通用 HISM 第二轮 BuildTree 正在后台执行"), QuietComponent->IsAsyncBuilding());
	QuietComponent->BeginBulkEdit();
	QuietComponent->AddInstance(FTransform(FVector(200.0, 0.0, 0.0)), false);
	QuietComponent->EndBulkEdit(0.56, true);
	TestTrue(TEXT("异步建树期间的实例编辑先留下合并请求"), QuietComponent->HasDeferredTreeBuild());
	const float UpdatedCustomData[] = {0.25f, 0.75f, 0.5f, 1.0f};
	TestTrue(TEXT("通用 HISM 异步建树期间接受 Custom Data 更新"),
		QuietComponent->SetMeshPoolCustomDataRange(0, 1, MakeArrayView(UpdatedCustomData)));
	const double QuietBuildDeadline = FPlatformTime::Seconds() + 5.0;
	while ((QuietComponent->IsAsyncBuilding() || QuietComponent->GetTreeBuildRequestCount() < 3)
		&& FPlatformTime::Seconds() < QuietBuildDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	TestFalse(TEXT("通用 HISM 并发编辑后的内部重试完成"), QuietComponent->IsAsyncBuilding());
	TestTrue(TEXT("通用 HISM 重试后 Cluster Tree 为最新版本"), QuietComponent->IsTreeFullyBuilt());
	TestEqual(TEXT("通用 HISM 的 UE 内部重试未被静默窗口吞掉"),
		QuietComponent->GetTreeBuildRequestCount(), static_cast<uint64>(3));
	TestEqual(TEXT("内部重试单独计数"), QuietComponent->GetTreeBuildRetryCount(), static_cast<uint64>(1));
	TestFalse(TEXT("树发布时立即清理已消费的 Pending 请求"), QuietComponent->HasDeferredTreeBuild());
	const double NextEditTime = FPlatformTime::Seconds();
	QuietComponent->BeginBulkEdit();
	QuietComponent->AddInstance(FTransform(FVector(300.0, 0.0, 0.0)), false);
	QuietComponent->EndBulkEdit(NextEditTime, true);
	// 故意不先调用 Host/TryStart：覆盖“构建已完成、轮询尚未发生”时旧权限仍然开放的问题。
	QuietComponent->BuildTreeIfOutdated(true, false);
	TestEqual(TEXT("新编辑不能继承上一轮内部重试权限"), QuietComponent->GetTreeBuildRequestCount(), static_cast<uint64>(3));
	TestFalse(TEXT("未调度的新编辑不启动异步任务"), QuietComponent->IsAsyncBuilding());
	TestFalse(TEXT("新编辑重新等待自身静默窗口"), QuietComponent->TryStartDeferredTreeBuild(
		NextEditTime + 0.125, 0.25, 1.0, false));
	TestTrue(TEXT("新静默窗口结束后只启动一次构建"), QuietComponent->TryStartDeferredTreeBuild(
		NextEditTime + 0.30, 0.25, 1.0, false));
	const double NextBuildDeadline = FPlatformTime::Seconds() + 5.0;
	while (QuietComponent->IsAsyncBuilding() && FPlatformTime::Seconds() < NextBuildDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	TestTrue(TEXT("新一轮树完整发布"), QuietComponent->IsTreeFullyBuilt());
	TestEqual(TEXT("无并发修改的新一轮没有重复建树"), QuietComponent->GetTreeBuildRequestCount(), static_cast<uint64>(4));
	TestFalse(TEXT("内部重试完成后不再启动一棵重复树"),
		QuietComponent->TryStartDeferredTreeBuild(2.0, 0.25, 1.0, false));
	TestFalse(TEXT("内部重试消费异步期间留下的合并请求"), QuietComponent->HasDeferredTreeBuild());

	UMeshPoolHierarchicalInstancedStaticMeshComponent* ContinuousComponent = MakeComponent();
	if (!TestNotNull(TEXT("创建持续编辑 HISM"), ContinuousComponent))
	{
		return false;
	}
	for (int32 Step = 0; Step < 8; ++Step)
	{
		const double EditTime = Step * 0.125;
		ContinuousComponent->BeginBulkEdit();
		ContinuousComponent->AddInstance(FTransform(FVector(Step * 100.0, 0.0, 0.0)), false);
		ContinuousComponent->EndBulkEdit(EditTime, true);
		TestFalse(TEXT("持续编辑未到 1s 不建树"),
				  ContinuousComponent->TryStartDeferredTreeBuild(EditTime, 0.25, 1.0, false));
	}
	ContinuousComponent->BeginBulkEdit();
	ContinuousComponent->AddInstance(FTransform(FVector(900.0, 0.0, 0.0)), false);
	ContinuousComponent->EndBulkEdit(1.0, true);
	TestTrue(TEXT("持续编辑累计 1s 后强制启动"), ContinuousComponent->TryStartDeferredTreeBuild(1.0, 0.25, 1.0, false));

	UMeshPoolHierarchicalInstancedStaticMeshComponent* ForcedComponent = MakeComponent();
	if (!TestNotNull(TEXT("创建强制模式 HISM"), ForcedComponent))
	{
		return false;
	}
	ForcedComponent->BeginBulkEdit();
	ForcedComponent->AddInstance(FTransform::Identity, false);
	ForcedComponent->EndBulkEdit(0.0, true);
	TestTrue(TEXT("FlushNow 对应的强制模式无需等待静默期"),
			 ForcedComponent->TryStartDeferredTreeBuild(0.0, 0.25, 1.0, true));
	const double RemainingBuildDeadline = FPlatformTime::Seconds() + 5.0;
	while ((ContinuousComponent->IsAsyncBuilding() || ForcedComponent->IsAsyncBuilding())
		&& FPlatformTime::Seconds() < RemainingBuildDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	TestFalse(TEXT("持续编辑 HISM 构建在测试销毁前完成"), ContinuousComponent->IsAsyncBuilding());
	TestFalse(TEXT("强制 HISM 构建在测试销毁前完成"), ForcedComponent->IsAsyncBuilding());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshPoolHISMGlobalBuildAndRetirementTest,
	"ElementSandbox.Presentation.MeshPool.HISMBuildsAreGlobalAndRetirementIsNonBlocking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolHISMGlobalBuildAndRetirementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}

	IConsoleVariable* BuildDelay = IConsoleManager::Get().FindConsoleVariable(
		TEXT("foliage.DebugBuildTreeAsyncDelayInSeconds"));
	const float PreviousBuildDelay = BuildDelay ? BuildDelay->GetFloat() : 0.0f;
	if (BuildDelay)
	{
		BuildDelay->Set(0.05f, ECVF_SetByCode);
	}

	const FMeshPoolClusterKey ClusterA = Harness.MakeCluster(*Mesh, FIntVector(20, 0, 0));
	const FMeshPoolClusterKey ClusterB = Harness.MakeCluster(*Mesh, FIntVector(21, 0, 0));
	TArray<FMeshPoolInstanceHandle> InstancesA;
	TArray<FMeshPoolInstanceHandle> InstancesB;
	constexpr int32 InitialInstanceCount = 80;
	for (int32 Index = 0; Index < InitialInstanceCount; ++Index)
	{
		InstancesA.Add(Harness.Subsystem->QueueAdd(
			ClusterA, FTransform(FVector(Index * 100.0, 0.0, 0.0))));
		InstancesB.Add(Harness.Subsystem->QueueAdd(
			ClusterB, FTransform(FVector(Index * 100.0, 1000.0, 0.0))));
	}
	const bool bInitialFlushSucceeded = Harness.Subsystem->FlushNow();
	TestTrue(TEXT("两个 HISM Cluster 的初始实例批量提交成功"), bInitialFlushSucceeded);
	AMeshPoolRenderHost* Host = Harness.Subsystem->GetRenderHost();
	if (!bInitialFlushSucceeded || !TestNotNull(TEXT("批量提交后创建 Render Host"), Host))
	{
		if (BuildDelay)
		{
			BuildDelay->Set(PreviousBuildDelay, ECVF_SetByCode);
		}
		return false;
	}
	const double BaselineDeadline = FPlatformTime::Seconds() + 5.0;
	while (Host->HasDeferredTreeBuilds() && FPlatformTime::Seconds() < BaselineDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		Host->ProcessDeferredTreeBuilds(FPlatformTime::Seconds(), 0.0, 0.0, true);
		FTSTicker::GetCoreTicker().Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	UMeshPoolHierarchicalInstancedStaticMeshComponent* ComponentA =
		Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Host->GetClusterComponent(ClusterA));
	UMeshPoolHierarchicalInstancedStaticMeshComponent* ComponentB =
		Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Host->GetClusterComponent(ClusterB));
	if (!TestNotNull(TEXT("Cluster A 使用受管 HISM"), ComponentA)
		|| !TestNotNull(TEXT("Cluster B 使用受管 HISM"), ComponentB))
	{
		if (BuildDelay) BuildDelay->Set(PreviousBuildDelay, ECVF_SetByCode);
		return false;
	}

	InstancesA.Add(Harness.Subsystem->QueueAdd(
		ClusterA, FTransform(FVector(InitialInstanceCount * 100.0, 0.0, 0.0))));
	InstancesB.Add(Harness.Subsystem->QueueAdd(
		ClusterB, FTransform(FVector(InitialInstanceCount * 100.0, 1000.0, 0.0))));
	TestTrue(TEXT("并发重建请求提交成功"), Harness.Subsystem->FlushNow());
	const int32 AsyncBuildCount = (ComponentA->IsAsyncBuilding() ? 1 : 0)
		+ (ComponentB->IsAsyncBuilding() ? 1 : 0);
	TestEqual(TEXT("同一 Render Host 最多只有一个后台 HISM Tree Build"), AsyncBuildCount, 1);

	UMeshPoolHierarchicalInstancedStaticMeshComponent* RetiringComponent =
		ComponentA->IsAsyncBuilding() ? ComponentA : ComponentB;
	const TArray<FMeshPoolInstanceHandle>& RetiringInstances =
		ComponentA->IsAsyncBuilding() ? InstancesA : InstancesB;
	for (const FMeshPoolInstanceHandle Instance : RetiringInstances)
	{
		TestTrue(TEXT("异步建树中的空 Cluster 实例可正常删除"), Harness.Subsystem->QueueRemove(Instance));
	}
	TestTrue(TEXT("空 Cluster 退休提交成功"), Harness.Subsystem->FlushNow());
	TestTrue(TEXT("后台任务完成前 Component 保持注册且不触发同步销毁"), RetiringComponent->IsRegistered());
	TestEqual(TEXT("退休中的物理 Component 不再计入逻辑 Cluster"),
		Harness.Subsystem->GetMeshPoolStats().ClusterCount, 1);

	const double RetirementDeadline = FPlatformTime::Seconds() + 5.0;
	while (RetiringComponent->IsRegistered() && FPlatformTime::Seconds() < RetirementDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		Host->ProcessDeferredTreeBuilds(FPlatformTime::Seconds(), 0.0, 0.0, true);
		FTSTicker::GetCoreTicker().Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	TestFalse(TEXT("后台树自然退出后空 Component 才被销毁"), RetiringComponent->IsRegistered());

	const double DrainDeadline = FPlatformTime::Seconds() + 5.0;
	while (Host->HasDeferredTreeBuilds() && FPlatformTime::Seconds() < DrainDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		Host->ProcessDeferredTreeBuilds(FPlatformTime::Seconds(), 0.0, 0.0, true);
		FTSTicker::GetCoreTicker().Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	if (BuildDelay)
	{
		BuildDelay->Set(PreviousBuildDelay, ECVF_SetByCode);
	}
	TestFalse(TEXT("测试结束前所有 HISM Tree Build 已排空"), Host->HasDeferredTreeBuilds());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPoolStableMigrationAndClusterCollectionTest,
								 "ElementSandbox.Presentation.MeshPool.StableMigrationAndEmptyClusterCollection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolStableMigrationAndClusterCollectionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}
	const FMeshPoolClusterKey Cold = Harness.MakeCluster(*Mesh, FIntVector(1, 0, 0));
	const FMeshPoolClusterKey Hot = Harness.MakeCluster(*Mesh, FIntVector(1, 0, 0), EMeshPoolBackend::ImmediateMovable);
	const FMeshPoolInstanceHandle Instance = Harness.Subsystem->QueueAdd(Cold, FTransform(FVector(1000.0, 0.0, 0.0)));
	TestTrue(TEXT("初始 HISM 提交"), Instance.IsSet() && Harness.Subsystem->FlushNow());
	TestTrue(TEXT("迁移到 Hot ISM 立即落地"),
			 Harness.Subsystem->QueueMigrate(Instance, Hot, FTransform(FVector(1010.0, 0.0, 0.0))));
	TestTrue(TEXT("迁移后原句柄仍有效"), Harness.Subsystem->IsValidInstance(Instance));
	TestEqual(TEXT("源 Cluster 为空后立即回收"), Harness.Subsystem->GetMeshPoolStats().ClusterCount, 1);
	TestTrue(TEXT("删除 Hot Instance"), Harness.Subsystem->QueueRemove(Instance));
	TestFalse(TEXT("删除后逻辑句柄失效"), Harness.Subsystem->IsValidInstance(Instance));
	TestEqual(TEXT("最后一个空 Cluster 被回收"), Harness.Subsystem->GetMeshPoolStats().ClusterCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPoolFailedLayerReprojectionTest,
								 "ElementSandbox.Presentation.MeshPool.FailedLayerRequestsFullReprojection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshPoolFailedLayerReprojectionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Harness.Subsystem || !Harness.Layer.IsSet() || !Mesh)
	{
		return false;
	}
	const FMeshPoolClusterKey Cluster = Harness.MakeCluster(*Mesh);
	const FMeshPoolInstanceHandle Instance = Harness.Subsystem->QueueAdd(
		Cluster, FTransform(FVector(100.0, 0.0, 0.0)));
	TestTrue(TEXT("建立待故障物理映射"), Instance.IsSet() && Harness.Subsystem->FlushNow());
	AMeshPoolRenderHost* Host = Harness.Subsystem->GetRenderHost();
	if (!TestNotNull(TEXT("存在 MeshPool Render Host"), Host))
	{
		return false;
	}
	Host->ClearLayer(Harness.Layer);
	TestTrue(TEXT("逻辑层仍可接收最终 Transform"), Harness.Subsystem->QueueUpdate(
		Instance, FTransform(FVector(200.0, 0.0, 0.0))));
	TestFalse(TEXT("物理映射丢失时 Flush 明确失败"), Harness.Subsystem->FlushNow());
	TestTrue(TEXT("失败不销毁逻辑驻留句柄"), Harness.Subsystem->IsValidInstance(Instance));
	TestEqual(TEXT("失败 Layer 的逻辑实例重新成为 Pending"),
		Harness.Subsystem->GetMeshPoolStats().PendingInstanceCount, 1);
	TestTrue(TEXT("失败 Layer 请求 Projector 完整重投影"),
		Harness.Subsystem->ConsumeLayerReprojectionRequest(Harness.Layer));
	TestTrue(TEXT("测试清理 Pending 句柄"), Harness.Subsystem->QueueRemove(Instance));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPresentationStableSourceHandleSnapshotTest,
	"ElementSandbox.Presentation.Source.StableHandleSurvivesSortAndGenerationReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPresentationStableSourceHandleSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Presentation::Tests;
	FPresentationTestWorld Harness;
	if (!Harness.Subsystem)
	{
		return false;
	}

	FPresentationViewSnapshot Captured;
	int32 ProjectionCount = 0;
	const FPresentationProjectorHandle Projector = Harness.Subsystem->RegisterProjector(
		TEXT("StableSourceHandleTest"),
		FPresentationProjectorDelegate::CreateLambda(
			[&Captured, &ProjectionCount](const FPresentationViewSnapshot& Snapshot)
			{
				Captured = Snapshot;
				++ProjectionCount;
			}));
	FPresentationViewSource A;
	A.ViewLocation = FVector(100.0, 0.0, 0.0);
	A.SubjectLocation = A.ViewLocation;
	A.Priority = 0;
	FPresentationViewSource B = A;
	B.ViewLocation = FVector(200.0, 0.0, 0.0);
	B.SubjectLocation = B.ViewLocation;
	B.Priority = 10;
	const FPresentationSourceHandle HandleA = Harness.Subsystem->RegisterSource(A);
	const FPresentationSourceHandle HandleB = Harness.Subsystem->RegisterSource(B);
	TestTrue(TEXT("注册两个稳定 Source"), Projector.IsSet() && HandleA.IsSet() && HandleB.IsSet());
	TestTrue(TEXT("固化优先级排序快照"), Harness.Subsystem->RunCycleNow());
	TestEqual(TEXT("高优先级 Source 排在前面"), Captured.Sources[0].ViewLocation, B.ViewLocation);
		TestTrue(TEXT("排序后 B 身份不串用"), Captured.Sources[0].SourceHandle == HandleB);
		TestTrue(TEXT("排序后 A 身份不串用"), Captured.Sources[1].SourceHandle == HandleA);
		TestFalse(TEXT("Dirty 投影排空后通用 Presentation 不 Tick"), Harness.Subsystem->IsTickable());
		const uint64 StableCycleCount = Harness.Subsystem->GetMeshPoolStats().ScheduledCycleCount;
		FPresentationViewSource DuplicateA = A;
		DuplicateA.Revision = 999;
		TestTrue(TEXT("相同观察值的重复 Revision 被接受为无操作"),
			Harness.Subsystem->UpdateSource(HandleA, DuplicateA));
		TestFalse(TEXT("相同观察值不重新唤醒 Presentation"), Harness.Subsystem->IsTickable());
		Harness.Subsystem->Tick(1.0f);
		TestEqual(TEXT("空闲期间不运行 Projector 周期"),
			Harness.Subsystem->GetMeshPoolStats().ScheduledCycleCount, StableCycleCount);
		TestTrue(TEXT("领域数据变化可在观察源静止时请求投影周期"),
			Harness.Subsystem->RequestProjectionCycle());
		TestTrue(TEXT("领域投影请求会唤醒 Presentation"), Harness.Subsystem->IsTickable());
		Harness.Subsystem->Tick(1.0f);
		TestEqual(TEXT("静止观察源下仍执行一次合并 Projector 周期"),
			Harness.Subsystem->GetMeshPoolStats().ScheduledCycleCount,
			StableCycleCount + 1);
		TestEqual(TEXT("显式领域 Dirty 只调用一次 Projector"), ProjectionCount, 2);

		A.Priority = 20;
		TestTrue(TEXT("更新 Source 时忽略调用方 Handle 并保留真实身份"), Harness.Subsystem->UpdateSource(HandleA, A));
		TestTrue(TEXT("真实观察变化只标记一次 Dirty 周期"), Harness.Subsystem->IsTickable());
	TestTrue(TEXT("再次固化重新排序"), Harness.Subsystem->RunCycleNow());
	TestEqual(TEXT("A 改优先级后排到前面"), Captured.Sources[0].ViewLocation, A.ViewLocation);
	TestTrue(TEXT("优先级变化不改变 A 句柄"), Captured.Sources[0].SourceHandle == HandleA);

	TestTrue(TEXT("注销 A"), Harness.Subsystem->UnregisterSource(HandleA));
	FPresentationViewSource C = A;
	C.ViewLocation = FVector(300.0, 0.0, 0.0);
	C.SubjectLocation = C.ViewLocation;
	const FPresentationSourceHandle HandleC = Harness.Subsystem->RegisterSource(C);
	TestTrue(TEXT("复用 Slot 时 Generation 前进"),
		HandleC.IsSet() && HandleC.GetIndex() == HandleA.GetIndex()
			&& HandleC.GetGeneration() != HandleA.GetGeneration() && HandleC != HandleA);
	TestFalse(TEXT("旧 Generation 不可再更新新 Source"), Harness.Subsystem->UpdateSource(HandleA, A));
	TestTrue(TEXT("新 Generation 正常进入快照"), Harness.Subsystem->RunCycleNow());
	const FPresentationViewSource* CapturedC = Captured.Sources.FindByPredicate(
		[&HandleC](const FPresentationViewSource& Source)
		{
			return Source.SourceHandle == HandleC;
		});
	TestNotNull(TEXT("快照携带复用后的新稳定句柄"), CapturedC);

	Harness.Subsystem->UnregisterSource(HandleB);
	Harness.Subsystem->UnregisterSource(HandleC);
	Harness.Subsystem->UnregisterProjector(Projector);
	return true;
}

#endif
