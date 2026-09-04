#include "MeteorBallisticKernel.h"

#include "Math/VectorRegister.h"

namespace UE::ElementSandbox::Meteor
{
bool FMeteorBallisticKernel::SolveGroundIntersectionSeconds(
	const float StartZ,
	const float VelocityZ,
	const float GravityZ,
	const float GroundPlaneZ,
	float& OutSeconds)
{
	OutSeconds = 0.0f;
	if (!FMath::IsFinite(StartZ) || !FMath::IsFinite(VelocityZ)
		|| !FMath::IsFinite(GravityZ) || !FMath::IsFinite(GroundPlaneZ)
		|| StartZ < GroundPlaneZ - UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const float C = StartZ - GroundPlaneZ;
	if (C <= UE_KINDA_SMALL_NUMBER && VelocityZ <= 0.0f)
	{
		return true;
	}
	if (FMath::Abs(GravityZ) <= UE_SMALL_NUMBER)
	{
		if (VelocityZ >= -UE_SMALL_NUMBER)
		{
			return false;
		}
		OutSeconds = -C / VelocityZ;
		return FMath::IsFinite(OutSeconds) && OutSeconds >= 0.0f;
	}
	const float Discriminant = VelocityZ * VelocityZ - 2.0f * GravityZ * C;
	if (!FMath::IsFinite(Discriminant) || Discriminant < 0.0f)
	{
		return false;
	}
	const float Root = FMath::Sqrt(Discriminant);
	const float First = (-VelocityZ - Root) / GravityZ;
	const float Second = (-VelocityZ + Root) / GravityZ;
	OutSeconds = TNumericLimits<float>::Max();
	if (First >= 0.0f) OutSeconds = First;
	if (Second >= 0.0f) OutSeconds = FMath::Min(OutSeconds, Second);
	return FMath::IsFinite(OutSeconds) && OutSeconds != TNumericLimits<float>::Max();
}

bool FMeteorBallisticKernel::BuildExplosionLaunchVelocity(
	const FVector3f& HorizontalDirection,
	const float AzimuthOffsetDegrees,
	const float ElevationDegrees,
	const float Speed,
	FVector3f& OutVelocity)
{
	OutVelocity = FVector3f::ZeroVector;
	if (HorizontalDirection.ContainsNaN() || !FMath::IsFinite(AzimuthOffsetDegrees)
		|| !FMath::IsFinite(ElevationDegrees) || ElevationDegrees <= -90.0f
		|| ElevationDegrees >= 90.0f || !FMath::IsFinite(Speed) || Speed <= 0.0f)
	{
		return false;
	}

	FVector3f Direction(HorizontalDirection.X, HorizontalDirection.Y, 0.0f);
	Direction = Direction.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::ForwardVector);
	const float AzimuthRadians = FMath::DegreesToRadians(AzimuthOffsetDegrees);
	Direction = FVector3f(
		Direction.X * FMath::Cos(AzimuthRadians) - Direction.Y * FMath::Sin(AzimuthRadians),
		Direction.X * FMath::Sin(AzimuthRadians) + Direction.Y * FMath::Cos(AzimuthRadians), 0.0f);
	const float ElevationRadians = FMath::DegreesToRadians(ElevationDegrees);
	OutVelocity = Direction * (Speed * FMath::Cos(ElevationRadians))
		+ FVector3f::UpVector * (Speed * FMath::Sin(ElevationRadians));
	return !OutVelocity.ContainsNaN();
}

bool FMeteorBallisticKernel::BuildOutwardExplosionLaunchVelocity(
	const FVector3f& HorizontalRadial,
	const float AzimuthDeviationDegrees,
	const float ElevationDegrees,
	const float Speed,
	FVector3f& OutVelocity)
{
	OutVelocity = FVector3f::ZeroVector;
	if (HorizontalRadial.ContainsNaN() || !FMath::IsFinite(AzimuthDeviationDegrees)
		|| FMath::Abs(AzimuthDeviationDegrees) >= 90.0f
		|| HorizontalRadial.SizeSquared2D() <= UE_SMALL_NUMBER)
	{
		return false;
	}
	FVector3f Radial(HorizontalRadial.X, HorizontalRadial.Y, 0.0f);
	Radial.Normalize();
	if (!BuildExplosionLaunchVelocity(
		Radial, AzimuthDeviationDegrees, ElevationDegrees, Speed, OutVelocity))
	{
		return false;
	}
	const FVector3f HorizontalVelocity(OutVelocity.X, OutVelocity.Y, 0.0f);
	return FVector3f::DotProduct(HorizontalVelocity, Radial) > UE_SMALL_NUMBER;
}

FVector3f FMeteorBallisticKernel::SamplePosition(
	const FVector3f& Start,
	const FVector3f& Velocity,
	const FVector3f& Acceleration,
	const float Seconds)
{
	return Start + Velocity * Seconds + Acceleration * (0.5f * Seconds * Seconds);
}

FVector3f FMeteorBallisticKernel::SampleVelocity(
	const FVector3f& Velocity,
	const FVector3f& Acceleration,
	const float Seconds)
{
	return Velocity + Acceleration * Seconds;
}

FQuat4f FMeteorBallisticKernel::SampleRotation(
	const FQuat4f& StartRotation,
	const FVector3f& AngularVelocityDegrees,
	const float Seconds)
{
	const float SpeedDegrees = AngularVelocityDegrees.Length();
	if (SpeedDegrees <= UE_SMALL_NUMBER || Seconds <= 0.0f)
	{
		return StartRotation;
	}
	const FVector3f Axis = AngularVelocityDegrees / SpeedDegrees;
	const FQuat4f Delta(Axis, FMath::DegreesToRadians(SpeedDegrees * Seconds));
	return (Delta * StartRotation).GetNormalized();
}

FQuat4f FMeteorBallisticKernel::ComputeStableRestRotation(
	const FBox3f& ProductLocalBounds,
	const FVector3f& Scale,
	const FQuat4f& ImpactRotation)
{
	if (!ProductLocalBounds.IsValid || ProductLocalBounds.ContainsNaN()
		|| Scale.ContainsNaN() || Scale.GetMin() <= UE_SMALL_NUMBER
		|| ImpactRotation.ContainsNaN())
	{
		return ImpactRotation;
	}
	const FVector3f Extent = ProductLocalBounds.GetExtent() * Scale.GetAbs();
	const FVector3f Axes[3] = {
		FVector3f::ForwardVector,
		FVector3f::RightVector,
		FVector3f::UpVector};
	int32 SupportAxis = 0;
	for (int32 Axis = 1; Axis < 3; ++Axis)
	{
		const float CandidateExtent = Extent[Axis];
		const float CurrentExtent = Extent[SupportAxis];
		if (CandidateExtent < CurrentExtent - 0.01f)
		{
			SupportAxis = Axis;
		}
		else if (FMath::IsNearlyEqual(CandidateExtent, CurrentExtent, 0.01f))
		{
			const float CandidateVertical = FMath::Abs(
				ImpactRotation.RotateVector(Axes[Axis]).Z);
			const float CurrentVertical = FMath::Abs(
				ImpactRotation.RotateVector(Axes[SupportAxis]).Z);
			if (CandidateVertical > CurrentVertical)
			{
				SupportAxis = Axis;
			}
		}
	}

	FVector3f LocalDown = Axes[SupportAxis];
	FVector3f CurrentDown = ImpactRotation.RotateVector(LocalDown).GetSafeNormal();
	if (CurrentDown.Z > 0.0f)
	{
		LocalDown *= -1.0f;
		CurrentDown *= -1.0f;
	}
	const FQuat4f TipToGround = FQuat4f::FindBetweenNormals(
		CurrentDown, -FVector3f::UpVector);
	return (TipToGround * ImpactRotation).GetNormalized();
}

void FMeteorBallisticKernel::SampleSettlingPose(
	const FVector3f& ImpactPosition,
	const FQuat4f& ImpactRotation,
	const FVector3f& RestPosition,
	const FQuat4f& RestRotation,
	const float LiftHeight,
	const float NormalizedTime,
	FVector3f& OutPosition,
	FQuat4f& OutRotation)
{
	const float Time = FMath::Clamp(NormalizedTime, 0.0f, 1.0f);
	const float Alpha = Time * Time * (3.0f - 2.0f * Time);
	OutPosition = FMath::Lerp(ImpactPosition, RestPosition, Alpha);
	OutPosition.Z += FMath::Max(0.0f, LiftHeight) * 4.0f * Alpha * (1.0f - Alpha);

	FQuat4f End = RestRotation;
	if ((ImpactRotation.X * End.X + ImpactRotation.Y * End.Y
		+ ImpactRotation.Z * End.Z + ImpactRotation.W * End.W) < 0.0f)
	{
		End = FQuat4f(-End.X, -End.Y, -End.Z, -End.W);
	}
	OutRotation = FQuat4f(
		FMath::Lerp(ImpactRotation.X, End.X, Alpha),
		FMath::Lerp(ImpactRotation.Y, End.Y, Alpha),
		FMath::Lerp(ImpactRotation.Z, End.Z, Alpha),
		FMath::Lerp(ImpactRotation.W, End.W, Alpha)).GetNormalized();
}

float FMeteorBallisticKernel::ComputeGroundContactCenterOffsetZ(
	const FBox3f& ProductLocalBounds,
	const FVector3f& Scale,
	const FQuat4f& Rotation)
{
	if (!ProductLocalBounds.IsValid || ProductLocalBounds.ContainsNaN()
		|| Scale.ContainsNaN() || Scale.GetMin() <= UE_SMALL_NUMBER
		|| Rotation.ContainsNaN())
	{
		return 0.0f;
	}
	const FVector3f ScaledCenter = ProductLocalBounds.GetCenter() * Scale;
	const FVector3f ScaledExtent = ProductLocalBounds.GetExtent() * Scale.GetAbs();
	const float ProjectedExtentZ =
		FMath::Abs(Rotation.RotateVector(FVector3f(ScaledExtent.X, 0.0f, 0.0f)).Z)
		+ FMath::Abs(Rotation.RotateVector(FVector3f(0.0f, ScaledExtent.Y, 0.0f)).Z)
		+ FMath::Abs(Rotation.RotateVector(FVector3f(0.0f, 0.0f, ScaledExtent.Z)).Z);
	return ProjectedExtentZ - Rotation.RotateVector(ScaledCenter).Z;
}

float FMeteorBallisticKernel::ComputeSettlingLiftHeight(
	const FBox3f& ProductLocalBounds,
	const FVector3f& Scale,
	const float GroundPlaneZ,
	const FVector3f& ImpactPosition,
	const FQuat4f& ImpactRotation,
	const FVector3f& RestPosition,
	const FQuat4f& RestRotation)
{
	float LiftHeight = 0.0f;
	for (int32 Sample = 1; Sample < 16; ++Sample)
	{
		FVector3f Position;
		FQuat4f Rotation;
		SampleSettlingPose(
			ImpactPosition,
			ImpactRotation,
			RestPosition,
			RestRotation,
			0.0f,
			static_cast<float>(Sample) / 16.0f,
			Position,
			Rotation);
		const float Alpha = static_cast<float>(Sample) / 16.0f;
		const float EasedAlpha = Alpha * Alpha * (3.0f - 2.0f * Alpha);
		const float Envelope = 4.0f * EasedAlpha * (1.0f - EasedAlpha);
		if (Envelope <= UE_SMALL_NUMBER)
		{
			continue;
		}
		const float RequiredCenterZ = GroundPlaneZ
			+ ComputeGroundContactCenterOffsetZ(ProductLocalBounds, Scale, Rotation);
		LiftHeight = FMath::Max(LiftHeight, (RequiredCenterZ - Position.Z) / Envelope);
	}
	return FMath::Max(0.0f, LiftHeight) + 0.5f;
}

bool FMeteorBallisticKernel::CompilePage(
	const FMeteorWorkPage& WorkPage,
	const FMeteorRuntimeConfig& Config,
	const FMeteorBurstId BurstId,
	const uint64 PageId,
	FMeteorTrajectoryPage& OutPage)
{
	OutPage = {};
	if (!WorkPage.IsStructurallyValid() || !Config.IsValid()
		|| !BurstId.IsSet() || PageId == 0)
	{
		return false;
	}
	const int32 Count = WorkPage.Num();
	TArray<float, TAlignedHeapAllocator<64>> ImpactDurations;
	ImpactDurations.SetNumUninitialized(Count);
	for (int32 Lane = 0; Lane < Count; ++Lane)
	{
		float TargetCenterZ = Config.GroundPlaneZ;
		bool bSolved = false;
		// 最终旋转取决于飞行时间，而触地中心又取决于最终旋转。这里解一个很小的
		// 固定点；产品尺度远小于弹道，通常两轮内收敛，六轮是确定性上限。
		for (int32 Iteration = 0; Iteration < 6; ++Iteration)
		{
			bSolved = SolveGroundIntersectionSeconds(
				WorkPage.PageOrigin.Z + WorkPage.StartZ[Lane],
				WorkPage.VelocityZ[Lane],
				Config.GravityZ,
				TargetCenterZ,
				ImpactDurations[Lane]);
			if (!bSolved)
			{
				break;
			}
			const FQuat4f FinalRotation = SampleRotation(
				WorkPage.StartRotations[Lane],
				FVector3f(
					WorkPage.AngularX[Lane],
					WorkPage.AngularY[Lane],
					WorkPage.AngularZ[Lane]),
				ImpactDurations[Lane]);
			const float SolvedTargetCenterZ = Config.GroundPlaneZ
				+ ComputeGroundContactCenterOffsetZ(
					WorkPage.ProductLocalBounds, WorkPage.Scales[Lane], FinalRotation);
			if (!FMath::IsFinite(SolvedTargetCenterZ))
			{
				bSolved = false;
				break;
			}
			if (FMath::IsNearlyEqual(SolvedTargetCenterZ, TargetCenterZ, 0.01f))
			{
				TargetCenterZ = SolvedTargetCenterZ;
				break;
			}
			TargetCenterZ = SolvedTargetCenterZ;
		}
		if (!bSolved || !SolveGroundIntersectionSeconds(
			WorkPage.PageOrigin.Z + WorkPage.StartZ[Lane],
			WorkPage.VelocityZ[Lane],
			Config.GravityZ,
			TargetCenterZ,
			ImpactDurations[Lane]))
		{
			return false;
		}
	}

	TArray<float, TAlignedHeapAllocator<64>> EndpointX;
	TArray<float, TAlignedHeapAllocator<64>> EndpointY;
	TArray<float, TAlignedHeapAllocator<64>> EndpointZ;
	EndpointX.SetNumUninitialized(Count);
	EndpointY.SetNumUninitialized(Count);
	EndpointZ.SetNumUninitialized(Count);
	const VectorRegister4Float HalfGravity = VectorSetFloat1(0.5f * Config.GravityZ);
	int32 Lane = 0;
	for (; Lane + 4 <= Count; Lane += 4)
	{
		const VectorRegister4Float Time = VectorLoadAligned(ImpactDurations.GetData() + Lane);
		const VectorRegister4Float TimeSquared = VectorMultiply(Time, Time);
		VectorStoreAligned(
			VectorMultiplyAdd(VectorLoadAligned(WorkPage.VelocityX.GetData() + Lane), Time,
				VectorLoadAligned(WorkPage.StartX.GetData() + Lane)),
			EndpointX.GetData() + Lane);
		VectorStoreAligned(
			VectorMultiplyAdd(VectorLoadAligned(WorkPage.VelocityY.GetData() + Lane), Time,
				VectorLoadAligned(WorkPage.StartY.GetData() + Lane)),
			EndpointY.GetData() + Lane);
		VectorStoreAligned(
			VectorAdd(
				VectorMultiplyAdd(VectorLoadAligned(WorkPage.VelocityZ.GetData() + Lane), Time,
					VectorLoadAligned(WorkPage.StartZ.GetData() + Lane)),
				VectorMultiply(HalfGravity, TimeSquared)),
			EndpointZ.GetData() + Lane);
	}
	for (; Lane < Count; ++Lane)
	{
		const float Time = ImpactDurations[Lane];
		EndpointX[Lane] = WorkPage.StartX[Lane] + WorkPage.VelocityX[Lane] * Time;
		EndpointY[Lane] = WorkPage.StartY[Lane] + WorkPage.VelocityY[Lane] * Time;
		EndpointZ[Lane] = WorkPage.StartZ[Lane] + WorkPage.VelocityZ[Lane] * Time
			+ 0.5f * Config.GravityZ * Time * Time;
	}

	OutPage.BurstId = BurstId;
	OutPage.PageId = PageId;
	OutPage.Revision = WorkPage.Revision;
	OutPage.Kernel = WorkPage.Kernel;
	OutPage.RenderArchetypeId = WorkPage.RenderArchetypeId;
	OutPage.PageOrigin = WorkPage.PageOrigin;
	OutPage.ValidFromSeconds = TNumericLimits<double>::Max();
	for (const double ValidFrom : WorkPage.ValidFromTimes)
	{
		OutPage.ValidFromSeconds = FMath::Min(OutPage.ValidFromSeconds, ValidFrom);
	}
	OutPage.ValidUntilSeconds = 0.0;
	OutPage.Ordinals = WorkPage.Ordinals;
	OutPage.WorldEntityIds = WorkPage.WorldEntityIds;
	OutPage.LocalStarts.Reserve(Count);
	OutPage.InitialVelocities.Reserve(Count);
	OutPage.Accelerations.Reserve(Count);
	OutPage.AngularVelocitiesDegrees.Reserve(Count);
	OutPage.StartRotations = WorkPage.StartRotations;
	OutPage.Scales = WorkPage.Scales;
	OutPage.VisualRadii = WorkPage.VisualRadii;
	OutPage.StartTimeOffsets.Reserve(Count);
	OutPage.ImpactDurations.Reserve(Count);
	OutPage.SettlingDurations.Reserve(Count);
	OutPage.LocalImpactEndpoints.Reserve(Count);
	OutPage.LocalRestEndpoints.Reserve(Count);
	OutPage.RestRotations.Reserve(Count);
	OutPage.SettlingLiftHeights.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector3f Start(WorkPage.StartX[Index], WorkPage.StartY[Index], WorkPage.StartZ[Index]);
		const FVector3f Velocity(WorkPage.VelocityX[Index], WorkPage.VelocityY[Index], WorkPage.VelocityZ[Index]);
		const FVector3f Acceleration(0.0f, 0.0f, Config.GravityZ);
		const FVector3f ImpactEndpoint(EndpointX[Index], EndpointY[Index], EndpointZ[Index]);
		const FQuat4f ImpactRotation = SampleRotation(
			WorkPage.StartRotations[Index],
			FVector3f(WorkPage.AngularX[Index], WorkPage.AngularY[Index], WorkPage.AngularZ[Index]),
			ImpactDurations[Index]);
		const FQuat4f RestRotation = ComputeStableRestRotation(
			WorkPage.ProductLocalBounds, WorkPage.Scales[Index], ImpactRotation);
		const float LocalGroundPlaneZ = static_cast<float>(
			Config.GroundPlaneZ - WorkPage.PageOrigin.Z);
		FVector3f RestEndpoint = ImpactEndpoint;
		RestEndpoint.Z = LocalGroundPlaneZ
			+ ComputeGroundContactCenterOffsetZ(
				WorkPage.ProductLocalBounds, WorkPage.Scales[Index], RestRotation);
		const float SettlingLiftHeight = ComputeSettlingLiftHeight(
			WorkPage.ProductLocalBounds,
			WorkPage.Scales[Index],
			LocalGroundPlaneZ,
			ImpactEndpoint,
			ImpactRotation,
			RestEndpoint,
			RestRotation);
		OutPage.LocalStarts.Add(Start);
		OutPage.InitialVelocities.Add(Velocity);
		OutPage.Accelerations.Add(Acceleration);
		OutPage.AngularVelocitiesDegrees.Add(FVector3f(
			WorkPage.AngularX[Index], WorkPage.AngularY[Index], WorkPage.AngularZ[Index]));
		OutPage.StartTimeOffsets.Add(static_cast<float>(
			WorkPage.StartTimes[Index] - OutPage.ValidFromSeconds));
		OutPage.ImpactDurations.Add(ImpactDurations[Index]);
		OutPage.SettlingDurations.Add(Config.DebrisSettlingSeconds);
		OutPage.LocalImpactEndpoints.Add(ImpactEndpoint);
		OutPage.LocalRestEndpoints.Add(RestEndpoint);
		OutPage.RestRotations.Add(RestRotation);
		OutPage.SettlingLiftHeights.Add(SettlingLiftHeight);
		OutPage.ValidUntilSeconds = FMath::Max(
			OutPage.ValidUntilSeconds,
			WorkPage.StartTimes[Index] + ImpactDurations[Index]
				+ Config.DebrisSettlingSeconds);

		const float Radius = WorkPage.VisualRadii[Index];
		OutPage.SweptBounds += Start - FVector3f(Radius);
		OutPage.SweptBounds += Start + FVector3f(Radius);
		OutPage.SweptBounds += ImpactEndpoint - FVector3f(Radius);
		OutPage.SweptBounds += ImpactEndpoint + FVector3f(Radius);
		OutPage.SweptBounds += RestEndpoint - FVector3f(Radius);
		OutPage.SweptBounds += RestEndpoint + FVector3f(Radius);
		OutPage.SweptBounds += FVector3f(
			RestEndpoint.X + Radius,
			RestEndpoint.Y + Radius,
			FMath::Max(ImpactEndpoint.Z, RestEndpoint.Z) + SettlingLiftHeight + Radius);
		if (Config.GravityZ < 0.0f && Velocity.Z > 0.0f)
		{
			const float ApexTime = FMath::Clamp(
				-Velocity.Z / Config.GravityZ, 0.0f, ImpactDurations[Index]);
			const FVector3f Apex = SamplePosition(Start, Velocity, Acceleration, ApexTime);
			OutPage.SweptBounds += Apex - FVector3f(Radius);
			OutPage.SweptBounds += Apex + FVector3f(Radius);
		}
	}
	return OutPage.IsValid();
}
}
