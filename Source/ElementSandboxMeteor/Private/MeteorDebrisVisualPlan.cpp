#include "MeteorDebrisVisualPlan.h"

namespace UE::ElementSandbox::Meteor
{
	namespace
	{
		FVector RandomSignedVector(FRandomStream& Random, const FVector2D& AbsoluteRange)
		{
			auto MakeAxis = [&Random, &AbsoluteRange]()
			{
				const float Magnitude = Random.FRandRange(AbsoluteRange.X, AbsoluteRange.Y);
				return Random.RandRange(0, 1) == 0 ? -Magnitude : Magnitude;
			};
			return FVector(MakeAxis(), MakeAxis(), MakeAxis());
		}

		FVector RandomPointInBounds(FRandomStream& Random, const FBox& Bounds, const FVector& Margin)
		{
			const FVector Center = Bounds.GetCenter();
			const FVector Available = (Bounds.GetExtent() - Margin).ComponentMax(FVector::ZeroVector);
			return Center + FVector(
				Random.FRandRange(-Available.X, Available.X),
				Random.FRandRange(-Available.Y, Available.Y),
				Random.FRandRange(-Available.Z, Available.Z));
		}
	}

	bool FMeteorDebrisVisualPlanInput::IsValid() const
	{
		return SourceBounds.IsValid && ProductLocalBounds.IsValid
			&& !SourceBounds.ContainsNaN() && !ProductLocalBounds.ContainsNaN()
			&& ProductCount > 0 && StableSeed != 0
			&& FMath::IsFinite(UniformScaleRange.X) && FMath::IsFinite(UniformScaleRange.Y)
			&& UniformScaleRange.X > 0.0 && UniformScaleRange.X <= UniformScaleRange.Y
			&& FMath::IsFinite(AngularSpeedRange.X) && FMath::IsFinite(AngularSpeedRange.Y)
			&& AngularSpeedRange.X >= 0.0 && AngularSpeedRange.X <= AngularSpeedRange.Y;
	}

	bool BuildMeteorDebrisVisualPlan(
		const FMeteorDebrisVisualPlanInput& Input,
		TArray<FMeteorDebrisVisualLane>& OutLanes)
	{
		OutLanes.Reset();
		if (!Input.IsValid()) return false;

		FRandomStream Random(Input.StableSeed);
		const FVector ProductExtent = Input.ProductLocalBounds.GetExtent().GetAbs();
		OutLanes.Reserve(Input.ProductCount);

		for (int32 Index = 0; Index < Input.ProductCount; ++Index)
		{
			FMeteorDebrisVisualLane& Lane = OutLanes.AddDefaulted_GetRef();
			const float Variation = Random.FRandRange(
				Input.UniformScaleRange.X, Input.UniformScaleRange.Y);
			const FVector ProductScale(Variation);
			const FVector Location = RandomPointInBounds(
				Random, Input.SourceBounds, ProductExtent * Variation);
			const FRotator Rotation(
				Random.FRandRange(-180.0f, 180.0f),
				Random.FRandRange(-180.0f, 180.0f),
				Random.FRandRange(-180.0f, 180.0f));

			Lane.FlightWorldTransform = FTransform(
				Rotation,
				Location,
				ProductScale);
			Lane.SettlementScale = ProductScale;
			Lane.AngularVelocityDegrees = RandomSignedVector(Random, Input.AngularSpeedRange);
		}
		return OutLanes.Num() == Input.ProductCount;
	}
}
