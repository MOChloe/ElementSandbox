#if WITH_DEV_AUTOMATION_TESTS

#include "ElementPresentationTypes.h"
#include "ElementPresentationWorldSubsystem.h"
#include "ElementVisualDefinition.h"
#include "PresentationWorldSubsystem.h"
#include "Visual/ElementVisualJournal.h"

#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Math/RotationMatrix.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

namespace ElementSandbox::ElementPresentation::Tests
{
static UWorld* CreateObservationWorld(const FName Name)
{
	UWorld::InitializationValues Values;
	Values.CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game, false, Name, nullptr, true, ERHIFeatureLevel::Num, &Values, true);
	if (!World)
	{
		return nullptr;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitWorld(Values);
	return World;
}

static void DestroyObservationWorld(UWorld* World)
{
	if (!World)
	{
		return;
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
}

static FElementPresentationConfig MakeTestConfig()
{
	FElementPresentationConfig Config;
	Config.ShardSize = 100.0;
	Config.CoverageRadius = 140.0;
	Config.HorizontalCoverageAngleDegrees = 180.0f;
	Config.VerticalCoverageAngleDegrees = 180.0f;
	Config.SubjectRecenterDistance = 20.0;
	Config.ViewRecenterDistance = 20.0;
	Config.FOVSafetyAngleDegrees = 10.0f;
	Config.MinimumRecenterAngleDegrees = 5.0f;
	Config.FieldOfViewChangeThresholdDegrees = 1.0f;
	Config.AspectRatioChangeThreshold = 0.01f;
	Config.ViewportDimensionChangeThreshold = 8;
	Config.GraceSeconds = 10.0;
	Config.MaxApplyCommandsPerTick = 64;
	Config.MaxApplyMilliseconds = 4.0;
	Config.InstancesPerPage = 8;
	Config.MaxSparePagesPerBackend = 2;
	Config.MaxCoverageShardsPerSource = 256;
	return Config;
}

static FPresentationViewSource MakeView(const FRotator Rotation = FRotator::ZeroRotator)
{
	FPresentationViewSource View;
	View.ViewLocation = FVector(50.0, 50.0, 50.0);
	View.SubjectLocation = View.ViewLocation;
	const FRotationMatrix Basis(Rotation);
	View.Forward = Basis.GetUnitAxis(EAxis::X);
	View.Right = Basis.GetUnitAxis(EAxis::Y);
	View.Up = Basis.GetUnitAxis(EAxis::Z);
	View.HorizontalFOVDegrees = 90.0f;
	View.AspectRatio = 16.0f / 9.0f;
	View.ViewportSize = FIntPoint(1920, 1080);
	View.Revision = 1;
	return View;
}

static FElementVisualDescriptor MakeVisual(const uint64 EntityId, const uint64 Revision = 1)
{
	FElementVisualDescriptor Descriptor;
	Descriptor.Key = FElementVisualKey::MakePersistent(
		FWorldEntityId(EntityId), TEXT("Test.ElementVisual"), 0);
	Descriptor.VisualDefinitionId = TEXT("Test.ElementVisual.Cube");
	Descriptor.Shard = FElementVisualShardKey{{1, 0, 0}};
	Descriptor.WorldTransform = FTransform(FVector(150.0, 50.0, 50.0));
	Descriptor.WorldBounds = FBox::BuildAABB(FVector(150.0, 50.0, 50.0), FVector(10.0));
	Descriptor.Color = FLinearColor::Red;
	Descriptor.Intensity = 1.0f;
	Descriptor.Revision = Revision;
	return Descriptor;
}

static bool RegisterTestDefinition(
	FAutomationTestBase& Test,
	UElementPresentationWorldSubsystem& Subsystem)
{
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Test.TestNotNull(TEXT("加载引擎 Cube 测试 Mesh"), Mesh))
	{
		return false;
	}
	FElementVisualDefinition Definition;
	Definition.DefinitionId = TEXT("Test.ElementVisual.Cube");
	Definition.StaticMesh = Mesh;
	Definition.Backend = EElementVisualInstanceBackend::Instanced;
	return Test.TestTrue(TEXT("注册测试 Visual Definition"), Subsystem.RegisterVisualDefinition(Definition));
}
} // namespace ElementSandbox::ElementPresentation::Tests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationObservationThresholdTest,
	"ElementSandbox.Element.Presentation.Observation.ThresholdAndJitter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationObservationThresholdTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	const FElementPresentationConfig Config = MakeTestConfig();
	TestTrue(TEXT("测试配置合法"), Config.IsValid());
	TestEqual(TEXT("180 覆盖、90 FOV、10 安全角得到 35 度重心阈值"),
		Config.CalculateHorizontalRecenterAngleDegrees(90.0f), 35.0f);
	TestTrue(TEXT("垂直 FOV 使用独立公式"),
		Config.CalculateVerticalRecenterAngleDegrees(50.0f) >
		Config.CalculateVerticalRecenterAngleDegrees(80.0f));

	UWorld* World = CreateObservationWorld(TEXT("ElementPresentationObservationThreshold"));
	if (!TestNotNull(TEXT("创建观察测试 World"), World))
	{
		return false;
	}
	UElementPresentationWorldSubsystem* ElementPresentation =
		World->GetSubsystem<UElementPresentationWorldSubsystem>();
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!TestNotNull(TEXT("ElementPresentation Subsystem 存在"), ElementPresentation)
		|| !TestNotNull(TEXT("公共 Presentation Source 存在"), Presentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	FPresentationViewSource View = MakeView();
	const FPresentationSourceHandle ViewHandle = Presentation->RegisterSource(View);
	TestTrue(TEXT("显式配置前也可缓存公共观察源"), ViewHandle.IsSet());
	TestEqual(TEXT("显式配置前不选择 Coverage"),
		ElementPresentation->GetStats().CoverageRecomputeCount, uint64(0));
	TestEqual(TEXT("显式配置前不读 Visual Snapshot"),
		ElementPresentation->GetStats().SnapshotReadCount, uint64(0));
	TestTrue(TEXT("注入观察阈值配置"), ElementPresentation->Configure(Config));
	ElementPresentation->SetSynchronousBuildsForTesting(true);
	if (!RegisterTestDefinition(*this, *ElementPresentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	TSharedRef<FElementVisualJournal, ESPMode::ThreadSafe> Journal =
		MakeShared<FElementVisualJournal, ESPMode::ThreadSafe>();
	const FElementVisualDescriptor Descriptor = MakeVisual(1001);
	TestEqual(TEXT("Producer 注入 Visual"), Journal->Upsert(Descriptor), EElementVisualMutationResult::Applied);
	TestTrue(TEXT("注册只读 Visual Source"), ElementPresentation->RegisterVisualSource(Journal).IsSet());

	TestTrue(TEXT("初次装载排空"), ElementPresentation->PumpUntilIdleForTesting());
	TestTrue(TEXT("覆盖内 Visual 已 Apply"), ElementPresentation->IsVisualAppliedForTesting(Descriptor.Key));
	const FElementPresentationStats StableStats = ElementPresentation->GetStats();

	View.ViewLocation += FVector(5.0, 0.0, 0.0);
	View.SubjectLocation += FVector(5.0, 0.0, 0.0);
	const FRotationMatrix SmallYaw(FRotator(0.0, 10.0, 0.0));
	View.Forward = SmallYaw.GetUnitAxis(EAxis::X);
	View.Right = SmallYaw.GetUnitAxis(EAxis::Y);
	View.Up = SmallYaw.GetUnitAxis(EAxis::Z);
	View.Revision = 2;
	TestTrue(TEXT("公共 Source 接受轻微更新"), Presentation->UpdateSource(ViewHandle, View));
	const FElementPresentationStats JitterStats = ElementPresentation->GetStats();
	TestEqual(TEXT("轻微位移和转动不重选 Coverage"),
		JitterStats.CoverageRecomputeCount, StableStats.CoverageRecomputeCount);
	TestEqual(TEXT("轻微抖动不读 Snapshot"), JitterStats.SnapshotReadCount, StableStats.SnapshotReadCount);
	TestEqual(TEXT("轻微抖动不创建 Build"), JitterStats.BuildDispatchCount, StableStats.BuildDispatchCount);

	const FRotationMatrix LargeYaw(FRotator(0.0, 36.0, 0.0));
	View.Forward = LargeYaw.GetUnitAxis(EAxis::X);
	View.Right = LargeYaw.GetUnitAxis(EAxis::Y);
	View.Up = LargeYaw.GetUnitAxis(EAxis::Z);
	View.Revision = 3;
	TestTrue(TEXT("公共 Source 接受跨阈值更新"), Presentation->UpdateSource(ViewHandle, View));
	const FElementPresentationStats InvalidatedStats = ElementPresentation->GetStats();
	TestEqual(TEXT("跨 35 度只重算一次 Coverage"),
		InvalidatedStats.CoverageRecomputeCount, StableStats.CoverageRecomputeCount + 1);
	TestEqual(TEXT("Retained Shard 不重复读 Snapshot"),
		InvalidatedStats.SnapshotReadCount, StableStats.SnapshotReadCount);

	const FRotationMatrix LargePitch(FRotator(55.0, 36.0, 0.0));
	View.Forward = LargePitch.GetUnitAxis(EAxis::X);
	View.Right = LargePitch.GetUnitAxis(EAxis::Y);
	View.Up = LargePitch.GetUnitAxis(EAxis::Z);
	View.Revision = 4;
	TestTrue(TEXT("公共 Source 接受跨 Pitch 阈值更新"), Presentation->UpdateSource(ViewHandle, View));
	const FElementPresentationStats PitchStats = ElementPresentation->GetStats();
	TestEqual(TEXT("Pitch 使用独立重心阈值"),
		PitchStats.CoverageRecomputeCount, InvalidatedStats.CoverageRecomputeCount + 1);

	View.HorizontalFOVDegrees = 80.0f;
	View.Revision = 5;
	TestTrue(TEXT("公共 Source 接受 FOV 更新"), Presentation->UpdateSource(ViewHandle, View));
	const FElementPresentationStats FOVStats = ElementPresentation->GetStats();
	TestEqual(TEXT("FOV 形状变化独立触发一次 Coverage 重算"),
		FOVStats.CoverageRecomputeCount, PitchStats.CoverageRecomputeCount + 1);

	DestroyObservationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationMultiViewCoverageTest,
	"ElementSandbox.Element.Presentation.Observation.MultiViewRefCountAndGrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationMultiViewCoverageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	UWorld* World = CreateObservationWorld(TEXT("ElementPresentationMultiViewCoverage"));
	if (!TestNotNull(TEXT("创建多观察源测试 World"), World))
	{
		return false;
	}
	UElementPresentationWorldSubsystem* ElementPresentation =
		World->GetSubsystem<UElementPresentationWorldSubsystem>();
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!TestNotNull(TEXT("ElementPresentation Subsystem 存在"), ElementPresentation)
		|| !TestNotNull(TEXT("公共 Presentation Source 存在"), Presentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	TestTrue(TEXT("注入多源测试配置"), ElementPresentation->Configure(MakeTestConfig()));
	ElementPresentation->SetSynchronousBuildsForTesting(true);
	if (!RegisterTestDefinition(*this, *ElementPresentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	TSharedRef<FElementVisualJournal, ESPMode::ThreadSafe> Journal =
		MakeShared<FElementVisualJournal, ESPMode::ThreadSafe>();
	const FElementVisualDescriptor Descriptor = MakeVisual(2001);
	Journal->Upsert(Descriptor);
	ElementPresentation->RegisterVisualSource(Journal);

	const FPresentationSourceHandle First = Presentation->RegisterSource(MakeView());
	FPresentationViewSource SecondView = MakeView();
	SecondView.Priority = 1;
	const FPresentationSourceHandle Second = Presentation->RegisterSource(SecondView);
	TestTrue(TEXT("两个观察源都注册"), First.IsSet() && Second.IsSet());
	TestTrue(TEXT("初次装载排空"), ElementPresentation->PumpUntilIdleForTesting());
	TestEqual(TEXT("同一 Shard 共享两个 Coverage 引用"),
		ElementPresentation->GetCoverageRefCountForTesting(Descriptor.Shard), 2);
	const FElementPresentationStats TwoSourceStats = ElementPresentation->GetStats();
	TestEqual(TEXT("多相机同一 Shard 只 Build 一次"), TwoSourceStats.BuildDispatchCount, uint64(1));

	TestTrue(TEXT("移除第一个观察源"), Presentation->UnregisterSource(First));
	TestEqual(TEXT("另一个观察源保留引用"),
		ElementPresentation->GetCoverageRefCountForTesting(Descriptor.Shard), 1);
	TestTrue(TEXT("仍有引用时实例保留"), ElementPresentation->IsVisualAppliedForTesting(Descriptor.Key));

	TestTrue(TEXT("移除最后一个观察源"), Presentation->UnregisterSource(Second));
	TestEqual(TEXT("最后一个观察源离开后 RefCount 归零"),
		ElementPresentation->GetCoverageRefCountForTesting(Descriptor.Shard), 0);
	TestTrue(TEXT("Grace 到期前实例仍保留"), ElementPresentation->IsVisualAppliedForTesting(Descriptor.Key));
	ElementPresentation->ExpireAllGraceForTesting();
	TestTrue(TEXT("Grace Remove 排空"), ElementPresentation->PumpUntilIdleForTesting());
	TestFalse(TEXT("Grace 到期后实例移除"), ElementPresentation->IsVisualAppliedForTesting(Descriptor.Key));
	const FElementPresentationStats RemovedStats = ElementPresentation->GetStats();
	TestEqual(TEXT("空页进入专用备用池"), RemovedStats.SparePageCount, 1);
	TestEqual(TEXT("没有非法可见移除"), RemovedStats.InvalidVisibleRemovalCount, uint64(0));

	DestroyObservationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationInFlightChaseTest,
	"ElementSandbox.Element.Presentation.Pipeline.InFlightStaleResultChasesLatest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationInFlightChaseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	UWorld* World = CreateObservationWorld(TEXT("ElementPresentationInFlightChase"));
	if (!TestNotNull(TEXT("创建在途追赶测试 World"), World))
	{
		return false;
	}
	UElementPresentationWorldSubsystem* ElementPresentation =
		World->GetSubsystem<UElementPresentationWorldSubsystem>();
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!TestNotNull(TEXT("ElementPresentation Subsystem 存在"), ElementPresentation)
		|| !TestNotNull(TEXT("公共 Presentation Source 存在"), Presentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	TestTrue(TEXT("注入在途测试配置"), ElementPresentation->Configure(MakeTestConfig()));
	ElementPresentation->SetSynchronousBuildsForTesting(true);
	ElementPresentation->SetBuildDispatchHeldForTesting(true);
	if (!RegisterTestDefinition(*this, *ElementPresentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	TSharedRef<FElementVisualJournal, ESPMode::ThreadSafe> Journal =
		MakeShared<FElementVisualJournal, ESPMode::ThreadSafe>();
	FElementVisualDescriptor Descriptor = MakeVisual(3001, 1);
	Journal->Upsert(Descriptor);
	ElementPresentation->RegisterVisualSource(Journal);
	Presentation->RegisterSource(MakeView());
	TestEqual(TEXT("初次 Target 只派发一个被挂起的 Build"),
		ElementPresentation->GetStats().BuildDispatchCount, uint64(1));

	Descriptor.Revision = 2;
	Descriptor.Intensity = 2.0f;
	TestEqual(TEXT("在途期间接收更高 Revision"),
		Journal->Upsert(Descriptor), EElementVisualMutationResult::Applied);
	TestEqual(TEXT("在途变化被合并而不重叠派发"),
		ElementPresentation->GetStats().BuildDispatchCount, uint64(1));
	ElementPresentation->ReleaseHeldBuildsForTesting();
	TestTrue(TEXT("旧结果丢弃并追赶最新结果"), ElementPresentation->PumpUntilIdleForTesting());
	TestEqual(TEXT("最终 Apply 最新 Revision"),
		ElementPresentation->GetAppliedVisualRevisionForTesting(Descriptor.Key), uint64(2));
	const FElementPresentationStats ChasedStats = ElementPresentation->GetStats();
	TestEqual(TEXT("只丢弃一个过期结果"), ChasedStats.BuildStaleDiscardCount, uint64(1));
	TestEqual(TEXT("只追赶一个 PendingDelta"), ChasedStats.PendingDeltaChaseCount, uint64(1));
	TestEqual(TEXT("追赶后总派发两次"), ChasedStats.BuildDispatchCount, uint64(2));

	const uint64 StableBuilds = ChasedStats.BuildDispatchCount;
	const uint64 StableApplies = ChasedStats.ApplyCommandCount;
	TestEqual(TEXT("相同 Revision 重放是 Producer 空操作"),
		Journal->Upsert(Descriptor), EElementVisualMutationResult::Unchanged);
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		TestFalse(TEXT("队列排空后始终不 Tick"), ElementPresentation->IsTickable());
	}
	const FElementPresentationStats IdleStats = ElementPresentation->GetStats();
	TestEqual(TEXT("重复 Revision 零 Build"), IdleStats.BuildDispatchCount, StableBuilds);
	TestEqual(TEXT("重复 Revision 零 Apply"), IdleStats.ApplyCommandCount, StableApplies);
	TestEqual(TEXT("空闲窗口没有 Idle Tick"), IdleStats.IdleTickCount, uint64(0));

	DestroyObservationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationAsyncWorkerTest,
	"ElementSandbox.Element.Presentation.Pipeline.AsyncWorkerPureResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationAsyncWorkerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	UWorld* World = CreateObservationWorld(TEXT("ElementPresentationAsyncWorker"));
	if (!TestNotNull(TEXT("创建异步 Worker 测试 World"), World))
	{
		return false;
	}
	UElementPresentationWorldSubsystem* ElementPresentation =
		World->GetSubsystem<UElementPresentationWorldSubsystem>();
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!TestNotNull(TEXT("ElementPresentation Subsystem 存在"), ElementPresentation)
		|| !TestNotNull(TEXT("公共 Presentation Source 存在"), Presentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	ElementPresentation->Configure(MakeTestConfig());
	if (!RegisterTestDefinition(*this, *ElementPresentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	TSharedRef<FElementVisualJournal, ESPMode::ThreadSafe> Journal =
		MakeShared<FElementVisualJournal, ESPMode::ThreadSafe>();
	const FElementVisualDescriptor Descriptor = MakeVisual(3501, 1);
	Journal->Upsert(Descriptor);
	ElementPresentation->RegisterVisualSource(Journal);
	Presentation->RegisterSource(MakeView());
	TestTrue(TEXT("真实 ThreadPool Build 最终排空"),
		ElementPresentation->PumpUntilIdleForTesting(8192));
	TestTrue(TEXT("异步纯值结果在 GameThread Apply"),
		ElementPresentation->IsVisualAppliedForTesting(Descriptor.Key));
	const FElementPresentationStats Stats = ElementPresentation->GetStats();
	TestEqual(TEXT("异步 Build 派发一次"), Stats.BuildDispatchCount, uint64(1));
	TestEqual(TEXT("异步 Build 完成一次"), Stats.BuildCompleteCount, uint64(1));
	TestEqual(TEXT("异步结果没有过期"), Stats.BuildStaleDiscardCount, uint64(0));

	DestroyObservationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationLocalOverflowTest,
	"ElementSandbox.Element.Presentation.Pipeline.LocalJournalOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationLocalOverflowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	UWorld* World = CreateObservationWorld(TEXT("ElementPresentationLocalOverflow"));
	if (!TestNotNull(TEXT("创建局部 Overflow 测试 World"), World))
	{
		return false;
	}
	UElementPresentationWorldSubsystem* ElementPresentation =
		World->GetSubsystem<UElementPresentationWorldSubsystem>();
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!TestNotNull(TEXT("ElementPresentation Subsystem 存在"), ElementPresentation)
		|| !TestNotNull(TEXT("公共 Presentation Source 存在"), Presentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	ElementPresentation->Configure(MakeTestConfig());
	ElementPresentation->SetSynchronousBuildsForTesting(true);
	if (!RegisterTestDefinition(*this, *ElementPresentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	FElementVisualJournalConfig JournalConfig;
	JournalConfig.MaxRetainedBatchesPerShard = 2;
	TSharedRef<FElementVisualJournal, ESPMode::ThreadSafe> Journal =
		MakeShared<FElementVisualJournal, ESPMode::ThreadSafe>(JournalConfig);
	FElementVisualDescriptor First = MakeVisual(4001, 1);
	FElementVisualDescriptor Second = MakeVisual(4002, 1);
	Second.Shard = FElementVisualShardKey{{1, 1, 0}};
	Second.WorldTransform.SetLocation(FVector(150.0, 150.0, 50.0));
	Second.WorldBounds = FBox::BuildAABB(FVector(150.0, 150.0, 50.0), FVector(10.0));
	Journal->BeginTransaction();
	Journal->Upsert(First);
	Journal->Upsert(Second);
	Journal->CommitTransaction();
	ElementPresentation->RegisterVisualSource(Journal);
	Presentation->RegisterSource(MakeView());
	TestTrue(TEXT("两个 Shard 初次装载排空"), ElementPresentation->PumpUntilIdleForTesting());
	const FElementPresentationStats BeforeOverflow = ElementPresentation->GetStats();
	uint64 StableSecondTargetRevision = 0;
	for (const FElementPresentationShardDebug& Debug : ElementPresentation->CopyDebugSnapshot().Shards)
	{
		if (Debug.Shard == Second.Shard)
		{
			StableSecondTargetRevision = Debug.TargetRevision;
		}
	}

	ElementPresentation->SetJournalConsumptionPausedForTesting(true);
	for (uint64 Revision = 2; Revision <= 4; ++Revision)
	{
		First.Revision = Revision;
		First.Intensity = static_cast<float>(Revision);
		Journal->Upsert(First);
	}
	ElementPresentation->SetJournalConsumptionPausedForTesting(false);
	TestTrue(TEXT("局部 Snapshot 恢复后排空"), ElementPresentation->PumpUntilIdleForTesting());
	const FElementPresentationStats AfterOverflow = ElementPresentation->GetStats();
	TestEqual(TEXT("Overflow 只触发一次局部重建"), AfterOverflow.LocalRebuildCount, uint64(1));
	TestEqual(TEXT("没有全局重建"), AfterOverflow.GlobalRebuildCount, uint64(0));
	TestEqual(TEXT("只为落后 Shard 增加一次 Build"),
		AfterOverflow.BuildDispatchCount, BeforeOverflow.BuildDispatchCount + 1);
	TestEqual(TEXT("落后 Shard 追到最新 Revision"),
		ElementPresentation->GetAppliedVisualRevisionForTesting(First.Key), uint64(4));
	for (const FElementPresentationShardDebug& Debug : ElementPresentation->CopyDebugSnapshot().Shards)
	{
		if (Debug.Shard == Second.Shard)
		{
			TestEqual(TEXT("无关 Shard Target Revision 不变"),
				Debug.TargetRevision, StableSecondTargetRevision);
		}
	}

	DestroyObservationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationBudgetPoolTest,
	"ElementSandbox.Element.Presentation.Pipeline.ApplyBudgetFailureAndPoolReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationBudgetPoolTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	UWorld* World = CreateObservationWorld(TEXT("ElementPresentationBudgetPool"));
	if (!TestNotNull(TEXT("创建预算与备用池测试 World"), World))
	{
		return false;
	}
	UElementPresentationWorldSubsystem* ElementPresentation =
		World->GetSubsystem<UElementPresentationWorldSubsystem>();
	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	if (!TestNotNull(TEXT("ElementPresentation Subsystem 存在"), ElementPresentation)
		|| !TestNotNull(TEXT("公共 Presentation Source 存在"), Presentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	FElementPresentationConfig Config = MakeTestConfig();
	Config.MaxApplyCommandsPerTick = 1;
	ElementPresentation->Configure(Config);
	ElementPresentation->SetSynchronousBuildsForTesting(true);
	if (!RegisterTestDefinition(*this, *ElementPresentation))
	{
		DestroyObservationWorld(World);
		return false;
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	FElementVisualDefinition HierarchicalDefinition;
	HierarchicalDefinition.DefinitionId = TEXT("Test.ElementVisual.HISM");
	HierarchicalDefinition.StaticMesh = Mesh;
	HierarchicalDefinition.Backend = EElementVisualInstanceBackend::Hierarchical;
	HierarchicalDefinition.CustomDataFloatCount = 5;
	TestTrue(TEXT("注册 HISM Definition"),
		ElementPresentation->RegisterVisualDefinition(HierarchicalDefinition));

	TSharedRef<FElementVisualJournal, ESPMode::ThreadSafe> Journal =
		MakeShared<FElementVisualJournal, ESPMode::ThreadSafe>();
	TArray<FElementVisualDescriptor> Descriptors;
	for (uint64 EntityId = 5001; EntityId <= 5004; ++EntityId)
	{
		FElementVisualDescriptor Descriptor = MakeVisual(EntityId, 1);
		Descriptor.WorldTransform.AddToTranslation(FVector(0.0, static_cast<double>(EntityId - 5001) * 5.0, 0.0));
		Descriptor.WorldBounds = FBox::BuildAABB(Descriptor.WorldTransform.GetLocation(), FVector(10.0));
		if (EntityId == 5004)
		{
			Descriptor.VisualDefinitionId = HierarchicalDefinition.DefinitionId;
		}
		Descriptors.Add(Descriptor);
	}
	Journal->BeginTransaction();
	for (const FElementVisualDescriptor& Descriptor : Descriptors)
	{
		Journal->Upsert(Descriptor);
	}
	Journal->CommitTransaction();
	ElementPresentation->RegisterVisualSource(Journal);
	ElementPresentation->SetApplyFailureCountForTesting(1);
	Presentation->RegisterSource(MakeView());
	TestTrue(TEXT("失败重试与预算积压最终排空"), ElementPresentation->PumpUntilIdleForTesting());
	const FElementPresentationStats LoadedStats = ElementPresentation->GetStats();
	TestEqual(TEXT("一次注入 Apply 失败被重试"), LoadedStats.ApplyFailedCount, uint64(1));
	TestTrue(TEXT("数量预算确实跨帧"), LoadedStats.ApplyBudgetExhaustedCount >= 3);
	TestEqual(TEXT("四个 Visual 全部 Applied"), LoadedStats.AppliedVisualCount, 4);
	TestEqual(TEXT("Element 专用 ISM 页存在"), LoadedStats.ISMComponentCount, 1);
	TestEqual(TEXT("Element 专用 HISM 页存在"), LoadedStats.HISMComponentCount, 1);
	TestEqual(TEXT("两种后端各分配一页"), LoadedStats.PoolPageAllocateCount, uint64(2));

	Journal->BeginTransaction();
	for (const FElementVisualDescriptor& Descriptor : Descriptors)
	{
		Journal->Remove(Descriptor.Shard, Descriptor.Key,
			EElementVisualChangeKind::RuntimeEvict, 2);
	}
	Journal->CommitTransaction();
	TestTrue(TEXT("RuntimeEvict Remove 排空"), ElementPresentation->PumpUntilIdleForTesting());
	const FElementPresentationStats SpareStats = ElementPresentation->GetStats();
	TestEqual(TEXT("ISM/HISM 空页都进入备用池"), SpareStats.SparePageCount, 2);
	TestEqual(TEXT("删除后 Applied 归零"), SpareStats.AppliedVisualCount, 0);

	FElementVisualDescriptor Reused = MakeVisual(5010, 1);
	Journal->Upsert(Reused);
	TestTrue(TEXT("备用页复用后的 Add 排空"), ElementPresentation->PumpUntilIdleForTesting());
	const FElementPresentationStats ReusedStats = ElementPresentation->GetStats();
	TestEqual(TEXT("复用不新增 Component 页"), ReusedStats.PoolPageAllocateCount, uint64(2));
	TestEqual(TEXT("至少复用一个匹配后端备用页"), ReusedStats.PoolPageReuseCount, uint64(1));
	TestEqual(TEXT("硬性非法移除计数为零"), ReusedStats.InvalidVisibleRemovalCount, uint64(0));

	DestroyObservationWorld(World);
	return true;
}

#endif
