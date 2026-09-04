#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "Runtime/ElementAuthorityExecution.h"
#include "Shape/ElementShapeKernels.h"

namespace ElementSandbox::Authority::Execution::Tests
{
	const FName TestProcessorId(TEXT("Test.Heat.Numeric"));
	const FName TestFragmentType(TEXT("Test.Heat.Source"));
	const FName TestNumericChannel(TEXT("Test.Heat"));
	const FName TestStateProcessorId(TEXT("Test.Heat.StateProcessor"));
	const FName TestStateChannel(TEXT("Test.Heated"));
	const FName TestCoolingProcessorId(TEXT("Test.Water.Cooling.Numeric"));
	const FName TestCoolingFragmentType(TEXT("Test.Water.Source"));

	struct FTestHeatSourceFragment final : FElementInfluenceFragment
	{
		double Power = 1.0;
	};

	struct FTestCoolingSourceFragment final : FElementInfluenceFragment
	{
		double CoolingPower = 1.0;
	};

	class FTestHeatNumericProcessor final : public FElementNumericProcessor
	{
	public:
		FTestHeatNumericProcessor()
		{
			Descriptor.ProcessorId = TestProcessorId;
			Descriptor.FragmentType = TestFragmentType;
			Descriptor.TargetDomains = EElementTargetDomain::Character
				| EElementTargetDomain::Building | EElementTargetDomain::WorldObject;
			Descriptor.WeightMode = EElementSpatialWeightMode::Uniform;
			Descriptor.WriteNumericChannels.Add(TestNumericChannel);
			Descriptor.RecomputedNumericChannels.Add(TestNumericChannel);
		}

		const FElementProcessorDescriptor& GetDescriptor() const override { return Descriptor; }

		bool CaptureInfluence(
			const FElementEntityRegistry& Registry,
			const FElementEntityHandle Source,
			FElementInfluenceSnapshot& OutSnapshot) const override
		{
			OutSnapshot = {};
			const FTestHeatSourceFragment* Fragment = Registry.FindFragment<FTestHeatSourceFragment>(Source);
			if (!Fragment) return false;
			OutSnapshot.Shape = Fragment->Shape;
			OutSnapshot.FragmentRevision = Fragment->Revision;
			OutSnapshot.Payload.Count = 1;
			OutSnapshot.Payload.Values[0] = Fragment->Power;
			return true;
		}

		void Execute(
			const TConstArrayView<FElementQueryStatistics> Statistics,
			TArray<FElementOffset>& OutOffsets) const override
		{
			for (const FElementQueryStatistics& Entry : Statistics)
			{
				FElementOffset& Offset = OutOffsets.AddDefaulted_GetRef();
				Offset.Target = Entry.Target;
				Offset.Channel = TestNumericChannel;
				Offset.Delta = Entry.SourcePayload.Values[0]
					* (Entry.ContactDurationSeconds > 0.0 ? Entry.IntegratedWeightSeconds : Entry.EndWeight);
			}
		}

	private:
		FElementProcessorDescriptor Descriptor;
	};

	class FTestCoolingNumericProcessor final : public FElementNumericProcessor
	{
	public:
		FTestCoolingNumericProcessor()
		{
			Descriptor.ProcessorId = TestCoolingProcessorId;
			Descriptor.FragmentType = TestCoolingFragmentType;
			Descriptor.TargetDomains = EElementTargetDomain::Character
				| EElementTargetDomain::Building | EElementTargetDomain::WorldObject;
			Descriptor.WeightMode = EElementSpatialWeightMode::Uniform;
			Descriptor.WriteNumericChannels.Add(TestNumericChannel);
			Descriptor.RecomputedNumericChannels.Add(TestNumericChannel);
		}

		const FElementProcessorDescriptor& GetDescriptor() const override { return Descriptor; }

		bool CaptureInfluence(
			const FElementEntityRegistry& Registry,
			const FElementEntityHandle Source,
			FElementInfluenceSnapshot& OutSnapshot) const override
		{
			OutSnapshot = {};
			const FTestCoolingSourceFragment* Fragment =
				Registry.FindFragment<FTestCoolingSourceFragment>(Source);
			if (!Fragment) return false;
			OutSnapshot.Shape = Fragment->Shape;
			OutSnapshot.FragmentRevision = Fragment->Revision;
			OutSnapshot.Payload.Count = 1;
			OutSnapshot.Payload.Values[0] = Fragment->CoolingPower;
			return true;
		}

		void Execute(
			const TConstArrayView<FElementQueryStatistics> Statistics,
			TArray<FElementOffset>& OutOffsets) const override
		{
			for (const FElementQueryStatistics& Entry : Statistics)
			{
				FElementOffset& Offset = OutOffsets.AddDefaulted_GetRef();
				Offset.Target = Entry.Target;
				Offset.Channel = TestNumericChannel;
				Offset.Delta = -Entry.SourcePayload.Values[0]
					* (Entry.ContactDurationSeconds > 0.0 ? Entry.IntegratedWeightSeconds : Entry.EndWeight);
			}
		}

	private:
		FElementProcessorDescriptor Descriptor;
	};

	class FTestHeatStateProcessor final : public FElementStateProcessor
	{
	public:
		explicit FTestHeatStateProcessor(const bool bInUseTargetSlotAsWakeDelay = false)
			: bUseTargetSlotAsWakeDelay(bInUseTargetSlotAsWakeDelay)
		{
			Descriptor.ProcessorId = TestStateProcessorId;
			Descriptor.TargetDomains = EElementTargetDomain::Character
				| EElementTargetDomain::Building | EElementTargetDomain::WorldObject;
			Descriptor.ReadNumericChannels.Add(TestNumericChannel);
			Descriptor.OwnedStateChannel = TestStateChannel;
		}

		const FElementProcessorDescriptor& GetDescriptor() const override { return Descriptor; }

		bool Execute(
			const FElementStateProcessorInput& Input,
			FElementStateProcessorOutput& OutOutput) const override
		{
			OutOutput = {};
			OutOutput.Target = Input.Target;
			OutOutput.NextState.SchemaId = TestStateChannel;
			OutOutput.NextState.Revision = Input.CurrentState.IsSet()
				? Input.CurrentState->Revision + 1 : 1;
			OutOutput.NextState.Payload.Count = 1;
			OutOutput.NextState.Payload.Values[0] = !Input.NumericValues.IsEmpty()
				&& Input.NumericValues[0].Value >= 2.0 ? 1.0 : 0.0;
			const bool bWasHeated = Input.CurrentState.IsSet()
				&& Input.CurrentState->Payload.Count > 0
				&& Input.CurrentState->Payload.Values[0] > 0.5;
			if (!bWasHeated && OutOutput.NextState.Payload.Values[0] > 0.5)
			{
				FElementStructuralCommand& Command = OutOutput.StructuralCommands.AddDefaulted_GetRef();
				Command.Kind = EElementStructuralCommandKind::AddInfluenceFragment;
				Command.Target = Input.Target;
				Command.FragmentType = TEXT("Test.Generated.HeatSource");
			}
			if (!Input.NumericValues.IsEmpty() && Input.NumericValues[0].Value > 0.0
				&& Input.NumericValues[0].Value < 2.0)
			{
				const int64 WakeDelayMilliseconds = bUseTargetSlotAsWakeDelay
					? static_cast<int64>(Input.Target.Slot) : 100;
				OutOutput.NextWakeTimeMilliseconds = Input.WorldTimeMilliseconds + WakeDelayMilliseconds;
			}
			return true;
		}

	private:
		FElementProcessorDescriptor Descriptor;
		bool bUseTargetSlotAsWakeDelay = false;
	};

	FElementCompoundShape MakeSphereShape(const FVector& Location, const double Radius)
	{
		FElementCompoundShape Shape;
		Shape.WorldTransform = FTransform(Location);
		Shape.Shapes.Add(FElementShape::MakeSphere(FVector::ZeroVector, Radius));
		return Shape;
	}

	FElementTargetKey MakeCharacterTarget(const int32 Slot, const uint32 Generation = 1)
	{
		FElementTargetKey Target;
		Target.Domain = EElementTargetDomain::Character;
		Target.RegistryId = 77;
		Target.Slot = Slot;
		Target.Generation = Generation;
		return Target;
	}

	FElementTargetSnapshot MakeTargetSnapshot(
		const FElementTargetKey Target,
		const FVector& Location,
		const uint64 Revision,
		const int64 Time = 0)
	{
		FElementTargetSnapshot Snapshot;
		Snapshot.Target = Target;
		Snapshot.Revision = Revision;
		Snapshot.EffectiveTimeMilliseconds = Time;
		Snapshot.Shape = MakeSphereShape(Location, 10.0);
		return Snapshot;
	}

	FElementMotionSubmission MakeMotion(
		const FElementTargetKey Target,
		const uint64 Revision,
		const FVector& From,
		const FVector& To,
		const int64 Start,
		const int64 End,
		const EElementQueryPriority Priority = EElementQueryPriority::Normal)
	{
		FElementMotionSubmission Motion;
		Motion.Target = Target;
		Motion.ExpectedTargetRevision = Revision;
		Motion.Priority = Priority;
		FElementMotionSegment& Segment = Motion.Segments.AddDefaulted_GetRef();
		Segment.PreviousTransform = FTransform(From);
		Segment.CurrentTransform = FTransform(To);
		Segment.StartTimeMilliseconds = Start;
		Segment.EndTimeMilliseconds = End;
		return Motion;
	}

	FTestHeatSourceFragment MakeSource(const FVector& Location, const double Radius, const double Power)
	{
		FTestHeatSourceFragment Fragment;
		Fragment.Shape = MakeSphereShape(Location, Radius);
		Fragment.Revision = 1;
		Fragment.Power = Power;
		return Fragment;
	}

	FTestCoolingSourceFragment MakeCoolingSource(
		const FVector& Location,
		const double Radius,
		const double CoolingPower)
	{
		FTestCoolingSourceFragment Fragment;
		Fragment.Shape = MakeSphereShape(Location, Radius);
		Fragment.Revision = 1;
		Fragment.CoolingPower = CoolingPower;
		return Fragment;
	}

	void RegisterProcessors(FElementAuthorityExecution& Runtime)
	{
		check(Runtime.RegisterNumericProcessor(MakeUnique<FTestHeatNumericProcessor>()));
		check(Runtime.RegisterStateProcessor(MakeUnique<FTestHeatStateProcessor>()));
		check(Runtime.ValidateProcessorRegistry());
	}

	void RegisterHeatAndCoolingProcessors(FElementAuthorityExecution& Runtime)
	{
		check(Runtime.RegisterNumericProcessor(MakeUnique<FTestHeatNumericProcessor>()));
		check(Runtime.RegisterNumericProcessor(MakeUnique<FTestCoolingNumericProcessor>()));
		check(Runtime.RegisterStateProcessor(MakeUnique<FTestHeatStateProcessor>()));
		check(Runtime.ValidateProcessorRegistry());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementRegistryDirtyPageTest,
	"ElementSandbox.ElementRuntime.Registry.DirtyQueueCurrentNextAndDeduplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementRegistryDirtyPageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementEntityRegistry Registry;
	const FElementEntityHandle Entity = Registry.CreateEntity();
	TestTrue(TEXT("添加 Influence Fragment"), Registry.AddFragment(Entity, MakeSource(FVector::ZeroVector, 100.0, 2.0)));
	TestTrue(TEXT("同页内连续修改"), Registry.EditFragment<FTestHeatSourceFragment>(Entity,
		[](FTestHeatSourceFragment& Source){ Source.Power = 3.0; }));
	FElementDirtyPage Current;
	TestTrue(TEXT("封闭 Current 页"), Registry.SealDirtyPage(Current));
	TestEqual(TEXT("同一实体重复修改只入队一次"), Current.Entities.Num(), 1);
	TestTrue(TEXT("Current 处理期间的新写入进入 Next"), Registry.EditFragment<FTestHeatSourceFragment>(Entity,
		[](FTestHeatSourceFragment& Source){ Source.Power = 4.0; }));
	FElementDirtyPage Next;
	TestTrue(TEXT("Next 可独立封箱"), Registry.SealDirtyPage(Next));
	TestEqual(TEXT("Next 仍只有同一实体一次"), Next.Entities.Num(), 1);
	TestTrue(TEXT("两个页面 Epoch 不同"), Current.Epoch != Next.Epoch);
	TestTrue(TEXT("去重计数记录真实工作"), Registry.GetStats().DirtyDeduplicatedCount >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementCompoundShapeAndSweepTest,
	"ElementSandbox.ElementRuntime.Query.CompoundShapeDedupAndHighSpeedSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementCompoundShapeAndSweepTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementCompoundShape Water;
	Water.Shapes.Add(FElementShape::MakeBox(FVector(-50.0, 0.0, 0.0), FQuat::Identity, FVector(60.0, 40.0, 20.0)));
	Water.Shapes.Add(FElementShape::MakeBox(FVector(50.0, 0.0, 0.0), FQuat::Identity, FVector(60.0, 40.0, 20.0)));
	const FElementCompoundShape Target = MakeSphereShape(FVector::ZeroVector, 5.0);
	TestTrue(TEXT("多个简单 Shape 可以组成一个逻辑水域"), FElementShapeKernels::Intersects(Water, Target));

	FElementMotionSegment Segment;
	Segment.PreviousTransform = FTransform(FVector(-300.0, 0.0, 0.0));
	Segment.CurrentTransform = FTransform(FVector(300.0, 0.0, 0.0));
	Segment.StartTimeMilliseconds = 0;
	Segment.EndTimeMilliseconds = 1000;
	FElementSweptShapeResult Result;
	TestTrue(TEXT("只看端点会漏掉的高速穿过由扫掠捕获"),
		FElementShapeKernels::Sweep(Water, MakeSphereShape(FVector::ZeroVector, 5.0), Segment,
			EElementSpatialWeightMode::Uniform, Result));
	TestTrue(TEXT("复合水域只产生一份累计接触"), Result.ContactDurationSeconds > 0.0);
	TestTrue(TEXT("Uniform 最大权重为一"), FMath::IsNearlyEqual(Result.MaximumWeight, 1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementLinearFalloffTargetVolumeTest,
	"ElementSandbox.ElementRuntime.Query.LinearFalloffUsesTargetShapeVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementLinearFalloffTargetVolumeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementCompoundShape LargeBoxTarget;
	LargeBoxTarget.WorldTransform = FTransform(FVector(150.0, 0.0, 0.0));
	LargeBoxTarget.Shapes.Add(FElementShape::MakeBox(
		FVector::ZeroVector, FQuat::Identity, FVector(60.0, 10.0, 10.0)));
	const double SphereWeight = FElementShapeKernels::CalculateWeight(
		MakeSphereShape(FVector::ZeroVector, 100.0), LargeBoxTarget,
		EElementSpatialWeightMode::LinearFalloff);
	TestTrue(TEXT("Sphere 衰减取 Target 实际表面而非体积中心"),
		FMath::IsNearlyEqual(SphereWeight, 0.10, 0.001));

	FElementCompoundShape CapsuleInfluence;
	CapsuleInfluence.Shapes.Add(FElementShape::MakeCapsule(
		FVector::ZeroVector, FVector::UpVector, 100.0, 50.0));
	const double CapsuleWeight = FElementShapeKernels::CalculateWeight(
		CapsuleInfluence, MakeSphereShape(FVector(150.0, 0.0, 40.0), 60.0),
		EElementSpatialWeightMode::LinearFalloff);
	TestTrue(TEXT("Capsule 衰减使用轴线到 Target Shape 的距离"),
		FMath::IsNearlyEqual(CapsuleWeight, 0.10, 0.001));

	FElementCompoundShape BoxInfluence;
	BoxInfluence.Shapes.Add(FElementShape::MakeBox(
		FVector::ZeroVector, FQuat::Identity, FVector(100.0)));
	const double BoxWeight = FElementShapeKernels::CalculateWeight(
		BoxInfluence, MakeSphereShape(FVector(150.0, 0.0, 0.0), 60.0),
		EElementSpatialWeightMode::LinearFalloff);
	TestTrue(TEXT("OBB 衰减使用与 Target Shape 相交的等权重面"),
		FMath::IsNearlyEqual(BoxWeight, 0.10, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementSurfaceDistanceFalloffSourceExtentTest,
	"ElementSandbox.ElementRuntime.Query.SurfaceDistanceFalloffIgnoresSourceExtent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementSurfaceDistanceFalloffSourceExtentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FElementCompoundShape SmallSource;
	SmallSource.Shapes.Add(FElementShape::MakeBox(
		FVector::ZeroVector, FQuat::Identity, FVector(50.0, 20.0, 20.0)));
	FElementCompoundShape SmallTarget;
	SmallTarget.WorldTransform = FTransform(FVector(80.0, 0.0, 0.0));
	SmallTarget.Shapes.Add(FElementShape::MakeBox(
		FVector::ZeroVector, FQuat::Identity, FVector(10.0, 20.0, 20.0)));

	FElementCompoundShape LongSource;
	LongSource.Shapes.Add(FElementShape::MakeBox(
		FVector::ZeroVector, FQuat::Identity, FVector(1100.0, 20.0, 20.0)));
	FElementCompoundShape LongTarget = SmallTarget;
	LongTarget.WorldTransform = FTransform(FVector(1130.0, 0.0, 0.0));

	const double SmallDistance = FElementShapeKernels::CalculateSurfaceDistance(SmallSource, SmallTarget);
	const double LongDistance = FElementShapeKernels::CalculateSurfaceDistance(LongSource, LongTarget);
	TestTrue(TEXT("短件与目标表面间距为 20cm"), FMath::IsNearlyEqual(SmallDistance, 20.0, 0.01));
	TestTrue(TEXT("22m 长件端部与目标表面间距仍为 20cm"),
		FMath::IsNearlyEqual(LongDistance, 20.0, 0.01));

	const double ExpectedWeight = 0.896;
	const double SmallWeight = FElementShapeKernels::CalculateSurfaceDistanceWeight(
		SmallSource, SmallTarget, 100.0);
	const double LongWeight = FElementShapeKernels::CalculateSurfaceDistanceWeight(
		LongSource, LongTarget, 100.0);
	TestTrue(TEXT("表面距离使用固定 SmoothStep 衰减"),
		FMath::IsNearlyEqual(SmallWeight, ExpectedWeight, 0.001));
	TestTrue(TEXT("相同表面间距的传导权重不受 Source 长度影响"),
		FMath::IsNearlyEqual(LongWeight, SmallWeight, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementAuthorityExecutionPipelineTest,
	"ElementSandbox.ElementRuntime.Authority.NumericReduceStateWakeAndIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementAuthorityExecutionPipelineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	RegisterProcessors(Runtime);
	const FElementEntityHandle SourceA = Runtime.CreateElement();
	const FElementEntityHandle SourceB = Runtime.CreateElement();
	TestTrue(TEXT("注册第一份 Heat Source"), Runtime.AddFragment(SourceA, MakeSource(FVector::ZeroVector, 100.0, 1.0)));
	TestTrue(TEXT("注册第二份 Heat Source"), Runtime.AddFragment(SourceB, MakeSource(FVector::ZeroVector, 100.0, 2.0)));
	const FElementTargetKey Target = MakeCharacterTarget(1);
	TestTrue(TEXT("角色以连续 POD Snapshot 登记"), Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector::ZeroVector, 1)));
	TestTrue(TEXT("Authority 边界封箱并执行"), Runtime.PumpWorkers(1000, true));
	TestTrue(TEXT("Barrier 发布"), Runtime.CommitAuthorityBarrier(1000));
	double Heat = 0.0;
	TestTrue(TEXT("读取归并后的 Heat"), Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat));
	TestTrue(TEXT("两份 Offset 在状态判断前完成归并"), Heat > 2.9 && Heat < 3.1);
	FElementStateValue State;
	TestTrue(TEXT("唯一 State Processor 已执行"), Runtime.TryGetStateValue(Target, TestStateChannel, State));
	TestEqual(TEXT("最终 Heat 只触发一次 Heated 状态"), State.Payload.Values[0], 1.0);
	const FElementAuthorityExecutionStats BeforeIdle = Runtime.GetStats();
	TestFalse(TEXT("稳定世界的下一个 Pump 是空操作"), Runtime.PumpWorkers(1125, true));
	const FElementAuthorityExecutionStats AfterIdle = Runtime.GetStats();
	TestEqual(TEXT("空闲不运行 Numeric Processor"),
		AfterIdle.NumericProcessorInvocationCount, BeforeIdle.NumericProcessorInvocationCount);
	TestEqual(TEXT("空闲不运行 Narrowphase"), AfterIdle.NarrowPhaseCount, BeforeIdle.NarrowPhaseCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementAuthorityCharacterBatchTest,
	"ElementSandbox.ElementRuntime.Authority.OnlyCharacterSingleArrayPassPerSourceBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementAuthorityCharacterBatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	RegisterProcessors(Runtime);
	for (int32 SourceIndex = 0; SourceIndex < 32; ++SourceIndex)
	{
		const FElementEntityHandle Source = Runtime.CreateElement();
		Runtime.AddFragment(Source, MakeSource(FVector(SourceIndex * 20.0, 0.0, 0.0), 30.0, 1.0));
	}
	constexpr int32 CharacterCount = 128;
	for (int32 CharacterIndex = 0; CharacterIndex < CharacterCount; ++CharacterIndex)
	{
		Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(
			MakeCharacterTarget(CharacterIndex + 1), FVector(CharacterIndex * 20.0, 0.0, 0.0), 1));
	}
	Runtime.PumpWorkers(1000, true);
	TestEqual(TEXT("整批 Source 变化只遍历一次 Character 连续数组"),
		Runtime.GetStats().CharacterSnapshotVisits, static_cast<uint64>(CharacterCount));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementRegistryPersistentIdentityTest,
	"ElementSandbox.ElementRuntime.Registry.PersistentIdentityAndRuntimeGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementRegistryPersistentIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FElementEntityRegistry Registry;
	const FWorldEntityId PersistentId(7001);
	const FElementEntityHandle First = Registry.CreateEntity(PersistentId);
	TestTrue(TEXT("首次持久身份登记成功"), First.IsSet());
	TestFalse(TEXT("同一运行期拒绝重复持久身份"), Registry.CreateEntity(PersistentId).IsSet());
	TestTrue(TEXT("持久身份可 O(1) 反查"), Registry.FindByPersistentId(PersistentId) == First);
	TestTrue(TEXT("RuntimeEvict 可销毁运行投影"), Registry.DestroyEntity(First));
	const FElementEntityHandle Restored = Registry.CreateEntity(PersistentId);
	TestTrue(TEXT("恢复同一持久身份得到新的运行代次"), Restored.IsSet() && Restored != First);
	TestFalse(TEXT("旧 Handle 在恢复后保持失效"), Registry.IsAlive(First));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementSourceLifecycleLocalRefreshTest,
	"ElementSandbox.ElementRuntime.Authority.SourceAddMoveRemoveAndDestroyRefreshLocalTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementSourceLifecycleLocalRefreshTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	RegisterProcessors(Runtime);
	const FElementTargetKey Target = MakeCharacterTarget(10);
	TestTrue(TEXT("登记静态目标"), Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector::ZeroVector, 1)));
	const FElementEntityHandle Source = Runtime.CreateElement();
	TestTrue(TEXT("新增 Source"), Runtime.AddFragment(Source, MakeSource(FVector::ZeroVector, 100.0, 1.0)));
	TestTrue(TEXT("新增 Source 只重查局部目标"), Runtime.PumpWorkers(1000, true));
	TestTrue(TEXT("提交新增 Source 结果"), Runtime.CommitAuthorityBarrier(1000));
	double Heat = 0.0;
	TestTrue(TEXT("Source 命中目标"), Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat));
	TestTrue(TEXT("初始影响为正"), Heat > 0.9);

	TestTrue(TEXT("移动 Source 经过 EditFragment 自动入队"),
		Runtime.EditFragment<FTestHeatSourceFragment>(Source, [](FTestHeatSourceFragment& Fragment)
		{
			Fragment.Shape = MakeSphereShape(FVector(1000.0, 0.0, 0.0), 100.0);
		}));
	TestTrue(TEXT("移动 Source 触发旧范围重查"), Runtime.PumpWorkers(1125, true));
	Runtime.CommitAuthorityBarrier(1125);
	Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat);
	TestTrue(TEXT("离开后的聚合值被重算为零"), FMath::IsNearlyZero(Heat));

	TestTrue(TEXT("移回 Source"),
		Runtime.EditFragment<FTestHeatSourceFragment>(Source, [](FTestHeatSourceFragment& Fragment)
		{
			Fragment.Shape = MakeSphereShape(FVector::ZeroVector, 100.0);
		}));
	Runtime.PumpWorkers(1250, true);
	Runtime.CommitAuthorityBarrier(1250);
	Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat);
	TestTrue(TEXT("移回后重新产生影响"), Heat > 0.9);

	TestTrue(TEXT("移除 Influence Fragment"), Runtime.RemoveFragment<FTestHeatSourceFragment>(Source));
	Runtime.PumpWorkers(1375, true);
	Runtime.CommitAuthorityBarrier(1375);
	Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat);
	TestTrue(TEXT("Fragment 移除后目标归零"), FMath::IsNearlyZero(Heat));
	TestTrue(TEXT("重新添加 Influence Fragment"), Runtime.AddFragment(Source, MakeSource(FVector::ZeroVector, 100.0, 1.0)));
	Runtime.PumpWorkers(1500, true);
	Runtime.CommitAuthorityBarrier(1500);
	TestTrue(TEXT("销毁 Element"), Runtime.DestroyElement(Source));
	Runtime.PumpWorkers(1625, true);
	Runtime.CommitAuthorityBarrier(1625);
	Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat);
	TestTrue(TEXT("Element 销毁同样重查旧范围"), FMath::IsNearlyZero(Heat));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementMotionAndSnapshotReplacementTest,
	"ElementSandbox.ElementRuntime.Query.SegmentedSweepAndNonContinuousReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementMotionAndSnapshotReplacementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	RegisterProcessors(Runtime);
	const FElementEntityHandle Source = Runtime.CreateElement();
	Runtime.AddFragment(Source, MakeSource(FVector::ZeroVector, 80.0, 1.0));
	const FElementTargetKey Target = MakeCharacterTarget(20);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector(-300.0, 0.0, 0.0), 1, 0));
	Runtime.PumpWorkers(1, true);
	Runtime.CommitAuthorityBarrier(1);

	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector(300.0, 0.0, 0.0), 2, 1000));
	FElementMotionSubmission Motion = MakeMotion(
		Target, 2, FVector(-300.0, 0.0, 0.0), FVector(-50.0, 40.0, 0.0),
		1, 500, EElementQueryPriority::Critical);
		FElementMotionSegment& Second = Motion.Segments.AddDefaulted_GetRef();
		Second.PreviousTransform = FTransform(FVector(-50.0, 40.0, 0.0));
		Second.CurrentTransform = FTransform(FVector(300.0, 0.0, 0.0));
		Second.StartTimeMilliseconds = 500;
		Second.EndTimeMilliseconds = 1000;
	TestTrue(TEXT("折线路径提交有效"), Motion.IsValid() && Runtime.SubmitMotion(Motion));
	TestTrue(TEXT("Critical 不等待 Authority 边界即可封箱"), Runtime.PumpWorkers(1000, false));
	Runtime.CommitAuthorityBarrier(1000);
	double Heat = 0.0;
	Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat);
	TestTrue(TEXT("多段原始路径在查询层压缩后仍捕获穿过"), Heat > 0.0);

	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector(-300.0, 0.0, 0.0), 3, 1125));
	Runtime.PumpWorkers(1125, true);
	Runtime.CommitAuthorityBarrier(1125);
	Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat);
	TestTrue(TEXT("非连续替换只检查新位置，不检查中间路径"), FMath::IsNearlyZero(Heat));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementSharedQueryOffsetAndNextBarrierTest,
	"ElementSandbox.ElementRuntime.Authority.SharedFireWaterOffsetsAndNoRecursiveBarrier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementSharedQueryOffsetAndNextBarrierTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	RegisterHeatAndCoolingProcessors(Runtime);
	const FElementEntityHandle HeatSource = Runtime.CreateElement();
	const FElementEntityHandle WaterSource = Runtime.CreateElement();
	Runtime.AddFragment(HeatSource, MakeSource(FVector::ZeroVector, 100.0, 3.0));
	Runtime.AddFragment(WaterSource, MakeCoolingSource(FVector::ZeroVector, 100.0, 1.0));
	const FElementTargetKey Target = MakeCharacterTarget(30);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector::ZeroVector, 1));
	Runtime.PumpWorkers(1000, true);
	Runtime.CommitAuthorityBarrier(1000);
	double Heat = 0.0;
	Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat);
	TestTrue(TEXT("Fire 与模拟 Water 共用查询批次后归并 Offset"), FMath::IsNearlyEqual(Heat, 2.0));
	TestEqual(TEXT("两个 Numeric Processor 均以独立 Worker 输入执行"),
		Runtime.GetStats().NumericProcessorInvocationCount, 2ull);
	FElementStateValue State;
	Runtime.TryGetStateValue(Target, TestStateChannel, State);
	TestEqual(TEXT("归并完成后唯一执行状态判断"), State.Payload.Values[0], 1.0);
	TArray<FElementStructuralCommand> Commands;
	Runtime.ConsumeStructuralCommands(Commands);
	TestEqual(TEXT("状态阶段只输出一个结构命令"), Commands.Num(), 1);

	const int32 BeforeInfluenceCount = Runtime.GetStats().InfluenceCount;
	const FElementEntityHandle Generated = Runtime.CreateElement();
	Runtime.AddFragment(Generated, MakeSource(FVector::ZeroVector, 50.0, 1.0));
	TestEqual(TEXT("Barrier 后产生的新 Source 尚未递归进入已提交批次"),
		Runtime.GetStats().InfluenceCount, BeforeInfluenceCount);
	Runtime.PumpWorkers(1125, true);
	TestEqual(TEXT("新 Source 只在下一次 Worker Pump 被捕获"),
		Runtime.GetStats().InfluenceCount, BeforeInfluenceCount + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementWakeReplacementCancellationAndStaleTest,
	"ElementSandbox.ElementRuntime.Authority.WakeReplaceCancelAndStaleGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementWakeReplacementCancellationAndStaleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	RegisterProcessors(Runtime);
	const FElementEntityHandle Source = Runtime.CreateElement();
	Runtime.AddFragment(Source, MakeSource(FVector::ZeroVector, 100.0, 1.0));
	const FElementTargetKey Target = MakeCharacterTarget(40);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector::ZeroVector, 1));
	Runtime.PumpWorkers(1000, true);
	Runtime.CommitAuthorityBarrier(1000);
	FElementAuthorityTargetStateSnapshot Captured;
	TestTrue(TEXT("可捕获包含绝对唤醒时间的稳定状态"), Runtime.CaptureTargetState(Target, Captured));
	TestEqual(TEXT("首次只登记一个最近唤醒"), Captured.Wakes.Num(), 1);
	if (!Captured.Wakes.IsEmpty()) TestEqual(TEXT("首次唤醒绝对时间"), Captured.Wakes[0].DueTimeMilliseconds, 1100ll);

	TestTrue(TEXT("替换已有目标的几何快照"),
		Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector::ZeroVector, 2, 1050)));
	Runtime.CaptureTargetState(Target, Captured);
	TestEqual(TEXT("快照替换不会越过 Authority Barrier 提前推进稳定结算时间"),
		Captured.LastSettlementMilliseconds, 1000ll);
	Runtime.PumpWorkers(1050, true);
	Runtime.CommitAuthorityBarrier(1050);
	Runtime.CaptureTargetState(Target, Captured);
	TestEqual(TEXT("Authority Barrier 才提交新的稳定结算时间"),
		Captured.LastSettlementMilliseconds, 1050ll);
	TestEqual(TEXT("输入变化替换旧唤醒而不叠加"), Captured.Wakes.Num(), 1);
	if (!Captured.Wakes.IsEmpty()) TestEqual(TEXT("替换后的绝对时间"), Captured.Wakes[0].DueTimeMilliseconds, 1150ll);
	TestTrue(TEXT("记录唤醒替换"), Runtime.GetStats().WakeReplacedCount >= 1);

	Runtime.EditFragment<FTestHeatSourceFragment>(Source,
		[](FTestHeatSourceFragment& Fragment){ Fragment.Power = 0.0; });
	Runtime.PumpWorkers(1060, true);
	Runtime.CommitAuthorityBarrier(1060);
	Runtime.CaptureTargetState(Target, Captured);
	TestTrue(TEXT("不再需要未来变化时取消唤醒"), Captured.Wakes.IsEmpty());
	TestTrue(TEXT("记录唤醒取消"), Runtime.GetStats().WakeCancelledCount >= 1);

	TestTrue(TEXT("旧 Revision 的移除请求被拒绝"),
		!Runtime.RemoveTargetSnapshot(Target, 1, EElementTargetRemovalReason::RuntimeEvict));
	TestEqual(TEXT("拒绝旧请求后目标仍存在"), Runtime.GetStats().TargetCount, 1);
	TestTrue(TEXT("正确 Revision 可移除目标"),
		Runtime.RemoveTargetSnapshot(Target, 2, EElementTargetRemovalReason::RuntimeEvict));
	const FElementTargetKey NewGeneration = MakeCharacterTarget(40, 2);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(NewGeneration, FVector::ZeroVector, 1, 1200));
	const uint64 DropsBefore = Runtime.GetStats().StaleResultDropCount;
	TestTrue(TEXT("生产线程只追加旧 Generation 的迟到运动"),
		Runtime.SubmitMotion(MakeMotion(Target, 2, FVector::ZeroVector, FVector(10.0, 0.0, 0.0), 1100, 1200)));
	Runtime.PumpWorkers(1200, true);
	TestTrue(TEXT("封箱后以 Generation 校验丢弃迟到运动"),
		Runtime.GetStats().StaleResultDropCount > DropsBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementEarlierWakeInsertedAfterLaterWakeTest,
	"ElementSandbox.ElementRuntime.Authority.EarlierWakeInsertedAfterLaterWake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementEarlierWakeInsertedAfterLaterWakeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	TestTrue(TEXT("注册测试 Numeric Processor"),
		Runtime.RegisterNumericProcessor(MakeUnique<FTestHeatNumericProcessor>()));
	TestTrue(TEXT("注册按 Target Slot 生成唤醒延迟的测试 State Processor"),
		Runtime.RegisterStateProcessor(MakeUnique<FTestHeatStateProcessor>(true)));
	TestTrue(TEXT("测试 Processor Registry 合法"), Runtime.ValidateProcessorRegistry());

	const FElementEntityHandle Source = Runtime.CreateElement();
	Runtime.AddFragment(Source, MakeSource(FVector::ZeroVector, 100.0, 1.0));
	const FElementTargetKey LaterTarget = MakeCharacterTarget(500);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(LaterTarget, FVector::ZeroVector, 1));
	Runtime.PumpWorkers(1000, true);
	Runtime.CommitAuthorityBarrier(1000);

	const FElementTargetKey EarlierTarget = MakeCharacterTarget(100);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(EarlierTarget, FVector::ZeroVector, 1, 1100));
	Runtime.PumpWorkers(1100, true);
	Runtime.CommitAuthorityBarrier(1100);

	FElementAuthorityTargetStateSnapshot LaterState;
	FElementAuthorityTargetStateSnapshot EarlierState;
	TestTrue(TEXT("捕获较晚唤醒目标"), Runtime.CaptureTargetState(LaterTarget, LaterState));
	TestTrue(TEXT("捕获较早唤醒目标"), Runtime.CaptureTargetState(EarlierTarget, EarlierState));
	TestEqual(TEXT("较晚目标只有一个有效唤醒"), LaterState.Wakes.Num(), 1);
	TestEqual(TEXT("较早目标只有一个有效唤醒"), EarlierState.Wakes.Num(), 1);
	if (LaterState.Wakes.Num() == 1)
	{
		TestEqual(TEXT("较晚唤醒先入堆"), LaterState.Wakes[0].DueTimeMilliseconds, 1500ll);
	}
	if (EarlierState.Wakes.Num() == 1)
	{
		TestEqual(TEXT("较早唤醒后入堆"), EarlierState.Wakes[0].DueTimeMilliseconds, 1200ll);
	}

	Runtime.PumpWorkers(1200, true);
	Runtime.CommitAuthorityBarrier(1200);
	Runtime.CaptureTargetState(LaterTarget, LaterState);
	Runtime.CaptureTargetState(EarlierTarget, EarlierState);
	TestEqual(TEXT("较晚目标保持单一状态通道"), LaterState.StateValues.Num(), 1);
	TestEqual(TEXT("较早目标保持单一状态通道"), EarlierState.StateValues.Num(), 1);
	if (LaterState.StateValues.Num() == 1)
	{
		TestEqual(TEXT("未到期目标仍保留原状态"), LaterState.StateValues[0].Revision, 1ull);
	}
	if (EarlierState.StateValues.Num() == 1)
	{
		TestEqual(TEXT("后插入的较早唤醒按时结算"), EarlierState.StateValues[0].Revision, 2ull);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementStaleSourceRevisionBarrierTest,
	"ElementSandbox.ElementRuntime.Authority.StaleSourceRevisionDroppedAtBarrier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementStaleSourceRevisionBarrierTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	RegisterProcessors(Runtime);
	const FElementEntityHandle Source = Runtime.CreateElement();
	Runtime.AddFragment(Source, MakeSource(FVector::ZeroVector, 100.0, 1.0));
	const FElementTargetKey Target = MakeCharacterTarget(50);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector::ZeroVector, 1));
	Runtime.PumpWorkers(1000, true);
	Runtime.EditFragment<FTestHeatSourceFragment>(Source,
		[](FTestHeatSourceFragment& Fragment){ Fragment.Power = 2.0; });
	Runtime.CommitAuthorityBarrier(1000);
	double Heat = 0.0;
	TestFalse(TEXT("Source Revision 改变后旧结果不发布"),
		Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat));
	TestTrue(TEXT("Barrier 记录 stale 丢弃"), Runtime.GetStats().StaleResultDropCount >= 1);
	Runtime.PumpWorkers(1125, true);
	Runtime.CommitAuthorityBarrier(1125);
	TestTrue(TEXT("下一批使用新 Revision"), Runtime.TryGetNumericValue(Target, TestNumericChannel, Heat));
	TestTrue(TEXT("新值来自修改后的 Source"), FMath::IsNearlyEqual(Heat, 2.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementConcurrentBoundedSubmissionTest,
	"ElementSandbox.ElementRuntime.Scheduler.ConcurrentAppendBoundedBackgroundAndPromotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementConcurrentBoundedSubmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecutionConfig Config;
	Config.OpenPageCapacity = 2;
	Config.BackgroundWorkBudget = 1;
	Config.BackgroundPromotionCycles = 2;
	Config.MaximumPendingMotionCount = 32;
	FElementAuthorityExecution Runtime(Config);
	RegisterProcessors(Runtime);
	const FElementTargetKey Target = MakeCharacterTarget(60);
	Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector::ZeroVector, 1));
	ParallelFor(32, [&Runtime, Target](const int32 Index)
	{
		Runtime.SubmitMotion(MakeMotion(
			Target, 1, FVector::ZeroVector, FVector(Index + 1.0, 0.0, 0.0),
			Index * 10 + 1, Index * 10 + 10, EElementQueryPriority::Background));
	});
	TestEqual(TEXT("多生产线程可安全填满有界开放页"), Runtime.GetStats().PendingBackgroundCount, 32);
	TestFalse(TEXT("超过硬上限的 Background 工作收到反压"), Runtime.SubmitMotion(MakeMotion(
		Target, 1, FVector::ZeroVector, FVector(100.0, 0.0, 0.0), 1000, 1010,
		EElementQueryPriority::Background)));
	TestEqual(TEXT("反压不会扩张队列"), Runtime.GetStats().PendingBackgroundCount, 32);
	TestEqual(TEXT("记录被拒绝的提交"), Runtime.GetStats().MotionSubmissionRejectedCount, 1ull);

	Runtime.PumpWorkers(1000, true);
	Runtime.CommitAuthorityBarrier(1000);
	TestEqual(TEXT("第一 Cycle 只消费 Background 预算"), Runtime.GetStats().PendingBackgroundCount, 31);
	Runtime.PumpWorkers(1125, true);
	Runtime.CommitAuthorityBarrier(1125);
	TestEqual(TEXT("等待八周期规则的测试配置到期后提升并清空"),
		Runtime.GetStats().PendingBackgroundCount + Runtime.GetStats().PendingNormalCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementTargetSnapshotBatchTest,
	"ElementSandbox.ElementRuntime.Authority.TargetSnapshotBatchDefersPublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementTargetSnapshotBatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Authority::Execution::Tests;
	FElementAuthorityExecution Runtime;
	TArray<FElementTargetKey> Targets;
	Targets.Reserve(128);
	Runtime.BeginTargetSnapshotBatch();
	for (int32 Index = 0; Index < 128; ++Index)
	{
		FElementTargetKey Target;
		Target.Domain = EElementTargetDomain::Building;
		Target.WorldEntityId = FWorldEntityId(static_cast<uint64>(Index + 1));
		Target.RegistryId = 91;
		Target.Slot = Index + 1;
		Target.Generation = 1;
		Targets.Add(Target);
		TestTrue(TEXT("批次内 Target Upsert 仍逐条校验"),
			Runtime.ReplaceTargetSnapshot(MakeTargetSnapshot(Target, FVector(Index * 25.0, 0.0, 0.0), 1)));
	}
	Runtime.EndTargetSnapshotBatch();
	TestEqual(TEXT("批次结束后全部 Target 可见"), Runtime.GetStats().TargetCount, Targets.Num());

	Runtime.BeginTargetSnapshotBatch();
	for (const FElementTargetKey Target : Targets)
	{
		TestTrue(TEXT("批次内 Target Remove 仍逐条校验"),
			Runtime.RemoveTargetSnapshot(Target, 1, EElementTargetRemovalReason::GameplayDestroy));
	}
	Runtime.EndTargetSnapshotBatch();
	TestEqual(TEXT("批次结束后全部 Target 已移除"), Runtime.GetStats().TargetCount, 0);
	return true;
}

#endif
