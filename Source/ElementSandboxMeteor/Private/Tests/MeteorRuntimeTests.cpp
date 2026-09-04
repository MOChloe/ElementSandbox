#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "MeteorBallisticKernel.h"
#include "MeteorClientRuntime.h"
#include "MeteorDebrisVisualPlan.h"
#include "MeteorPageScheduler.h"
#include "MeteorSettlementQueue.h"
#include "Storage/WorldChunkCodec.h"

namespace UE::ElementSandbox::Meteor
{
namespace
{
	FMeteorDebrisSeed MakeSeed(
		const uint32 Ordinal,
		const double StartTime,
		const double Deadline,
		const FVector& Start = FVector(0.0, 0.0, 1000.0),
		const FVector& Velocity = FVector(100.0, 0.0, 400.0))
	{
		FMeteorDebrisSeed Seed;
		Seed.Key = {{77}, Ordinal};
		Seed.WorldEntityId = FWorldEntityId(1000000ull + Ordinal);
		Seed.RenderArchetypeId = TEXT("WorldObject.WoodBlock");
		Seed.StartPosition = Start;
		Seed.InitialVelocity = Velocity;
		Seed.AngularVelocityDegrees = FVector(10.0, 20.0, 30.0);
		Seed.Scale = FVector3f::OneVector;
		Seed.ProductLocalBounds = FBox3f(
			FVector3f(-70.0f, -24.0f, -20.0f),
			FVector3f(70.0f, 24.0f, 20.0f));
		Seed.VisualRadius = 100.0f;
		Seed.StartTimeSeconds = StartTime;
		Seed.ValidFromSeconds = StartTime;
		Seed.LatestComputeStartSeconds = Deadline;
		return Seed;
	}

	bool CompileSinglePage(
		const TArray<FMeteorDebrisSeed>& Seeds,
		FMeteorTrajectoryPage& OutPage,
		const float GroundPlaneZ = 0.0f)
	{
		FMeteorWorkPage Work;
		Work.Reset({0, 1}, Seeds.IsEmpty() ? NAME_None : Seeds[0].RenderArchetypeId,
			FVector3d::ZeroVector);
		for (const FMeteorDebrisSeed& Seed : Seeds)
		{
			if (!Work.Append(Seed)) return false;
		}
		FMeteorRuntimeConfig Config;
		Config.GroundPlaneZ = GroundPlaneZ;
		return FMeteorBallisticKernel::CompilePage(Work, Config, {77}, 1, OutPage);
	}

	bool PreparePage(FMeteorClientRuntime& Runtime, const FMeteorTrajectoryPage& Page)
	{
			return Runtime.PrepareTrajectoryPage(MakeShared<FMeteorTrajectoryPage>(Page));
	}

	bool ActivatePage(
		FMeteorClientRuntime& Runtime,
		const FMeteorTrajectoryPage& Page,
		const TConstArrayView<uint32> Ordinals,
		const double AuthorityStartTimeSeconds = 0.0)
	{
		return Runtime.ActivateTrajectoryLanes(
			Page.PageId,
			Page.Revision,
			Ordinals,
				AuthorityStartTimeSeconds) == Ordinals.Num();
	}

	bool PrepareAndActivate(
		FMeteorClientRuntime& Runtime,
		const FMeteorTrajectoryPage& Page,
		const double AuthorityStartTimeSeconds = 0.0)
	{
		return PreparePage(Runtime, Page)
			&& ActivatePage(Runtime, Page, Page.Ordinals, AuthorityStartTimeSeconds);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorBallisticGroundPlaneTest,
	"ElementSandbox.Meteor.Kernel.BallisticGroundPlane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorBallisticGroundPlaneTest::RunTest(const FString& Parameters)
{
	float Seconds = 0.0f;
	TestTrue(TEXT("向上抛出的碎片存在正根"),
		FMeteorBallisticKernel::SolveGroundIntersectionSeconds(1000.0f, 400.0f, -980.0f, 0.0f, Seconds));
	TestTrue(TEXT("落地时间为正且有限"), Seconds > 0.0f && FMath::IsFinite(Seconds));
	const FVector3f Endpoint = FMeteorBallisticKernel::SamplePosition(
		FVector3f(0.0f, 0.0f, 1000.0f), FVector3f(100.0f, 0.0f, 400.0f),
		FVector3f(0.0f, 0.0f, -980.0f), Seconds);
	TestTrue(TEXT("解析交点位于统一地面"), FMath::IsNearlyEqual(Endpoint.Z, 0.0f, 0.05f));

	TArray<FMeteorDebrisSeed> Seeds;
	for (uint32 Index = 0; Index < 9; ++Index)
	{
		Seeds.Add(MakeSeed(Index, 10.0 + Index * 0.01, 8.0,
			FVector(10000000.0 + Index * 50.0, -20000000.0, 1000.0 + Index),
			FVector(100.0 + Index, -40.0, 400.0)));
	}
	FMeteorTrajectoryPage Page;
	TestTrue(TEXT("SIMD 页面编译成功（含标量尾部）"), CompileSinglePage(Seeds, Page));
	for (int32 Lane = 0; Lane < Page.Num(); ++Lane)
	{
		const FVector3f SampledImpact = FMeteorBallisticKernel::SamplePosition(
			Page.LocalStarts[Lane], Page.InitialVelocities[Lane], Page.Accelerations[Lane],
			Page.ImpactDurations[Lane]);
		TestTrue(TEXT("首次触地点与 Kernel 一致"),
			SampledImpact.Equals(Page.LocalImpactEndpoints[Lane], 0.1f));
		const FQuat4f ImpactRotation = FMeteorBallisticKernel::SampleRotation(
			Page.StartRotations[Lane], Page.AngularVelocitiesDegrees[Lane],
			Page.ImpactDurations[Lane]);
		const FVector WorldImpact = FVector(
			Page.PageOrigin + FVector3d(Page.LocalImpactEndpoints[Lane]));
		const FBox ImpactBounds = FBox(
			FVector(Seeds[Lane].ProductLocalBounds.Min),
			FVector(Seeds[Lane].ProductLocalBounds.Max)).TransformBy(FTransform(
				FQuat(ImpactRotation), WorldImpact, FVector(Page.Scales[Lane])));
		TestTrue(TEXT("首次触地姿态最低点精确接触地面"),
			FMath::IsNearlyEqual(ImpactBounds.Min.Z, 0.0, 0.15));

		const FVector WorldRest = FVector(
			Page.PageOrigin + FVector3d(Page.LocalRestEndpoints[Lane]));
		const FBox RestBounds = FBox(
			FVector(Seeds[Lane].ProductLocalBounds.Min),
			FVector(Seeds[Lane].ProductLocalBounds.Max)).TransformBy(FTransform(
				FQuat(Page.RestRotations[Lane]), WorldRest, FVector(Page.Scales[Lane])));
		TestTrue(TEXT("结算终态最低点精确接触地面"),
			FMath::IsNearlyEqual(RestBounds.Min.Z, 0.0, 0.15));
		TestTrue(TEXT("触地后保留非零翻滚结算时间"), Page.SettlingDurations[Lane] > 0.0f);

		FVector3f MidPosition;
		FQuat4f MidRotation;
		FMeteorBallisticKernel::SampleSettlingPose(
			Page.LocalImpactEndpoints[Lane], ImpactRotation,
			Page.LocalRestEndpoints[Lane], Page.RestRotations[Lane],
			Page.SettlingLiftHeights[Lane], 0.5f, MidPosition, MidRotation);
		const FVector WorldMid = FVector(Page.PageOrigin + FVector3d(MidPosition));
		const FBox MidBounds = FBox(
			FVector(Seeds[Lane].ProductLocalBounds.Min),
			FVector(Seeds[Lane].ProductLocalBounds.Max)).TransformBy(FTransform(
				FQuat(MidRotation), WorldMid, FVector(Page.Scales[Lane])));
		TestTrue(TEXT("翻滚中段不会穿过统一地面"), MidBounds.Min.Z >= -0.5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorExplosionHemisphereTest,
	"ElementSandbox.Meteor.Kernel.ExplosionHemisphere",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorExplosionHemisphereTest::RunTest(const FString& Parameters)
{
	FVector3f Left;
	FVector3f Center;
	FVector3f Right;
	TestTrue(TEXT("外向半平面左侧可构造"),
		FMeteorBallisticKernel::BuildOutwardExplosionLaunchVelocity(
			FVector3f::ForwardVector, -80.0f, 12.0f, 20000.0f, Left));
	TestTrue(TEXT("外向径向中心可构造"),
		FMeteorBallisticKernel::BuildOutwardExplosionLaunchVelocity(
			FVector3f::ForwardVector, 0.0f, 12.0f, 20000.0f, Center));
	TestTrue(TEXT("外向半平面右侧可构造"),
		FMeteorBallisticKernel::BuildOutwardExplosionLaunchVelocity(
			FVector3f::ForwardVector, 80.0f, 12.0f, 20000.0f, Right));
	TestTrue(TEXT("宽扇面覆盖两侧且都远离爆心"),
		Left.Y < 0.0f && Right.Y > 0.0f && Left.X > 0.0f && Center.X > 0.0f && Right.X > 0.0f);
	FVector3f Rejected;
	TestFalse(TEXT("切向或反向偏转被拒绝"),
		FMeteorBallisticKernel::BuildOutwardExplosionLaunchVelocity(
			FVector3f::ForwardVector, 90.0f, 12.0f, 20000.0f, Rejected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorShockwaveTimingTest,
	"ElementSandbox.Meteor.Shockwave.ImpactCoreArrival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorShockwaveTimingTest::RunTest(const FString& Parameters)
{
	FMeteorRuntimeConfig Config;
	Config.ShockwaveRadius = 600000.0f;
	Config.ImpactCoreRadius = 200000.0f;
	Config.ShockwaveSpeed = 300000.0f;
	TestTrue(TEXT("冲击波配置合法"), Config.IsValid());
	constexpr double ImpactTime = 50.0;
	TestEqual(TEXT("撞击中心在 ImpactTime 到达"),
		Config.ComputeShockwaveArrivalTime(ImpactTime, 0.0), ImpactTime);
	TestEqual(TEXT("核心边缘同步到达"),
		Config.ComputeShockwaveArrivalTime(ImpactTime, 200000.0), ImpactTime);
	TestEqual(TEXT("外围按波速传播"),
		Config.ComputeShockwaveArrivalTime(ImpactTime, 600000.0), ImpactTime + 4.0 / 3.0);
	TestEqual(TEXT("传播半径精确钳制"), Config.ComputeShockwaveRadius(8.0), 600000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorTrajectoryPayloadAndCausalActivationTest,
	"ElementSandbox.Meteor.Protocol.ImmutablePayloadAndCausalActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorTrajectoryPayloadAndCausalActivationTest::RunTest(const FString& Parameters)
{
	FMeteorTrajectoryPage Page;
	TestTrue(TEXT("准备轨迹页"), CompileSinglePage({MakeSeed(4097, 3.0, 1.0)}, Page));
	TArray<uint8> Bytes;
	TestTrue(TEXT("合法页可序列化"), Page.SerializeToBytes(Bytes));
	FMeteorTrajectoryPage Decoded;
	TestTrue(TEXT("合法页可反序列化"), FMeteorTrajectoryPage::DeserializeFromBytes(Bytes, Decoded));
	TestTrue(TEXT("不可变弹道与结算系数保持"),
		Decoded.LocalStarts[0].Equals(Page.LocalStarts[0], UE_KINDA_SMALL_NUMBER)
		&& Decoded.InitialVelocities[0].Equals(Page.InitialVelocities[0], UE_KINDA_SMALL_NUMBER)
		&& Decoded.Accelerations[0].Equals(Page.Accelerations[0], UE_KINDA_SMALL_NUMBER)
		&& Decoded.LocalImpactEndpoints[0].Equals(
			Page.LocalImpactEndpoints[0], UE_KINDA_SMALL_NUMBER)
		&& Decoded.LocalRestEndpoints[0].Equals(
			Page.LocalRestEndpoints[0], UE_KINDA_SMALL_NUMBER)
		&& Decoded.RestRotations[0].Equals(Page.RestRotations[0], UE_KINDA_SMALL_NUMBER)
		&& FMath::IsNearlyEqual(
			Decoded.SettlingLiftHeights[0], Page.SettlingLiftHeights[0]));

	FMeteorTrajectoryActivation Activation;
	Activation.BurstId = Page.BurstId;
	Activation.PageId = Page.PageId;
	Activation.Revision = Page.Revision;
	Activation.AuthorityStartTimeSeconds = 3.0;
	Activation.Ordinals = Page.Ordinals;
	TestFalse(TEXT("缺少源身份与 Tombstone 的 Activate 非法"), Activation.IsValid());
	Activation.SourceWorldEntityId = FWorldEntityId(991);
	Activation.SourceTombstoneRevision = 7;
	TestTrue(TEXT("Activate 完整携带源身份、Tombstone 与权威时间"), Activation.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorDeadlineSchedulerTest,
	"ElementSandbox.Meteor.Scheduler.DeadlineAndGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorDeadlineSchedulerTest::RunTest(const FString& Parameters)
{
	FMeteorRuntimeConfig Config;
	Config.MaximumWorkPages = 2;
	FMeteorPageScheduler Scheduler;
	TestTrue(TEXT("调度器初始化"), Scheduler.Initialize({77}, Config, 0.0));
	TestTrue(TEXT("远期 Lane 入队"), Scheduler.EnqueueSeed(MakeSeed(0, 5.0, 2.0)));
	Scheduler.Pump(2.01);
	FMeteorPageHandle FirstHandle;
	FMeteorWorkPage Work;
	TestTrue(TEXT("到期半页可抢占"), Scheduler.TryAcquireWork(FirstHandle, Work));
	FMeteorTrajectoryPage Completed;
	TestTrue(TEXT("Worker 编译"), FMeteorBallisticKernel::CompilePage(Work, Config, {77}, 1, Completed));
	TestTrue(TEXT("完成页提交"), Scheduler.CompleteWork(FirstHandle, MoveTemp(Completed)));
	FMeteorPageHandle ConsumedHandle;
	TestTrue(TEXT("完成页消费"), Scheduler.ConsumeCompleted(ConsumedHandle, Completed));
	TestTrue(TEXT("槽位回收"), Scheduler.ReleaseCompleted(ConsumedHandle));
	TestTrue(TEXT("回收后新 Lane 可复用槽"), Scheduler.EnqueueSeed(MakeSeed(1, 5.0, 2.02)));
	Scheduler.Pump(2.03);
	FMeteorPageHandle SecondHandle;
	TestTrue(TEXT("新代页面可执行"), Scheduler.TryAcquireWork(SecondHandle, Work));
	TestEqual(TEXT("复用相同槽"), SecondHandle.Slot, FirstHandle.Slot);
	TestNotEqual(TEXT("Generation 必须变化"), SecondHandle.Generation, FirstHandle.Generation);
	TestFalse(TEXT("旧 Generation 不能完成新页"), Scheduler.FailWork(FirstHandle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorSettlementProximityPriorityTest,
	"ElementSandbox.Meteor.Settlement.ProximityPriorityAndGlobalProgress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorSettlementProximityPriorityTest::RunTest(const FString& Parameters)
{
	FMeteorBurstId BurstId;
	BurstId.Value = 901;
	FMeteorSettlementQueue Queue;
	TestTrue(TEXT("近场优先队列初始化"), Queue.Initialize(BurstId, 1000.0f, 3000.0f));
	const auto Enqueue = [&Queue, BurstId](
		const uint32 Ordinal, const FVector& Location, const double DueTime)
	{
		FMeteorSettlementLane Lane;
		Lane.Key = {BurstId, Ordinal};
		Lane.WorldEntityId = FWorldEntityId(2000000ull + Ordinal);
		Lane.ProductDefinitionId = TEXT("WorldObject.WoodBlock");
		Lane.WorldTransform = FTransform(FQuat::Identity, Location, FVector::OneVector);
		Lane.DueTimeSeconds = DueTime;
		return Queue.Enqueue(MoveTemp(Lane));
	};
	TestTrue(TEXT("全局最老远景入队"), Enqueue(0, FVector(10000.0, 0.0, 20.0), 1.0));
	TestTrue(TEXT("脚边第一格入队"), Enqueue(1, FVector(100.0, 0.0, 20.0), 2.0));
	TestTrue(TEXT("脚边第二格入队"), Enqueue(2, FVector(1100.0, 0.0, 20.0), 3.0));
	TestTrue(TEXT("另一远景入队"), Enqueue(3, FVector(12000.0, 0.0, 20.0), 4.0));

	TArray<FMeteorSettlementReservation> Reservations;
	const auto ReservedOrdinal = [&Queue](const FMeteorSettlementReservation& Reservation)
	{
		const FMeteorSettlementLane* Lane = Queue.FindReserved(Reservation);
		return Lane ? Lane->Key.DebrisOrdinal : MAX_uint32;
	};
	const TArray<FVector> OriginPlayer = {FVector::ZeroVector};
	TestEqual(TEXT("一批预留固定三个"),
		Queue.ReserveDue(OriginPlayer, 10.0, 3, 1, Reservations), 3);
	TestTrue(TEXT("前两个名额按角色 Cell 距离优先"),
		Reservations.Num() == 3
		&& Reservations[0].Source == EMeteorSettlementReservationSource::ProximityCell
		&& Reservations[1].Source == EMeteorSettlementReservationSource::ProximityCell
		&& ReservedOrdinal(Reservations[0]) == 1
		&& ReservedOrdinal(Reservations[1]) == 2);
	TestTrue(TEXT("保留名额推进全局最老远景"),
		Reservations.Num() == 3
		&& Reservations[2].Source == EMeteorSettlementReservationSource::GlobalOldest
		&& ReservedOrdinal(Reservations[2]) == 0);
	for (const FMeteorSettlementReservation& Reservation : Reservations)
	{
		TestTrue(TEXT("失败批次可完整回滚双索引"), Queue.RollbackReserved(Reservation));
	}

	const TArray<FVector> FarPlayer = {FVector(12000.0, 0.0, 0.0)};
	TestEqual(TEXT("角色移动后远景立即成为近场"),
		Queue.ReserveDue(FarPlayer, 10.0, 2, 0, Reservations), 2);
	TestTrue(TEXT("新脚边 Cell 先于原脚边处理"),
		Reservations.Num() == 2
		&& ReservedOrdinal(Reservations[0]) == 3
		&& ReservedOrdinal(Reservations[1]) == 0);
	for (const FMeteorSettlementReservation& Reservation : Reservations)
	{
		TestTrue(TEXT("近场提交成功"), Queue.CommitReserved(Reservation));
	}

	const TArray<FVector> NoPlayers;
	TestEqual(TEXT("无角色时退化为全局到期顺序"),
		Queue.ReserveDue(NoPlayers, 10.0, 4, 1, Reservations), 2);
	TestTrue(TEXT("全局剩余项保持 DueTime 顺序"),
		Reservations.Num() == 2
		&& ReservedOrdinal(Reservations[0]) == 1
		&& ReservedOrdinal(Reservations[1]) == 2);
	for (const FMeteorSettlementReservation& Reservation : Reservations)
	{
		TestTrue(TEXT("全局提交成功"), Queue.CommitReserved(Reservation));
	}
	TestTrue(TEXT("全部 Lane 最终完成且无饥饿"), Queue.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorDebrisVisualPlanTest,
	"ElementSandbox.Meteor.Visual.SourceAABBProducesCanonicalPickupBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorDebrisVisualPlanTest::RunTest(const FString& Parameters)
{
	FMeteorDebrisVisualPlanInput BuildingInput;
	BuildingInput.SourceBounds = FBox(FVector(-500.0, -400.0, 0.0), FVector(500.0, 400.0, 800.0));
	BuildingInput.ProductLocalBounds = FBox(FVector(-25.0), FVector(25.0));
	BuildingInput.UniformScaleRange = FVector2D(0.9, 1.1);
	BuildingInput.AngularSpeedRange = FVector2D(20.0, 80.0);
	BuildingInput.StableSeed = 1234;
	BuildingInput.ProductCount = 6;
	TArray<FMeteorDebrisVisualLane> BuildingLanes;
	TestTrue(TEXT("源 AABB 内普通木块规划成功"), BuildMeteorDebrisVisualPlan(BuildingInput, BuildingLanes));
	TestEqual(TEXT("每个产品生成一条木块 Lane"), BuildingLanes.Num(), BuildingInput.ProductCount);
	for (const FMeteorDebrisVisualLane& Lane : BuildingLanes)
	{
		const FVector Scale = Lane.FlightWorldTransform.GetScale3D();
		TestTrue(TEXT("飞行阶段保持规范木块的均匀产品尺度"),
			FMath::IsNearlyEqual(Scale.X, Scale.Y)
				&& FMath::IsNearlyEqual(Scale.Y, Scale.Z)
				&& Scale.X >= BuildingInput.UniformScaleRange.X
				&& Scale.X <= BuildingInput.UniformScaleRange.Y);
		TestTrue(TEXT("飞行和落地使用完全相同的木块尺度"),
			Scale.Equals(Lane.SettlementScale));
		TestTrue(TEXT("木块起点位于被破坏源的 Bounds 内"),
			BuildingInput.SourceBounds.IsInsideOrOn(
				Lane.FlightWorldTransform.GetLocation()));
	}

	TArray<FMeteorDebrisVisualLane> RepeatedLanes;
	TestTrue(TEXT("相同输入可重复生成木块规划"),
		BuildMeteorDebrisVisualPlan(BuildingInput, RepeatedLanes));
	TestEqual(TEXT("稳定规划数量一致"), RepeatedLanes.Num(), BuildingLanes.Num());
	for (int32 Index = 0; Index < BuildingLanes.Num() && Index < RepeatedLanes.Num(); ++Index)
	{
		TestTrue(TEXT("木块位置、姿态与尺度保持确定性"),
			BuildingLanes[Index].FlightWorldTransform.Equals(
				RepeatedLanes[Index].FlightWorldTransform)
			&& BuildingLanes[Index].SettlementScale.Equals(
				RepeatedLanes[Index].SettlementScale)
			&& BuildingLanes[Index].AngularVelocityDegrees.Equals(
				RepeatedLanes[Index].AngularVelocityDegrees));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorClientManyLaneZeroFlightWorkTest,
	"ElementSandbox.Meteor.Scale.ManyLanesHaveZeroOngoingCPUFlightWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorClientManyLaneZeroFlightWorkTest::RunTest(const FString& Parameters)
{
	constexpr int32 PageCount = 16;
	constexpr int32 LaneCount = PageCount * WorkPageCapacity;
	FMeteorClientRuntime Runtime;
	TestTrue(TEXT("大批量 Runtime 初始化"), Runtime.Initialize(FMeteorRuntimeConfig{}));
	uint32 Ordinal = 0;
	for (int32 PageIndex = 0; PageIndex < PageCount; ++PageIndex)
	{
		TArray<FMeteorDebrisSeed> Seeds;
		Seeds.Reserve(WorkPageCapacity);
		for (int32 Lane = 0; Lane < WorkPageCapacity; ++Lane)
		{
			Seeds.Add(MakeSeed(Ordinal++, 0.0, -1.0,
				FVector(Lane * 2.0, PageIndex * 2.0, 1000.0), FVector(30.0, -20.0, 250.0)));
		}
		FMeteorTrajectoryPage Page;
		TestTrue(TEXT("大批量页编译"), CompileSinglePage(Seeds, Page));
		Page.PageId = PageIndex + 1;
		TestTrue(TEXT("大批量页 Prepare/Activate"), PrepareAndActivate(Runtime, Page));
	}
	TestEqual(TEXT("所有 Lane 只登记 Flying 身份"), Runtime.GetStats().FlyingLaneCount, LaneCount);
	TArray<FMeteorClientPresentationLane> Uploads;
	Runtime.ConsumePresentationChanges(Uploads, 0.0);
	TestEqual(TEXT("Activate 阶段一次消费全部不可变数据"), Uploads.Num(), LaneCount);
	Runtime.ConsumePresentationChanges(Uploads, 0.0);
	TestEqual(TEXT("之后飞行帧没有 CPU 采样与上传工作"), Uploads.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeteorReservedPageProtocolTest,
	"ElementSandbox.Meteor.Protocol.ReservedIdentityAndVersion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeteorReservedPageProtocolTest::RunTest(const FString&)
{
	FMeteorTrajectoryPage Page, Decoded;
	TestTrue(TEXT("轨迹编译保留预分配身份"), CompileSinglePage({MakeSeed(0, 10, 8), MakeSeed(1, 10, 8)}, Page));
	TArray<uint8> Bytes;
	TestTrue(TEXT("新版轨迹页编码"), Page.SerializeToBytes(Bytes));
	TestTrue(TEXT("新版轨迹页解码"), FMeteorTrajectoryPage::DeserializeFromBytes(Bytes, Decoded));
	TestTrue(TEXT("身份逐 Lane 无损往返"), Decoded.WorldEntityIds == Page.WorldEntityIds);
	const uint32 PreviousVersion = TrajectoryPayloadFormatVersion - 1;
	FMemory::Memcpy(Bytes.GetData() + sizeof(uint32), &PreviousVersion, sizeof(uint32));
	TestFalse(TEXT("旧协议直接拒绝"), FMeteorTrajectoryPage::DeserializeFromBytes(Bytes, Decoded));
	Page.WorldEntityIds[1] = Page.WorldEntityIds[0];
	TestFalse(TEXT("同页身份冲突拒绝"), Page.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeteorClientOrderingTest,
	"ElementSandbox.Meteor.Client.LifecycleMessageOrdering", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeteorClientOrderingTest::RunTest(const FString&)
{
	FMeteorTrajectoryPage Page;
	CompileSinglePage({MakeSeed(0, 10, 8), MakeSeed(1, 10, 8), MakeSeed(2, 10, 8)}, Page);
	FMeteorClientRuntime Runtime; Runtime.Initialize(FMeteorRuntimeConfig{});
	TestTrue(TEXT("Settlement 先到"), Runtime.MarkSettled(0, Page.WorldEntityIds[0]));
	Runtime.CancelTrajectoryLanes(Page.PageId, Page.Revision, MakeArrayView(&Page.Ordinals[1], 1));
	TestTrue(TEXT("终态先到后仍接受 Payload 的其他 Lane"), PreparePage(Runtime, Page));
	TArray<FMeteorClientPresentationLane> Changes;
	Runtime.ConsumePresentationChanges(Changes, -3);
	TestEqual(TEXT("三条生命周期记录共用身份"), Changes.Num(), 3);
	TestTrue(TEXT("重复 Payload 幂等"), PreparePage(Runtime, Page));
	Runtime.ConsumePresentationChanges(Changes, -3);
	TestEqual(TEXT("重复 Payload 不重新提交"), Changes.Num(), 0);
	TestEqual(TEXT("Activate 只激活尚未落地且未取消的 Lane"), Runtime.ActivateTrajectoryLanes(Page.PageId, Page.Revision, Page.Ordinals, 100), 1);
	Runtime.ConsumePresentationChanges(Changes, -3);
	TestEqual(TEXT("只提交唯一新激活"), Changes.Num(), 1);
	if (Changes.Num() == 1)
	{
		TestEqual(TEXT("迟到激活保持原服务器时刻映射"), Changes[0].LocalStartTimeSeconds, 97.0);
		TestTrue(TEXT("落地姿态来自同一持久终态"), Changes[0].RestTransform.Equals(Page.GetRestTransform(2)));
	}
	TestFalse(TEXT("取消后的迟到 Settlement 不复活"), Runtime.MarkSettled(1, Page.WorldEntityIds[1]));
	TestTrue(TEXT("最终 Settlement 接受"), Runtime.MarkSettled(2, Page.WorldEntityIds[2]));
	Runtime.ConsumePresentationChanges(Changes, 0);
	TestEqual(TEXT("终态后释放完整轨迹页"), Runtime.GetStats().PreparedPageCount, 0);
	Runtime.MarkSettled(2, Page.WorldEntityIds[2]);
	TestFalse(TEXT("重复 Settlement 无新工作"), Runtime.HasPendingPresentationChanges());
	Runtime.Reset(); Runtime.Initialize(FMeteorRuntimeConfig{});
	Runtime.CancelTrajectoryPage(Page.PageId, Page.Revision);
	TestFalse(TEXT("整页取消先到后拒绝 Payload"), PreparePage(Runtime, Page));
	TestEqual(TEXT("整页取消结束激活等待"), Runtime.GetPreparedPageRevision(Page.PageId), Page.Revision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeteorPersistentRestPoseTest,
	"ElementSandbox.Meteor.Protocol.PersistentRestPose", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeteorPersistentRestPoseTest::RunTest(const FString&)
{
	TArray<FMeteorDebrisSeed> Seeds;
	for (uint32 I = 0; I < 64; ++I)
		Seeds.Add(MakeSeed(I, 10, 8, FVector(I * 1703.127 - 10000000, I * 29.783 - 20000, 700 + I),
			FVector(837.13 + I, -925.62 - I, 840.91)));
	FMeteorTrajectoryPage Page;
	TestTrue(TEXT("编译大坐标与负坐标轨迹"), CompileSinglePage(Seeds, Page));
	FMeteorClientRuntime Client; Client.Initialize(FMeteorRuntimeConfig{}); PreparePage(Client, Page);
	TArray<FMeteorClientPresentationLane> Changes; Client.ConsumePresentationChanges(Changes, 0);
	TestEqual(TEXT("所有准备姿态可用"), Changes.Num(), Page.Num());
	for (int32 Lane = 0; Lane < Changes.Num(); ++Lane)
	{
		FWorldChunkData Source, Decoded;
		FWorldPersistentEntityRecord Record;
		Record.EntityId = Page.WorldEntityIds[Lane]; Record.Domain = EWorldEntityDomain::WorldObject;
		Record.DefinitionId = Page.RenderArchetypeId; Record.WorldTransform = Page.GetRestTransform(Lane);
		Source.Coord = FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()); Source.Revision = 1;
		Source.Records.Add(Record); TArray<uint8> Bytes; FString Error;
		TestTrue(TEXT("终态实际编码"), FWorldChunkCodec::Encode(Source, Bytes, Error));
		TestTrue(TEXT("终态实际解码"), FWorldChunkCodec::Decode(Bytes, Decoded, Error));
		if (Decoded.Records.IsEmpty()) continue;
		TestTrue(TEXT("准备与 Snapshot 的位置一致"),
			Changes[Lane].RestTransform.GetLocation().Equals(Decoded.Records[0].WorldTransform.GetLocation(), 1e-8));
		TestTrue(TEXT("准备与 Snapshot 的完整姿态无需更新"),
			Changes[Lane].RestTransform.Equals(Decoded.Records[0].WorldTransform, 1e-4));
	}
	return true;
}
}

#endif
