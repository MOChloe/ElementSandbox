#include "Fire/ElementFireRuleSet.h"

#include "Fire/FireCombustionModel.h"

namespace
{
	void SetError(FString* OutError, const TCHAR* Error)
	{
		if (OutError) *OutError = Error;
	}

	bool IsFinitePositive(const double Value)
	{
		return FMath::IsFinite(Value) && Value > 0.0;
	}

	bool ValidateSource(const FElementFireSourceRule& Rule)
	{
		return IsFinitePositive(Rule.Intensity) && IsFinitePositive(Rule.RangeCentimeters);
	}
}

UElementFireRuleSet::UElementFireRuleSet()
{
	StructureProfile.IgnitionDose = 6.00;
	StructureProfile.HeatDecayPerSecond = 0.20;
	StructureProfile.BurnDurationSeconds = 18.0;
	StructureProfile.EmittedFireIntensity = 1.00;
	StructureProfile.EmissionRangeCentimeters = 80.0;
	StructureProfile.EmissionPolicy = EFirePropagationPolicy::All;
	StructureProfile.BurnCompletion = EFireBurnCompletion::BurnedOut;

	StickProfile.IgnitionDose = 0.60;
	StickProfile.HeatDecayPerSecond = 0.15;
	StickProfile.BurnDurationSeconds = 30.0;
	StickProfile.EmittedFireIntensity = 0.65;
	StickProfile.EmissionRangeCentimeters = 80.0;
	StickProfile.EmissionPolicy = EFirePropagationPolicy::CharacterOnly;
	StickProfile.BurnCompletion = EFireBurnCompletion::BurnedOut;

	CharacterProfile.IgnitionDose = 0.35;
	CharacterProfile.HeatDecayPerSecond = 0.15;
	CharacterProfile.BurnDurationSeconds = 0.0;
	CharacterProfile.EmittedFireIntensity = 0.35;
	CharacterProfile.EmissionRangeCentimeters = 80.0;
	CharacterProfile.EmissionPolicy = EFirePropagationPolicy::CharacterOnly;
	CharacterProfile.BurnCompletion = EFireBurnCompletion::ReturnToCold;
	CharacterProfile.BurningSupportIntensity = 0.20;
	CharacterProfile.BurningTailSeconds = 6.0;

	FirePileSource.Intensity = 1.0;
	FirePileSource.RangeCentimeters = 45.0;
	FirePileSource.Policy = EFirePropagationPolicy::CharacterOnly;
	MountedTorchSource.Intensity = 1.0;
	MountedTorchSource.RangeCentimeters = 80.0;
	MountedTorchSource.Policy = EFirePropagationPolicy::CharacterOnly;
	FireballSource.Intensity = 3.0;
	FireballSource.RangeCentimeters = 50.0;
	FireballSource.Policy = EFirePropagationPolicy::All;

}

bool FFireRuleSnapshot::IsValid(FString* OutError) const
{
	if (Revision == 0
		|| !FFireCombustionModel::ValidateProfile(Structure, OutError)
		|| !FFireCombustionModel::ValidateProfile(Stick, OutError)
		|| !FFireCombustionModel::ValidateProfile(Character, OutError))
	{
		return false;
	}
	if (Structure.BurnCompletion != EFireBurnCompletion::BurnedOut
		|| Stick.BurnCompletion != EFireBurnCompletion::BurnedOut
		|| Character.BurnCompletion != EFireBurnCompletion::ReturnToCold)
	{
		SetError(OutError, TEXT("结构、木棍和 Character 的燃尽语义不匹配。"));
		return false;
	}
	if (!ValidateSource(FirePile) || !ValidateSource(MountedTorch) || !ValidateSource(Fireball)
		|| FirePile.Policy != EFirePropagationPolicy::CharacterOnly
		|| MountedTorch.Policy != EFirePropagationPolicy::CharacterOnly
		|| Fireball.Policy != EFirePropagationPolicy::All
		|| FirePileCapsuleCenter.ContainsNaN()
		|| !IsFinitePositive(FirePileCapsuleRadius)
		|| !IsFinitePositive(FirePileCapsuleSegmentHalfLength)
		|| MountedTorchSphereCenter.ContainsNaN()
		|| !IsFinitePositive(MountedTorchSphereRadius)
		|| !IsFinitePositive(FireballSphereRadius)
		|| FireballLifetimeMilliseconds <= 0)
	{
		SetError(OutError, TEXT("固定火源/Fireball Shape、时长、强度、距离或策略非法。"));
		return false;
	}
	if (!IsFinitePositive(BaseDamagePerPeriod) || !IsFinitePositive(DamagePeriodSeconds)
		|| !FMath::IsFinite(StackTwoExitIntensity)
		|| !FMath::IsFinite(StackTwoEnterIntensity)
		|| !FMath::IsFinite(StackThreeExitIntensity)
		|| !FMath::IsFinite(StackThreeEnterIntensity)
		|| StackTwoExitIntensity < 0.0
		|| StackTwoExitIntensity >= StackTwoEnterIntensity
		|| StackTwoEnterIntensity >= StackThreeExitIntensity
		|| StackThreeExitIntensity >= StackThreeEnterIntensity)
	{
		SetError(OutError, TEXT("GAS 层数迟滞或伤害周期非法。"));
		return false;
	}
	return true;
}

bool UElementFireRuleSet::Freeze(FFireRuleSnapshot& OutSnapshot, FString& OutError) const
{
	OutSnapshot = {};
	OutSnapshot.Revision = RuleRevision;
	OutSnapshot.Structure = StructureProfile;
	OutSnapshot.Stick = StickProfile;
	OutSnapshot.Character = CharacterProfile;
	OutSnapshot.FirePile = FirePileSource;
	OutSnapshot.MountedTorch = MountedTorchSource;
	OutSnapshot.Fireball = FireballSource;
	OutSnapshot.FirePileCapsuleCenter = FirePileCapsuleCenter;
	OutSnapshot.FirePileCapsuleRadius = FirePileCapsuleRadius;
	OutSnapshot.FirePileCapsuleSegmentHalfLength = FirePileCapsuleSegmentHalfLength;
	OutSnapshot.MountedTorchSphereCenter = MountedTorchSphereCenter;
	OutSnapshot.MountedTorchSphereRadius = MountedTorchSphereRadius;
	OutSnapshot.FireballSphereRadius = FireballSphereRadius;
	OutSnapshot.FireballLifetimeMilliseconds = FireballLifetimeMilliseconds;
	OutSnapshot.StackTwoEnterIntensity = StackTwoEnterIntensity;
	OutSnapshot.StackTwoExitIntensity = StackTwoExitIntensity;
	OutSnapshot.StackThreeEnterIntensity = StackThreeEnterIntensity;
	OutSnapshot.StackThreeExitIntensity = StackThreeExitIntensity;
	OutSnapshot.BaseDamagePerPeriod = BaseDamagePerPeriod;
	OutSnapshot.DamagePeriodSeconds = DamagePeriodSeconds;
	return OutSnapshot.IsValid(&OutError);
}
