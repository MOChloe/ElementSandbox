#include "Fire/FireCombustionModel.h"

namespace
{
	constexpr double DoseTolerance = 1.e-9;
	constexpr double TimeToleranceMilliseconds = 1.e-7;

	bool IsFiniteNonNegative(const double Value)
	{
		return FMath::IsFinite(Value) && Value >= 0.0;
	}

	uint32 NextRevision(const uint32 Revision)
	{
		return Revision == MAX_uint32 ? 1u : Revision + 1u;
	}

	void SetError(FString* OutError, const TCHAR* Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
	}

	void AddEvent(
		FFireIntervalResult& Result,
		const EFireCombustionEventType Type,
		const double TimeMilliseconds)
	{
		Result.Events.Add({Type, TimeMilliseconds});
	}

	void EnterCold(FFireCombustionState& State)
	{
		State.Phase = EFireCombustionPhase::Cold;
		State.HeatDose = 0.0;
		State.BurnStartTimeMilliseconds = 0.0;
		State.BurnEndTimeMilliseconds = 0.0;
	}

	void EnterBurning(
		FFireCombustionState& State,
		const FFireCombustionProfile& Profile,
		const double TimeMilliseconds)
	{
		State.Phase = EFireCombustionPhase::Burning;
		State.HeatDose = 0.0;
		State.BurnStartTimeMilliseconds = TimeMilliseconds;
		const double DurationSeconds = Profile.BurnCompletion == EFireBurnCompletion::ReturnToCold
			? Profile.BurningTailSeconds
			: Profile.BurnDurationSeconds;
		State.BurnEndTimeMilliseconds = TimeMilliseconds + DurationSeconds * 1000.0;
	}

	void EndBurning(
		FFireIntervalResult& Result,
		FFireCombustionState& State,
		const FFireCombustionProfile& Profile,
		const double TimeMilliseconds)
	{
		AddEvent(Result, EFireCombustionEventType::BurnEnded, TimeMilliseconds);
		if (Profile.BurnCompletion == EFireBurnCompletion::BurnedOut)
		{
			State.Phase = EFireCombustionPhase::BurnedOut;
			State.HeatDose = 0.0;
			State.BurnStartTimeMilliseconds = 0.0;
			State.BurnEndTimeMilliseconds = 0.0;
		}
		else
		{
			EnterCold(State);
		}
	}

	bool ValidateInput(const FFireIntervalInput& Input)
	{
		if (Input.StartTimeMilliseconds < 0
			|| Input.EndTimeMilliseconds < Input.StartTimeMilliseconds)
		{
			return false;
		}
		if (Input.StartTimeMilliseconds == Input.EndTimeMilliseconds)
		{
			return Input.Segments.IsEmpty();
		}
		if (Input.Segments.IsEmpty())
		{
			return false;
		}
		int64 ExpectedStart = Input.StartTimeMilliseconds;
		for (const FFireIntensitySegment& Segment : Input.Segments)
		{
			if (Segment.StartTimeMilliseconds != ExpectedStart
				|| Segment.EndTimeMilliseconds <= Segment.StartTimeMilliseconds
				|| Segment.EndTimeMilliseconds > Input.EndTimeMilliseconds
				|| !IsFiniteNonNegative(Segment.IncomingFireIntensity))
			{
				return false;
			}
			ExpectedStart = Segment.EndTimeMilliseconds;
		}
		return ExpectedStart == Input.EndTimeMilliseconds;
	}

	void AdvanceHeating(
		FFireIntervalResult& Result,
		FFireCombustionState& State,
		const FFireCombustionProfile& Profile,
		const double IncomingIntensity,
		double& CursorMilliseconds,
		const double EndMilliseconds)
	{
		const double RatePerSecond = IncomingIntensity - Profile.HeatDecayPerSecond;
		if (State.Phase == EFireCombustionPhase::Cold && RatePerSecond > 0.0)
		{
			State.Phase = EFireCombustionPhase::Heating;
			AddEvent(Result, EFireCombustionEventType::StartedHeating, CursorMilliseconds);
		}
		if (State.Phase == EFireCombustionPhase::Cold || FMath::IsNearlyZero(RatePerSecond, DoseTolerance))
		{
			CursorMilliseconds = EndMilliseconds;
			return;
		}

		if (RatePerSecond > 0.0)
		{
			const double MillisecondsToIgnition =
				(Profile.IgnitionDose - State.HeatDose) / RatePerSecond * 1000.0;
			if (CursorMilliseconds + MillisecondsToIgnition <= EndMilliseconds + TimeToleranceMilliseconds)
			{
				CursorMilliseconds = FMath::Min(EndMilliseconds, CursorMilliseconds + MillisecondsToIgnition);
				EnterBurning(State, Profile, CursorMilliseconds);
				AddEvent(Result, EFireCombustionEventType::Ignited, CursorMilliseconds);
				return;
			}
			State.HeatDose += RatePerSecond * (EndMilliseconds - CursorMilliseconds) / 1000.0;
			State.HeatDose = FMath::Clamp(State.HeatDose, DoseTolerance, Profile.IgnitionDose - DoseTolerance);
			CursorMilliseconds = EndMilliseconds;
			return;
		}

		const double MillisecondsToCold = State.HeatDose / -RatePerSecond * 1000.0;
		if (CursorMilliseconds + MillisecondsToCold <= EndMilliseconds + TimeToleranceMilliseconds)
		{
			CursorMilliseconds = FMath::Min(EndMilliseconds, CursorMilliseconds + MillisecondsToCold);
			EnterCold(State);
			AddEvent(Result, EFireCombustionEventType::Cooled, CursorMilliseconds);
			return;
		}
		State.HeatDose += RatePerSecond * (EndMilliseconds - CursorMilliseconds) / 1000.0;
		State.HeatDose = FMath::Clamp(State.HeatDose, DoseTolerance, Profile.IgnitionDose - DoseTolerance);
		CursorMilliseconds = EndMilliseconds;
	}

	void AdvanceBurning(
		FFireIntervalResult& Result,
		FFireCombustionState& State,
		const FFireCombustionProfile& Profile,
		const double IncomingIntensity,
		double& CursorMilliseconds,
		const double EndMilliseconds)
	{
		if (Profile.BurnCompletion == EFireBurnCompletion::ReturnToCold
			&& IncomingIntensity >= Profile.BurningSupportIntensity)
		{
			State.BurnEndTimeMilliseconds = FMath::Max(
				State.BurnEndTimeMilliseconds,
				EndMilliseconds + Profile.BurningTailSeconds * 1000.0);
			CursorMilliseconds = EndMilliseconds;
			return;
		}
		if (State.BurnEndTimeMilliseconds <= EndMilliseconds + TimeToleranceMilliseconds)
		{
			CursorMilliseconds = FMath::Max(CursorMilliseconds, State.BurnEndTimeMilliseconds);
			EndBurning(Result, State, Profile, CursorMilliseconds);
			return;
		}
		CursorMilliseconds = EndMilliseconds;
	}
}

bool FFireCombustionModel::ValidateProfile(
	const FFireCombustionProfile& Profile,
	FString* OutError)
{
	if (!FMath::IsFinite(Profile.IgnitionDose) || Profile.IgnitionDose <= 0.0)
	{
		SetError(OutError, TEXT("IgnitionDose 必须是有限正数。"));
		return false;
	}
	if (!IsFiniteNonNegative(Profile.HeatDecayPerSecond))
	{
		SetError(OutError, TEXT("HeatDecayPerSecond 必须是有限非负数。"));
		return false;
	}
	if (!FMath::IsFinite(Profile.EmittedFireIntensity) || Profile.EmittedFireIntensity <= 0.0
		|| !FMath::IsFinite(Profile.EmissionRangeCentimeters) || Profile.EmissionRangeCentimeters <= 0.0)
	{
		SetError(OutError, TEXT("燃烧输出强度和距离必须是有限正数。"));
		return false;
	}
	if (Profile.BurnCompletion == EFireBurnCompletion::BurnedOut)
	{
		if (!FMath::IsFinite(Profile.BurnDurationSeconds) || Profile.BurnDurationSeconds <= 0.0)
		{
			SetError(OutError, TEXT("BurnedOut Profile 必须提供正燃烧时长。"));
			return false;
		}
		if (Profile.BurningSupportIntensity != 0.0 || Profile.BurningTailSeconds != 0.0)
		{
			SetError(OutError, TEXT("固定燃料 Profile 不能配置刷新式尾焰。"));
			return false;
		}
	}
	else if (!FMath::IsFinite(Profile.BurningSupportIntensity)
		|| Profile.BurningSupportIntensity <= 0.0
		|| !FMath::IsFinite(Profile.BurningTailSeconds)
		|| Profile.BurningTailSeconds <= 0.0)
	{
		SetError(OutError, TEXT("ReturnToCold Profile 必须提供正支持阈值和尾焰时长。"));
		return false;
	}
	return true;
}

bool FFireCombustionModel::ValidateState(
	const FFireCombustionState& State,
	const FFireCombustionProfile& Profile,
	FString* OutError)
{
	if (!ValidateProfile(Profile, OutError) || State.LastResolvedWorldTimeMilliseconds < 0
		|| State.Revision == 0 || !FMath::IsFinite(State.HeatDose)
		|| !FMath::IsFinite(State.BurnStartTimeMilliseconds)
		|| !FMath::IsFinite(State.BurnEndTimeMilliseconds))
	{
		SetError(OutError, TEXT("火焰状态包含非法时间、Dose 或 Revision。"));
		return false;
	}
	switch (State.Phase)
	{
	case EFireCombustionPhase::Cold:
	case EFireCombustionPhase::BurnedOut:
		if (FMath::Abs(State.HeatDose) > DoseTolerance
			|| FMath::Abs(State.BurnStartTimeMilliseconds) > TimeToleranceMilliseconds
			|| FMath::Abs(State.BurnEndTimeMilliseconds) > TimeToleranceMilliseconds)
		{
			SetError(OutError, TEXT("Cold/BurnedOut 必须清空 Dose 与燃烧时间。"));
			return false;
		}
		return true;
	case EFireCombustionPhase::Heating:
		if (State.HeatDose <= 0.0 || State.HeatDose >= Profile.IgnitionDose
			|| FMath::Abs(State.BurnStartTimeMilliseconds) > TimeToleranceMilliseconds
			|| FMath::Abs(State.BurnEndTimeMilliseconds) > TimeToleranceMilliseconds)
		{
			SetError(OutError, TEXT("Heating Dose 必须严格位于 (0, IgnitionDose)。"));
			return false;
		}
		return true;
	case EFireCombustionPhase::Burning:
		if (FMath::Abs(State.HeatDose) > DoseTolerance
			|| State.BurnStartTimeMilliseconds < 0.0
			|| State.BurnEndTimeMilliseconds <= State.BurnStartTimeMilliseconds
			|| State.BurnEndTimeMilliseconds <= State.LastResolvedWorldTimeMilliseconds - TimeToleranceMilliseconds)
		{
			SetError(OutError, TEXT("Burning 必须清空 Dose 并持有尚未到期的合法绝对起止时间。"));
			return false;
		}
		return true;
	default:
		SetError(OutError, TEXT("未知火焰状态。"));
		return false;
	}
}

FFireIntervalResult FFireCombustionModel::AdvanceInterval(
	const FFireCombustionState& InitialState,
	const FFireCombustionProfile& Profile,
	const FFireIntervalInput& Input)
{
	FFireIntervalResult Result;
	Result.FinalState = InitialState;
	if (!ValidateState(InitialState, Profile)
		|| !ValidateInput(Input)
		|| InitialState.LastResolvedWorldTimeMilliseconds != Input.StartTimeMilliseconds)
	{
		return Result;
	}
	if (Input.StartTimeMilliseconds == Input.EndTimeMilliseconds)
	{
		if (Result.FinalState.Phase == EFireCombustionPhase::Burning)
		{
			Result.bHasNextInternalDeadline = true;
			Result.NextInternalDeadlineMilliseconds =
				Result.FinalState.BurnEndTimeMilliseconds;
		}
		Result.bSucceeded = true;
		return Result;
	}

	FFireCombustionState& State = Result.FinalState;
	if (State.Phase == EFireCombustionPhase::BurnedOut)
	{
		State.LastResolvedWorldTimeMilliseconds = Input.EndTimeMilliseconds;
		State.Revision = NextRevision(State.Revision);
		Result.bSucceeded = true;
		return Result;
	}

	for (const FFireIntensitySegment& Segment : Input.Segments)
	{
		double CursorMilliseconds = Segment.StartTimeMilliseconds;
		constexpr int32 MaximumTransitionsPerInputSegment = 16;
		int32 TransitionCount = 0;
		bool bObservedIgnitionInSegment = false;
		while (CursorMilliseconds < Segment.EndTimeMilliseconds - TimeToleranceMilliseconds)
		{
			// ReturnToCold 在“高于 Dose 衰减、低于燃烧支持阈值”的恒定输入下会形成
			// 解析周期。先保留首个真实点燃事件，随后一次跳过任意多个完整周期，
			// 避免长区间退化成逐周期重放，同时保持最终状态与任意分片一致。
			if (bObservedIgnitionInSegment
				&& State.Phase == EFireCombustionPhase::Cold
				&& Profile.BurnCompletion == EFireBurnCompletion::ReturnToCold
				&& Segment.IncomingFireIntensity > Profile.HeatDecayPerSecond
				&& Segment.IncomingFireIntensity < Profile.BurningSupportIntensity)
			{
				const double RatePerSecond =
					Segment.IncomingFireIntensity - Profile.HeatDecayPerSecond;
				const double CycleMilliseconds =
					(Profile.IgnitionDose / RatePerSecond + Profile.BurningTailSeconds) * 1000.0;
				const double RemainingMilliseconds =
					Segment.EndTimeMilliseconds - CursorMilliseconds;
				if (FMath::IsFinite(CycleMilliseconds)
					&& CycleMilliseconds > TimeToleranceMilliseconds
					&& RemainingMilliseconds + TimeToleranceMilliseconds >= CycleMilliseconds)
				{
					const double CompleteCycles = FMath::FloorToDouble(
						(RemainingMilliseconds + TimeToleranceMilliseconds) / CycleMilliseconds);
					if (CompleteCycles >= 1.0)
					{
						CursorMilliseconds += CompleteCycles * CycleMilliseconds;
						if (CursorMilliseconds > Segment.EndTimeMilliseconds
							- TimeToleranceMilliseconds)
						{
							CursorMilliseconds = Segment.EndTimeMilliseconds;
						}
						++Result.AnalyticSegmentCount;
						continue;
					}
				}
			}
			if (++TransitionCount > MaximumTransitionsPerInputSegment)
			{
				Result.FinalState = InitialState;
				Result.Events.Reset();
				return Result;
			}
			++Result.AnalyticSegmentCount;
			const EFireCombustionPhase Before = State.Phase;
			const double BeforeCursor = CursorMilliseconds;
			if (State.Phase == EFireCombustionPhase::Cold
				|| State.Phase == EFireCombustionPhase::Heating)
			{
				AdvanceHeating(Result, State, Profile, Segment.IncomingFireIntensity,
					CursorMilliseconds, Segment.EndTimeMilliseconds);
			}
			else if (State.Phase == EFireCombustionPhase::Burning)
			{
				AdvanceBurning(Result, State, Profile, Segment.IncomingFireIntensity,
					CursorMilliseconds, Segment.EndTimeMilliseconds);
			}
			else
			{
				CursorMilliseconds = Segment.EndTimeMilliseconds;
			}
			if (!bObservedIgnitionInSegment)
			{
				bObservedIgnitionInSegment = Result.Events.ContainsByPredicate(
					[&Segment](const FFireCombustionEvent& Event)
					{
						return Event.Type == EFireCombustionEventType::Ignited
							&& Event.EffectiveTimeMilliseconds
								>= Segment.StartTimeMilliseconds - TimeToleranceMilliseconds;
					});
			}
			if (CursorMilliseconds <= BeforeCursor + TimeToleranceMilliseconds && State.Phase == Before)
			{
				Result.FinalState = InitialState;
				Result.Events.Reset();
				return Result;
			}
		}
	}

	State.LastResolvedWorldTimeMilliseconds = Input.EndTimeMilliseconds;
	State.Revision = NextRevision(State.Revision);
	if (!ValidateState(State, Profile))
	{
		Result.FinalState = InitialState;
		Result.Events.Reset();
		return Result;
	}
	if (State.Phase == EFireCombustionPhase::Burning)
	{
		Result.bHasNextInternalDeadline = true;
		Result.NextInternalDeadlineMilliseconds = State.BurnEndTimeMilliseconds;
	}
	Result.bSucceeded = true;
	return Result;
}

double FFireCombustionModel::ComputeDistanceAttenuation(
	const double SurfaceDistanceCentimeters,
	const double RangeCentimeters)
{
	if (!FMath::IsFinite(SurfaceDistanceCentimeters) || !FMath::IsFinite(RangeCentimeters)
		|| RangeCentimeters <= 0.0 || SurfaceDistanceCentimeters >= RangeCentimeters)
	{
		return 0.0;
	}
	const double X = FMath::Clamp(SurfaceDistanceCentimeters / RangeCentimeters, 0.0, 1.0);
	return 1.0 - X * X * (3.0 - 2.0 * X);
}
