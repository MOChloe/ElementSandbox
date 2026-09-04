#include "Runtime/ElementFireRuntimeTypes.h"

#include "Entity/ElementEntityRegistry.h"
#include "Fire/FireCombustionModel.h"

namespace ElementFireRuntimeNames
{
	const FName NumericProcessor(TEXT("Element.Fire.Numeric"));
	const FName StateProcessor(TEXT("Element.Thermal.StateProcessor"));
	const FName ThermalInput(TEXT("Element.Thermal.Input"));
	const FName CurrentFireRate(TEXT("Element.Thermal.FireRate"));
	const FName ThermalState(TEXT("Element.Thermal.State"));
	const FName Projection(TEXT("Element.Fire.Projection"));
	const FName FireSourceFragment(TEXT("Element.Fire.SourceFragment"));
}

namespace
{
	constexpr uint8 FireSourcePayloadCount = 3;
	constexpr uint8 ThermalStatePayloadCount = 8;

	FElementCompoundShape ExpandFireSourceShape(
		const FElementCompoundShape& Source,
		const double RangeCentimeters)
	{
		const FBox Bounds = Source.CalculateWorldBounds().ExpandBy(RangeCentimeters);
		if (Bounds.IsValid == 0) return {};
		// 这份 Shape 只作 BVH/运动粗筛，用世界空间 AABB 才不会让宿主 Scale
		// 错误缩放以厘米为单位的 Range；真实权重仍由未扩张 Source 的表面距离决定。
		FElementCompoundShape Result;
		Result.Shapes.Add(FElementShape::MakeBox(
			Bounds.GetCenter(), FQuat::Identity, Bounds.GetExtent()));
		return Result;
	}

	uint64 AdvanceNonZero(const uint64 Value)
	{
		return Value == MAX_uint64 ? 1 : Value + 1;
	}

	uint32 FoldRevision(const uint64 Value)
	{
		return static_cast<uint32>((Value - 1u) % static_cast<uint64>(MAX_uint32)) + 1u;
	}

	double FindNumericValue(const FElementStateProcessorInput& Input, const FName Channel)
	{
		for (const FElementNumericValue& Value : Input.NumericValues)
		{
			if (Value.Channel == Channel) return Value.Value;
		}
		return 0.0;
	}

	bool DecodeThermalState(
		const TOptional<FElementStateValue>& Encoded,
		const int64 InitialTimeMilliseconds,
		FFireCombustionState& OutState,
		double& OutConsumedInput,
		uint8& OutGasStack,
		double& OutPreviousRate)
	{
		OutState = {};
		OutState.LastResolvedWorldTimeMilliseconds = InitialTimeMilliseconds;
		OutState.Revision = 1;
		OutConsumedInput = 0.0;
		OutGasStack = 0;
		OutPreviousRate = 0.0;
		if (!Encoded.IsSet()) return true;
		const FElementStateValue& Value = Encoded.GetValue();
		if (Value.SchemaId != ElementFireRuntimeNames::ThermalState
			|| Value.Revision == 0 || Value.Payload.Count != ThermalStatePayloadCount)
		{
			return false;
		}
		const double Phase = Value.Payload.Values[0];
		const double GasStack = Value.Payload.Values[6];
		if (Phase < 0.0 || Phase > static_cast<double>(EFireCombustionPhase::BurnedOut)
			|| Phase != FMath::RoundToDouble(Phase)
			|| GasStack < 0.0 || GasStack > 3.0 || GasStack != FMath::RoundToDouble(GasStack))
		{
			return false;
		}
		OutState.Phase = static_cast<EFireCombustionPhase>(FMath::RoundToInt(Phase));
		OutState.HeatDose = Value.Payload.Values[1];
		OutState.BurnStartTimeMilliseconds = Value.Payload.Values[2];
		OutState.BurnEndTimeMilliseconds = Value.Payload.Values[3];
		OutState.LastResolvedWorldTimeMilliseconds = FMath::RoundToInt64(Value.Payload.Values[4]);
		OutState.Revision = FoldRevision(Value.Revision);
		OutConsumedInput = Value.Payload.Values[5];
		OutGasStack = static_cast<uint8>(FMath::RoundToInt(GasStack));
		OutPreviousRate = Value.Payload.Values[7];
		return FMath::IsFinite(OutConsumedInput) && OutConsumedInput >= 0.0
			&& FMath::IsFinite(OutPreviousRate) && OutPreviousRate >= 0.0;
	}

	FElementStateValue EncodeThermalState(
		const FFireCombustionState& State,
		const double ConsumedInput,
		const uint8 GasStack,
		const double CurrentRate,
		const uint64 Revision)
	{
		FElementStateValue Value;
		Value.SchemaId = ElementFireRuntimeNames::ThermalState;
		Value.Revision = Revision;
		Value.Payload.Count = ThermalStatePayloadCount;
		Value.Payload.Values[0] = static_cast<double>(State.Phase);
		Value.Payload.Values[1] = State.HeatDose;
		Value.Payload.Values[2] = State.BurnStartTimeMilliseconds;
		Value.Payload.Values[3] = State.BurnEndTimeMilliseconds;
		Value.Payload.Values[4] = static_cast<double>(State.LastResolvedWorldTimeMilliseconds);
		Value.Payload.Values[5] = ConsumedInput;
		Value.Payload.Values[6] = static_cast<double>(GasStack);
		Value.Payload.Values[7] = CurrentRate;
		return Value;
	}

	uint8 ResolveGasStack(
		const uint8 Previous,
		const bool bBurning,
		const double Intensity,
		const FFireRuleSnapshot& Rules)
	{
		if (!bBurning) return 0;
		if (Previous >= 3)
		{
			if (Intensity >= Rules.StackThreeExitIntensity) return 3;
			return Intensity < Rules.StackTwoExitIntensity ? 1 : 2;
		}
		if (Previous == 2)
		{
			if (Intensity >= Rules.StackThreeEnterIntensity) return 3;
			return Intensity < Rules.StackTwoExitIntensity ? 1 : 2;
		}
		if (Intensity >= Rules.StackThreeEnterIntensity) return 3;
		if (Intensity >= Rules.StackTwoEnterIntensity) return 2;
		return 1;
	}

	TOptional<int64> PredictNextWake(
		const FFireCombustionState& State,
		const FFireCombustionProfile& Profile,
		const double CurrentRate,
		const int64 Now)
	{
		if (State.Phase == EFireCombustionPhase::Burning)
		{
			return FMath::Max<int64>(Now, FMath::CeilToInt64(State.BurnEndTimeMilliseconds));
		}
		if (State.Phase == EFireCombustionPhase::BurnedOut) return {};
		const double NetRate = CurrentRate - Profile.HeatDecayPerSecond;
		if (State.Phase == EFireCombustionPhase::Cold && NetRate > 0.0)
		{
			return Now + FMath::Max<int64>(1, FMath::CeilToInt64(Profile.IgnitionDose / NetRate * 1000.0));
		}
		if (State.Phase == EFireCombustionPhase::Heating)
		{
			if (NetRate > 0.0)
			{
				return Now + FMath::Max<int64>(1,
					FMath::CeilToInt64((Profile.IgnitionDose - State.HeatDose) / NetRate * 1000.0));
			}
			if (NetRate < 0.0)
			{
				return Now + FMath::Max<int64>(1,
					FMath::CeilToInt64(State.HeatDose / -NetRate * 1000.0));
			}
		}
		return {};
	}
}

bool FFireSourceFragment::IsValid() const
{
	return Shape.IsValid() && Revision != 0 && FMath::IsFinite(Intensity) && Intensity > 0.0
			&& FMath::IsFinite(RangeCentimeters) && RangeCentimeters > 0.0
			&& ExpireTimeMilliseconds >= 0;
}

FElementValuePayload MakeFireTargetMetadata(
	const EElementFireTargetProfile Profile,
	const bool bFireInteractionActive)
{
	FElementValuePayload Metadata;
	Metadata.Count = 2;
	Metadata.Values[0] = static_cast<double>(Profile);
	Metadata.Values[1] = bFireInteractionActive ? 1.0 : 0.0;
	return Metadata;
}

EElementFireTargetProfile ReadFireTargetMetadata(const FElementValuePayload& Metadata)
{
	if (Metadata.Count < 1 || !FMath::IsFinite(Metadata.Values[0])) return EElementFireTargetProfile::None;
	const double Raw = Metadata.Values[0];
	if (Raw != FMath::RoundToDouble(Raw)
		|| Raw < 0.0 || Raw > static_cast<double>(EElementFireTargetProfile::Character))
	{
		return EElementFireTargetProfile::None;
	}
	return static_cast<EElementFireTargetProfile>(FMath::RoundToInt(Raw));
}

FElementFireNumericProcessor::FElementFireNumericProcessor()
{
	Descriptor.ProcessorId = ElementFireRuntimeNames::NumericProcessor;
	Descriptor.FragmentType = ElementFireRuntimeNames::FireSourceFragment;
	Descriptor.TargetDomains = EElementTargetDomain::Character
		| EElementTargetDomain::Building | EElementTargetDomain::WorldObject;
	Descriptor.WeightMode = EElementSpatialWeightMode::SurfaceDistanceFalloff;
	Descriptor.WriteNumericChannels = {
		ElementFireRuntimeNames::ThermalInput,
		ElementFireRuntimeNames::CurrentFireRate};
	Descriptor.RecomputedNumericChannels = {ElementFireRuntimeNames::CurrentFireRate};
}

bool FElementFireNumericProcessor::CaptureInfluence(
	const FElementEntityRegistry& Registry,
	const FElementEntityHandle Source,
	FElementInfluenceSnapshot& OutSnapshot) const
{
	OutSnapshot = {};
	const FFireSourceFragment* Fire = Registry.FindFragment<FFireSourceFragment>(Source);
	if (!Fire || !Fire->IsValid()) return false;
	OutSnapshot.Source = Source;
	OutSnapshot.HostTarget = Fire->HostTarget;
	OutSnapshot.ProcessorId = Descriptor.ProcessorId;
	OutSnapshot.FragmentRevision = Fire->Revision;
	OutSnapshot.Shape = ExpandFireSourceShape(Fire->Shape, Fire->RangeCentimeters);
	OutSnapshot.FalloffOriginShape = Fire->Shape;
	OutSnapshot.FalloffDistanceCentimeters = Fire->RangeCentimeters;
	OutSnapshot.SpatialHandle = Fire->SpatialSnapshot;
	OutSnapshot.Payload.Count = FireSourcePayloadCount;
	OutSnapshot.Payload.Values[0] = Fire->Intensity;
	OutSnapshot.Payload.Values[1] = static_cast<double>(Fire->Policy);
	OutSnapshot.Payload.Values[2] = static_cast<double>(Fire->ExpireTimeMilliseconds);
	return OutSnapshot.IsValid();
}

void FElementFireNumericProcessor::Execute(
	const TConstArrayView<FElementQueryStatistics> Statistics,
	TArray<FElementOffset>& OutOffsets) const
{
	for (const FElementQueryStatistics& Entry : Statistics)
	{
		if (!Entry.IsValid() || Entry.SourcePayload.Count != FireSourcePayloadCount) continue;
		const double Intensity = Entry.SourcePayload.Values[0];
		const int32 Policy = FMath::RoundToInt(Entry.SourcePayload.Values[1]);
		if (!FMath::IsFinite(Intensity) || Intensity <= 0.0
			|| (Policy != static_cast<int32>(EFirePropagationPolicy::All)
				&& Policy != static_cast<int32>(EFirePropagationPolicy::CharacterOnly)))
		{
			continue;
		}
		const bool bInteractiveStick = Entry.Target.Domain == EElementTargetDomain::WorldObject
			&& ReadFireTargetMetadata(Entry.TargetMetadata) == EElementFireTargetProfile::Stick
			&& Entry.TargetMetadata.Count >= 2 && Entry.TargetMetadata.Values[1] > 0.5;
		if (Policy == static_cast<int32>(EFirePropagationPolicy::CharacterOnly)
			&& Entry.Target.Domain != EElementTargetDomain::Character && !bInteractiveStick)
		{
			continue;
		}
		const double IntegratedDose = Intensity * Entry.IntegratedWeightSeconds;
		if (IntegratedDose > 0.0)
		{
			OutOffsets.Add({Entry.Target, ElementFireRuntimeNames::ThermalInput, IntegratedDose});
		}
		const double CurrentRate = Intensity * Entry.EndWeight;
		if (CurrentRate > 0.0)
		{
			OutOffsets.Add({Entry.Target, ElementFireRuntimeNames::CurrentFireRate, CurrentRate});
		}
	}
}

FElementThermalStateProcessor::FElementThermalStateProcessor(const FFireRuleSnapshot& InRules)
	: Rules(InRules)
{
	Descriptor.ProcessorId = ElementFireRuntimeNames::StateProcessor;
	Descriptor.TargetDomains = EElementTargetDomain::Character
		| EElementTargetDomain::Building | EElementTargetDomain::WorldObject;
	Descriptor.ReadNumericChannels = {
		ElementFireRuntimeNames::ThermalInput,
		ElementFireRuntimeNames::CurrentFireRate};
	Descriptor.OwnedStateChannel = ElementFireRuntimeNames::ThermalState;
}

const FFireCombustionProfile* FElementThermalStateProcessor::SelectProfile(
	const EElementFireTargetProfile Profile) const
{
	switch (Profile)
	{
	case EElementFireTargetProfile::Structure: return &Rules.Structure;
	case EElementFireTargetProfile::Stick: return &Rules.Stick;
	case EElementFireTargetProfile::Character: return &Rules.Character;
	default: return nullptr;
	}
}

bool FElementThermalStateProcessor::Execute(
	const FElementStateProcessorInput& Input,
	FElementStateProcessorOutput& OutOutput) const
{
	OutOutput = {};
	const EElementFireTargetProfile TargetProfile = ReadFireTargetMetadata(Input.TargetMetadata);
	const FFireCombustionProfile* Profile = SelectProfile(TargetProfile);
	if (!Input.Target.IsValid() || Input.TargetRevision == 0 || !Profile) return false;

	FFireCombustionState Initial;
	double ConsumedInput = 0.0;
	uint8 PreviousGasStack = 0;
	double PreviousRate = 0.0;
	if (!DecodeThermalState(
		Input.CurrentState, Input.PreviousSettlementTimeMilliseconds,
		Initial, ConsumedInput, PreviousGasStack, PreviousRate)
		|| Initial.LastResolvedWorldTimeMilliseconds != Input.PreviousSettlementTimeMilliseconds
		|| !FFireCombustionModel::ValidateState(Initial, *Profile))
	{
		return false;
	}

	const double TotalInput = FindNumericValue(Input, ElementFireRuntimeNames::ThermalInput);
	const double CurrentRate = FMath::Max(0.0,
		FindNumericValue(Input, ElementFireRuntimeNames::CurrentFireRate));
	if (!FMath::IsFinite(TotalInput) || TotalInput < ConsumedInput || !FMath::IsFinite(CurrentRate)) return false;
	const double DeltaSeconds = FMath::Max(0.0,
		static_cast<double>(Input.WorldTimeMilliseconds - Input.PreviousSettlementTimeMilliseconds) / 1000.0);
	const double AddedInput = TotalInput - ConsumedInput;
	// 有路径/稳定区间统计时使用查询层累计量；只有边界速率变化时用旧速率把
	// 上一个稳定区间结算到当前时刻，再从本轮起保存新速率。
	const double AverageInput = DeltaSeconds > 0.0
		? (AddedInput > UE_DOUBLE_SMALL_NUMBER
			? FMath::Max(0.0, AddedInput / DeltaSeconds)
			: PreviousRate)
		: 0.0;

	FFireIntervalInput Interval;
	Interval.StartTimeMilliseconds = Input.PreviousSettlementTimeMilliseconds;
	Interval.EndTimeMilliseconds = Input.WorldTimeMilliseconds;
	if (Interval.EndTimeMilliseconds > Interval.StartTimeMilliseconds)
	{
		Interval.Segments.Add({Interval.StartTimeMilliseconds, Interval.EndTimeMilliseconds, AverageInput});
	}
	const FFireIntervalResult Result = FFireCombustionModel::AdvanceInterval(Initial, *Profile, Interval);
	if (!Result.bSucceeded) return false;

	const bool bWasBurning = Initial.IsBurning();
	const bool bIsBurning = Result.FinalState.IsBurning();
	const uint8 GasStack = TargetProfile == EElementFireTargetProfile::Character
		? ResolveGasStack(PreviousGasStack, bIsBurning, CurrentRate, Rules)
		: 0;
	const uint64 NextRevision = AdvanceNonZero(
		Input.CurrentState.IsSet() ? Input.CurrentState->Revision : Input.TargetRevision);

	OutOutput.Target = Input.Target;
	OutOutput.NextState = EncodeThermalState(
		Result.FinalState, TotalInput, GasStack, CurrentRate, NextRevision);
	OutOutput.NextWakeTimeMilliseconds = PredictNextWake(
		Result.FinalState, *Profile, CurrentRate, Input.WorldTimeMilliseconds);

	if (!bWasBurning && bIsBurning)
	{
		FElementStructuralCommand& Command = OutOutput.StructuralCommands.AddDefaulted_GetRef();
		Command.Kind = EElementStructuralCommandKind::AddInfluenceFragment;
		Command.Target = Input.Target;
		Command.FragmentType = ElementFireRuntimeNames::FireSourceFragment;
		Command.Payload.Count = 3;
		Command.Payload.Values[0] = Profile->EmittedFireIntensity;
		Command.Payload.Values[1] = Profile->EmissionRangeCentimeters;
		Command.Payload.Values[2] = static_cast<double>(Profile->EmissionPolicy);
	}
	else if (bWasBurning && !bIsBurning)
	{
		FElementStructuralCommand& Command = OutOutput.StructuralCommands.AddDefaulted_GetRef();
		Command.Kind = EElementStructuralCommandKind::RemoveInfluenceFragment;
		Command.Target = Input.Target;
		Command.FragmentType = ElementFireRuntimeNames::FireSourceFragment;
	}

	FElementProjectionCommand& Projection = OutOutput.ProjectionCommands.AddDefaulted_GetRef();
	Projection.Target = Input.Target;
	Projection.Channel = ElementFireRuntimeNames::Projection;
	Projection.Revision = NextRevision;
	Projection.Payload.Count = 7;
	Projection.Payload.Values[0] = static_cast<double>(Result.FinalState.Phase);
	Projection.Payload.Values[1] = Result.FinalState.HeatDose;
	Projection.Payload.Values[2] = CurrentRate;
	Projection.Payload.Values[3] = static_cast<double>(GasStack);
	Projection.Payload.Values[4] = Result.FinalState.BurnStartTimeMilliseconds;
	Projection.Payload.Values[5] = Result.FinalState.BurnEndTimeMilliseconds;
	Projection.Payload.Values[6] = static_cast<double>(Input.WorldTimeMilliseconds);
	return OutOutput.NextState.IsValid() && Projection.IsValid();
}
