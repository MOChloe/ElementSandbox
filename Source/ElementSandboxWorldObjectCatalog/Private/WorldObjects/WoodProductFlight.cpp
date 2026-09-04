#include "WorldObjects/WoodProductFlight.h"

bool FWoodProductFlight::IsValid() const
{
	return WorldEntityId.IsSet() && BurstId && BatchId && Revision && !DefinitionId.IsNone()
		&& !RestTransform.ContainsNaN() && RestTransform.GetRotation().IsNormalized()
		&& !StartOffset.ContainsNaN() && !ImpactOffset.ContainsNaN() && !Velocity.ContainsNaN()
		&& !Acceleration.ContainsNaN() && !AngularVelocityDegrees.ContainsNaN() && StartRotation.IsNormalized()
		&& FMath::IsFinite(LocalStartTime) && FMath::IsFinite(ImpactSeconds) && ImpactSeconds >= 0
		&& FMath::IsFinite(SettlingSeconds) && SettlingSeconds >= 0 && FMath::IsFinite(LiftHeight) && LiftHeight >= 0
		&& FMath::IsFinite(Radius) && Radius > 0;
}
float FWoodProductFlight::GetDisplacementExtent() const
{
	float Extent = FMath::Max(StartOffset.GetAbsMax(), ImpactOffset.GetAbsMax() + LiftHeight);
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		auto At = [&](float T) { return StartOffset[Axis] + Velocity[Axis] * T + 0.5f * Acceleration[Axis] * T * T; };
		Extent = FMath::Max(Extent, FMath::Abs(At(ImpactSeconds)));
		if (FMath::Abs(Acceleration[Axis]) > UE_SMALL_NUMBER)
			Extent = FMath::Max(Extent, FMath::Abs(At(FMath::Clamp(-Velocity[Axis] / Acceleration[Axis], 0.0f, ImpactSeconds))));
	}
	return Extent + 2.0f * Radius + 1.0f;
}
void FWoodProductFlight::PackCustomData(TArray<float>& Out) const
{
	const FQuat Q = RestTransform.GetRotation();
	const float Data[CustomFloatCount] = {
		StartOffset.X, StartOffset.Y, StartOffset.Z, static_cast<float>(LocalStartTime),
		Velocity.X, Velocity.Y, Velocity.Z, ImpactSeconds,
		Acceleration.X, Acceleration.Y, Acceleration.Z, SettlingSeconds,
		AngularVelocityDegrees.X, AngularVelocityDegrees.Y, AngularVelocityDegrees.Z, LiftHeight,
		StartRotation.X, StartRotation.Y, StartRotation.Z, StartRotation.W,
		static_cast<float>(Q.X), static_cast<float>(Q.Y), static_cast<float>(Q.Z), static_cast<float>(Q.W),
		ImpactOffset.X, ImpactOffset.Y, ImpactOffset.Z,
		Phase == EWoodProductFlightPhase::Prepared ? 0.0f : Phase == EWoodProductFlightPhase::Active ? 1.0f : 2.0f};
	Out.Append(Data, CustomFloatCount);
}
