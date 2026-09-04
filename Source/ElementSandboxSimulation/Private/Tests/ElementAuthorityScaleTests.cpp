#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Runtime/ElementAuthorityExecution.h"
#include "Spatial/ElementBvh.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace ElementSandbox::Authority::Scale::Tests
{
	FBox MakeBounds(const FVector& Center, const double Extent = 1.0)
	{
		return FBox(Center - FVector(Extent), Center + FVector(Extent));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementMillionStaticInfluenceIdleTest,
	"ElementSandbox.ElementRuntime.Scale.MillionStaticInfluencesRemainIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

bool FElementMillionStaticInfluenceIdleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Scale::Tests;
	constexpr int32 InfluenceCount = 1000000;
	FElementBvh InfluenceBvh;
	InfluenceBvh.Reserve(InfluenceCount);
	for (int32 Index = 0; Index < InfluenceCount; ++Index)
	{
		const FVector Center(
			static_cast<double>(Index % 1000) * 100.0,
			static_cast<double>(Index / 1000) * 100.0,
			0.0);
		if (!InfluenceBvh.Insert(MakeBounds(Center)).IsSet())
		{
			AddError(FString::Printf(TEXT("第 %d 个 Influence 插入失败。"), Index));
			return false;
		}
	}
	TestTrue(TEXT("百万 Influence 初始 BVH 发布"), InfluenceBvh.PublishSnapshot());
	const FElementBvhStats Before = InfluenceBvh.GetStats();
	for (int32 Cycle = 0; Cycle < 16; ++Cycle)
	{
		TestFalse(TEXT("静止 BVH 在 Authority Barrier 间没有发布工作"), InfluenceBvh.PublishSnapshot());
	}
	const FElementBvhStats After = InfluenceBvh.GetStats();
	TestEqual(TEXT("静止世界不增加查询"), After.QueryCount, Before.QueryCount);
	TestEqual(TEXT("静止世界不增加候选"), After.CandidateCount, Before.CandidateCount);
	TestEqual(TEXT("静止世界不增加 refit"), After.RefitCount, Before.RefitCount);
	TestEqual(TEXT("静止世界不增加 rebuild"), After.RebuildCount, Before.RebuildCount);

	FElementAuthorityExecution Scheduler;
	for (int32 Cycle = 0; Cycle < 16; ++Cycle)
	{
		TestFalse(TEXT("无变化 Authority Worker Pump 是空操作"), Scheduler.PumpWorkers(Cycle * 125, true));
	}
	TestEqual(TEXT("空闲时 Numeric Processor 工作量为零"),
		Scheduler.GetStats().NumericProcessorInvocationCount, 0ull);
	TestEqual(TEXT("空闲时 Narrowphase 工作量为零"), Scheduler.GetStats().NarrowPhaseCount, 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementSparseTenThousandByTenThousandBvhTest,
	"ElementSandbox.ElementRuntime.Scale.SparseTenThousandByTenThousandAvoidsCartesianProduct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

bool FElementSparseTenThousandByTenThousandBvhTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Scale::Tests;
	constexpr int32 Count = 10000;
	FElementBvh Bvh;
	Bvh.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Bvh.Insert(MakeBounds(FVector(Index * 1000.0, 0.0, 0.0), 5.0));
	}
	Bvh.PublishSnapshot();
	const TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> Snapshot = Bvh.GetPublishedSnapshot();
	if (!TestTrue(TEXT("稀疏 Influence Snapshot 已发布"), Snapshot.IsValid())) return false;
	FElementBvhStats QueryStats;
	TArray<FElementSpatialSnapshotHandle> Candidates;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Snapshot->Query(MakeBounds(FVector(Index * 1000.0, 0.0, 0.0), 10.0), Candidates, &QueryStats);
		if (Candidates.Num() != 1)
		{
			AddError(FString::Printf(TEXT("稀疏查询 %d 应只有一个局部候选，实际 %d。"), Index, Candidates.Num()));
			return false;
		}
	}
	TestEqual(TEXT("一万次查询只产生一万个局部候选"), QueryStats.CandidateCount, 10000ull);
	TestTrue(TEXT("节点访问远小于一亿次笛卡尔积"), QueryStats.NodeVisitCount < 1000000ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementBvhQualityRebuildTest,
	"ElementSandbox.ElementRuntime.Spatial.QualityDegradationSchedulesBackgroundRebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementBvhQualityRebuildTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Scale::Tests;
	constexpr int32 Count = 256;
	FElementBvh Bvh;
	TArray<FElementSpatialSnapshotHandle> Handles;
	Handles.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Handles.Add(Bvh.Insert(MakeBounds(FVector(Index * 100.0, 0.0, 0.0))));
	}
	Bvh.PublishSnapshot();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const double X = (Index & 1) == 0 ? 1000000.0 + Index : -1000000.0 - Index;
		Bvh.Update(Handles[Index], MakeBounds(FVector(X, 0.0, 0.0)));
	}
	TestTrue(TEXT("批量 refit 立即发布可用 Snapshot"), Bvh.PublishSnapshot());
	TestTrue(TEXT("质量下降触发后台重建而非阻塞当前查询"),
		Bvh.GetStats().BackgroundRebuildScheduledCount >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementBvhDeferredLargeTopologyTest,
	"ElementSandbox.ElementRuntime.Spatial.DeferredLargeTargetTopologyPublishesAtomically",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementBvhDeferredLargeTopologyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Scale::Tests;
	constexpr int32 InitialCount = 8192;
	FElementBvh Bvh;
	Bvh.Reserve(InitialCount + 1);
	for (int32 Index = 0; Index < InitialCount; ++Index)
	{
		TestTrue(TEXT("初始 Target 插入成功"),
			Bvh.Insert(MakeBounds(FVector(Index * 20.0, 0.0, 0.0))).IsSet());
	}
	if (!TestTrue(TEXT("初始 Target Snapshot 同步发布"), Bvh.PublishSnapshot()))
	{
		return false;
	}
	const TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> Previous = Bvh.GetPublishedSnapshot();
	if (!TestTrue(TEXT("旧 Snapshot 可用"), Previous.IsValid()))
	{
		return false;
	}

	const FVector NewCenter(1000000.0, 0.0, 0.0);
	TestTrue(TEXT("新增 Target 插入成功"), Bvh.Insert(MakeBounds(NewCenter)).IsSet());
	TestFalse(TEXT("大型拓扑不会在 GameThread 立即重建"),
		Bvh.PublishSnapshot(EElementBvhPublishMode::DeferredLargeTopology));
	TestEqual(TEXT("后台构建期间旧不可变 Snapshot 仍完整"), Previous->Num(), InitialCount);

	bool bPublished = false;
	const double Deadline = FPlatformTime::Seconds() + 10.0;
	while (!bPublished && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
		bPublished = Bvh.PublishSnapshot(EElementBvhPublishMode::DeferredLargeTopology);
	}
	if (!TestTrue(TEXT("后台 Target BVH 在时限内原子发布"), bPublished))
	{
		return false;
	}
	const TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> Current = Bvh.GetPublishedSnapshot();
	if (!TestTrue(TEXT("新 Snapshot 可用"), Current.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("新 Snapshot 包含新增 Target"), Current->Num(), InitialCount + 1);
	TArray<FElementSpatialSnapshotHandle> Candidates;
	Current->Query(MakeBounds(NewCenter, 2.0), Candidates);
	TestEqual(TEXT("新增 Target 可由新 Snapshot 查询"), Candidates.Num(), 1);
	TestTrue(TEXT("大型拓扑走过后台构建"), Bvh.GetStats().BackgroundRebuildScheduledCount >= 1);
	return true;
}

#endif
