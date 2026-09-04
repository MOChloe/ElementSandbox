#if WITH_DEV_AUTOMATION_TESTS

#include "Fire/FireCombustionModel.h"
#include "Misc/AutomationTest.h"

namespace
{
	FFireCombustionProfile MakeStructureProfile()
	{
		FFireCombustionProfile Profile;
		Profile.IgnitionDose = 1.50;
		Profile.HeatDecayPerSecond = 0.20;
		Profile.BurnDurationSeconds = 18.0;
		Profile.EmittedFireIntensity = 0.80;
		Profile.EmissionRangeCentimeters = 100.0;
		return Profile;
	}

	FFireCombustionProfile MakeStickProfile()
	{
		FFireCombustionProfile Profile;
		Profile.IgnitionDose = 0.60;
		Profile.HeatDecayPerSecond = 0.15;
		Profile.BurnDurationSeconds = 30.0;
		Profile.EmittedFireIntensity = 0.65;
		Profile.EmissionRangeCentimeters = 80.0;
		return Profile;
	}

	FFireCombustionProfile MakeCharacterProfile()
	{
		FFireCombustionProfile Profile;
		Profile.IgnitionDose = 0.35;
		Profile.HeatDecayPerSecond = 0.15;
		Profile.BurnDurationSeconds = 6.0;
		Profile.EmittedFireIntensity = 0.35;
		Profile.EmissionRangeCentimeters = 80.0;
		Profile.EmissionPolicy = EFirePropagationPolicy::CharacterOnly;
		Profile.BurnCompletion = EFireBurnCompletion::ReturnToCold;
		Profile.BurningSupportIntensity = 0.20;
		Profile.BurningTailSeconds = 6.0;
		return Profile;
	}

	FFireIntervalInput Constant(const int64 Start, const int64 End, const double Intensity)
	{
		FFireIntervalInput Input;
		Input.StartTimeMilliseconds = Start;
		Input.EndTimeMilliseconds = End;
		if (End > Start)
		{
			Input.Segments.Add({Start, End, Intensity});
		}
		return Input;
	}

	const FFireCombustionEvent* FindEvent(
		const FFireIntervalResult& Result,
		const EFireCombustionEventType Type)
	{
		return Result.Events.FindByPredicate([Type](const FFireCombustionEvent& Event)
		{
			return Event.Type == Type;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFireGameplayThresholdTest,
	"ElementSandbox.Combustion.GameplayThresholds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFireGameplayThresholdTest::RunTest(const FString& Parameters)
{
	const auto AssertIgnition = [this](
		const TCHAR* Name,
		const FFireCombustionProfile& Profile,
		const double Intensity,
		const double ExpectedMilliseconds)
	{
		const FFireIntervalResult Result = FFireCombustionModel::AdvanceInterval(
			FFireCombustionState(), Profile, Constant(0, 10000, Intensity));
		const FFireCombustionEvent* Ignited = FindEvent(Result, EFireCombustionEventType::Ignited);
		TestTrue(Name, Result.bSucceeded && Ignited != nullptr);
		if (Ignited)
		{
			TestTrue(FString::Printf(TEXT("%s 使用解析阈值时刻"), Name),
				FMath::IsNearlyEqual(Ignited->EffectiveTimeMilliseconds, ExpectedMilliseconds, 0.001));
		}
	};
	AssertIgnition(TEXT("火堆点燃木棍"), MakeStickProfile(), 1.0, 600.0 / 0.85);
	AssertIgnition(TEXT("木棍点燃木结构"), MakeStructureProfile(), 0.65, 1500.0 / 0.45);
	AssertIgnition(TEXT("木结构点燃相邻木结构"), MakeStructureProfile(), 0.80, 2500.0);
	AssertIgnition(TEXT("火堆点燃 Character"), MakeCharacterProfile(), 1.0, 350.0 / 0.85);
	AssertIgnition(TEXT("木棍点燃 Character"), MakeCharacterProfile(), 0.65, 700.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFirePartitionInvariantTest,
	"ElementSandbox.Combustion.ArbitraryPartitionInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFirePartitionInvariantTest::RunTest(const FString& Parameters)
{
	const FFireCombustionProfile Profile = MakeStructureProfile();
	const FFireIntervalResult Whole = FFireCombustionModel::AdvanceInterval(
		FFireCombustionState(), Profile, Constant(0, 25000, 0.65));
	const int64 Cuts[] = {0, 7, 125, 999, 3001, 3333, 3334, 12007, 21333, 25000};
	FFireCombustionState State;
	double IgnitionTime = -1.0;
	int32 IgnitionCount = 0;
	for (int32 Index = 0; Index + 1 < UE_ARRAY_COUNT(Cuts); ++Index)
	{
		const FFireIntervalResult Part = FFireCombustionModel::AdvanceInterval(
			State, Profile, Constant(Cuts[Index], Cuts[Index + 1], 0.65));
		TestTrue(TEXT("所有任意分片推进成功"), Part.bSucceeded);
		if (!Part.bSucceeded)
		{
			return false;
		}
		State = Part.FinalState;
		if (const FFireCombustionEvent* Ignited = FindEvent(Part, EFireCombustionEventType::Ignited))
		{
			++IgnitionCount;
			IgnitionTime = Ignited->EffectiveTimeMilliseconds;
		}
	}
	TestEqual(TEXT("分片只点燃一次"), IgnitionCount, 1);
	TestEqual(TEXT("整段与分片最终 Phase 一致"), State.Phase, Whole.FinalState.Phase);
	TestTrue(TEXT("整段与分片起止时间一致"),
		FMath::IsNearlyEqual(State.BurnStartTimeMilliseconds, Whole.FinalState.BurnStartTimeMilliseconds, 0.001)
		&& FMath::IsNearlyEqual(State.BurnEndTimeMilliseconds, Whole.FinalState.BurnEndTimeMilliseconds, 0.001));
	TestTrue(TEXT("分片不改变点燃时刻"), FMath::IsNearlyEqual(IgnitionTime, 10000.0 / 3.0, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFireDecayAndBurnedOutTest,
	"ElementSandbox.Combustion.DecayAndBurnedOutAreStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFireDecayAndBurnedOutTest::RunTest(const FString& Parameters)
{
	const FFireCombustionProfile Profile = MakeStickProfile();
	const FFireIntervalResult Heated = FFireCombustionModel::AdvanceInterval(
		FFireCombustionState(), Profile, Constant(0, 500, 1.0));
	TestEqual(TEXT("未到阈值时 Heating"), Heated.FinalState.Phase, EFireCombustionPhase::Heating);
	const FFireIntervalResult Cooled = FFireCombustionModel::AdvanceInterval(
		Heated.FinalState, Profile, Constant(500, 10000, 0.0));
	TestEqual(TEXT("Dose 衰减到零后 Cold"), Cooled.FinalState.Phase, EFireCombustionPhase::Cold);
	TestTrue(TEXT("Cold Dose 清零"), FMath::IsNearlyZero(Cooled.FinalState.HeatDose));

	FFireCombustionState Spent;
	Spent.Phase = EFireCombustionPhase::BurnedOut;
	const FFireIntervalResult StillSpent = FFireCombustionModel::AdvanceInterval(
		Spent, Profile, Constant(0, int64(10) * 365 * 24 * 60 * 60 * 1000, 1000.0));
	TestTrue(TEXT("十年长区间一次解析成功"), StillSpent.bSucceeded);
	TestEqual(TEXT("BurnedOut 永久忽略热输入"), StillSpent.FinalState.Phase, EFireCombustionPhase::BurnedOut);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFireCharacterRefreshTest,
	"ElementSandbox.Combustion.CharacterRefreshAndTail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFireCharacterRefreshTest::RunTest(const FString& Parameters)
{
	const FFireCombustionProfile Profile = MakeCharacterProfile();
	const FFireIntervalResult Supported = FFireCombustionModel::AdvanceInterval(
		FFireCombustionState(), Profile, Constant(0, 20000, 1.0));
	TestEqual(TEXT("有效火源中持续 Burning"), Supported.FinalState.Phase, EFireCombustionPhase::Burning);
	TestTrue(TEXT("截止时间刷新为最后支持时刻加六秒"),
		FMath::IsNearlyEqual(Supported.FinalState.BurnEndTimeMilliseconds, 26000.0, 0.001));
	const FFireIntervalResult Tail = FFireCombustionModel::AdvanceInterval(
		Supported.FinalState, Profile, Constant(20000, 26000, 0.0));
	TestEqual(TEXT("离开六秒后回到 Cold"), Tail.FinalState.Phase, EFireCombustionPhase::Cold);
	const FFireCombustionEvent* Ended = FindEvent(Tail, EFireCombustionEventType::BurnEnded);
	TestTrue(TEXT("尾焰使用真实绝对截止时间"), Ended
		&& FMath::IsNearlyEqual(Ended->EffectiveTimeMilliseconds, 26000.0, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFireValidationAndAttenuationTest,
	"ElementSandbox.Combustion.ValidationAndAttenuation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFireValidationAndAttenuationTest::RunTest(const FString& Parameters)
{
	FFireCombustionProfile Invalid = MakeStructureProfile();
	Invalid.IgnitionDose = 0.0;
	TestFalse(TEXT("非法规则被严格拒绝"), FFireCombustionModel::ValidateProfile(Invalid));
	TestTrue(TEXT("表面接触保持满强度"),
		FMath::IsNearlyEqual(FFireCombustionModel::ComputeDistanceAttenuation(0.0, 100.0), 1.0));
	TestTrue(TEXT("半程 smoothstep 衰减为一半"),
		FMath::IsNearlyEqual(FFireCombustionModel::ComputeDistanceAttenuation(50.0, 100.0), 0.5));
	TestTrue(TEXT("距离等于 Range 时无贡献"),
		FMath::IsNearlyZero(FFireCombustionModel::ComputeDistanceAttenuation(100.0, 100.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFireCharacterLongIntervalTest,
	"ElementSandbox.Combustion.CharacterLongIntervalIsAnalytic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFireCharacterLongIntervalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FFireCombustionProfile Profile = MakeCharacterProfile();
	constexpr double WeakIntensity = 0.19;
	constexpr int64 TenYearsMilliseconds = int64(10) * 365 * 24 * 60 * 60 * 1000;
	const FFireIntervalResult Whole = FFireCombustionModel::AdvanceInterval(
		FFireCombustionState(), Profile,
		Constant(0, TenYearsMilliseconds, WeakIntensity));
	TestTrue(TEXT("十年弱输入由有限个解析段完成"),
		Whole.bSucceeded && Whole.AnalyticSegmentCount <= 8);

	const int64 Cut = TenYearsMilliseconds / 3;
	const FFireIntervalResult First = FFireCombustionModel::AdvanceInterval(
		FFireCombustionState(), Profile, Constant(0, Cut, WeakIntensity));
	const FFireIntervalResult Second = FFireCombustionModel::AdvanceInterval(
		First.FinalState, Profile, Constant(Cut, TenYearsMilliseconds, WeakIntensity));
	TestTrue(TEXT("长区间任意分片都成功"), First.bSucceeded && Second.bSucceeded);
	TestEqual(TEXT("长区间分片不改变最终 Phase"),
		Second.FinalState.Phase, Whole.FinalState.Phase);
	TestTrue(TEXT("长区间分片不改变 Dose 或绝对燃烧时刻"),
		FMath::IsNearlyEqual(Second.FinalState.HeatDose, Whole.FinalState.HeatDose, 0.001)
			&& FMath::IsNearlyEqual(Second.FinalState.BurnStartTimeMilliseconds,
				Whole.FinalState.BurnStartTimeMilliseconds, 0.001)
			&& FMath::IsNearlyEqual(Second.FinalState.BurnEndTimeMilliseconds,
				Whole.FinalState.BurnEndTimeMilliseconds, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFireFourPhaseContractTest,
	"ElementSandbox.Combustion.FourPhaseContractAndExactHold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFireFourPhaseContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FFireCombustionProfile Profile = MakeStructureProfile();
	FFireCombustionState State;
	TestTrue(TEXT("Cold 初态合法"), FFireCombustionModel::ValidateState(State, Profile));

	const FFireIntervalResult Heated = FFireCombustionModel::AdvanceInterval(
		State, Profile, Constant(0, 1000, 0.70));
	TestTrue(TEXT("Cold 进入 Heating"), Heated.bSucceeded
		&& Heated.FinalState.Phase == EFireCombustionPhase::Heating);
	const double HeldDose = Heated.FinalState.HeatDose;
	const FFireIntervalResult Held = FFireCombustionModel::AdvanceInterval(
		Heated.FinalState, Profile, Constant(1000, 100000, Profile.HeatDecayPerSecond));
	TestTrue(TEXT("输入等于衰减时 Dose 精确保持"), Held.bSucceeded
		&& Held.FinalState.Phase == EFireCombustionPhase::Heating
		&& FMath::IsNearlyEqual(Held.FinalState.HeatDose, HeldDose, 1.e-9));

	const FFireIntervalResult Ignited = FFireCombustionModel::AdvanceInterval(
		Held.FinalState, Profile, Constant(100000, 110000, 0.80));
	TestTrue(TEXT("Heating 进入 Burning"), Ignited.bSucceeded
		&& Ignited.FinalState.Phase == EFireCombustionPhase::Burning
		&& Ignited.FinalState.HeatDose == 0.0);
	const int64 Finish = FMath::CeilToInt64(Ignited.FinalState.BurnEndTimeMilliseconds);
	const FFireIntervalResult Spent = FFireCombustionModel::AdvanceInterval(
		Ignited.FinalState, Profile,
		Constant(110000, Finish, 1000.0));
	TestTrue(TEXT("Burning 到绝对截止时间进入 BurnedOut"), Spent.bSucceeded
		&& Spent.FinalState.Phase == EFireCombustionPhase::BurnedOut);

	FFireCombustionState InvalidCold;
	InvalidCold.HeatDose = 0.1;
	TestFalse(TEXT("Cold 携带 Dose 被拒绝"),
		FFireCombustionModel::ValidateState(InvalidCold, Profile));
	FFireCombustionState InvalidBurning;
	InvalidBurning.Phase = EFireCombustionPhase::Burning;
	InvalidBurning.BurnStartTimeMilliseconds = 10.0;
	InvalidBurning.BurnEndTimeMilliseconds = 10.0;
	TestFalse(TEXT("Burning 非法绝对时间被拒绝"),
		FFireCombustionModel::ValidateState(InvalidBurning, Profile));
	return true;
}

#endif
