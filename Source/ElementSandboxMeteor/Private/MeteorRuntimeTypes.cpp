#include "MeteorRuntimeTypes.h"

#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Storage/WorldChunkCodec.h"

namespace UE::ElementSandbox::Meteor
{
FTransform FMeteorTrajectoryPage::GetRestTransform(int32 Lane) const
{
	return FWorldChunkCodec::QuantizeTransform(FTransform(FQuat(RestRotations[Lane]),
		FVector(PageOrigin) + FVector(LocalRestEndpoints[Lane]), FVector(Scales[Lane])));
}

namespace
{
	constexpr uint32 PayloadMagic = 0x5254454d; // METR little-endian

	template <typename ElementType>
	void SerializeArray(FArchive& Archive, TArray<ElementType>& Values, const int32 ExpectedCount)
	{
		int32 Count = Values.Num();
		Archive << Count;
		if (Archive.IsLoading())
		{
			if (Count < 0 || Count > WorkPageCapacity || (ExpectedCount >= 0 && Count != ExpectedCount))
			{
				Archive.SetError();
				return;
			}
			Values.SetNumUninitialized(Count);
		}
		for (ElementType& Value : Values)
		{
			Archive << Value;
		}
	}
}

bool FMeteorDebrisSeed::IsValid() const
{
	return Key.IsSet() && WorldEntityId.IsSet() && !RenderArchetypeId.IsNone() && !StartPosition.ContainsNaN()
		&& !StartRotation.ContainsNaN() && !InitialVelocity.ContainsNaN()
		&& !AngularVelocityDegrees.ContainsNaN() && !Scale.ContainsNaN()
		&& Scale.GetMin() > UE_SMALL_NUMBER && ProductLocalBounds.IsValid
		&& !ProductLocalBounds.ContainsNaN() && FMath::IsFinite(VisualRadius)
		&& VisualRadius > UE_SMALL_NUMBER && FMath::IsFinite(StartTimeSeconds)
		&& FMath::IsFinite(ValidFromSeconds) && FMath::IsFinite(LatestComputeStartSeconds)
		&& ValidFromSeconds >= StartTimeSeconds
		&& LatestComputeStartSeconds <= ValidFromSeconds;
}

void FMeteorWorkPage::Reset(
	const FMeteorPageHandle InHandle,
	const FName ProductId,
	const FVector3d& Origin)
{
	Handle = InHandle;
	Kernel = EMeteorTrajectoryKernel::BallisticGroundPlane;
	RenderArchetypeId = ProductId;
	PageOrigin = Origin;
	ProductLocalBounds = FBox3f(EForceInit::ForceInit);
	EarliestDeadlineSeconds = TNumericLimits<double>::Max();
	Revision = Revision == MAX_uint32 ? 1 : Revision + 1;
	Ordinals.Reset(WorkPageCapacity);
	WorldEntityIds.Reset(WorkPageCapacity);
	StartX.Reset(WorkPageCapacity);
	StartY.Reset(WorkPageCapacity);
	StartZ.Reset(WorkPageCapacity);
	VelocityX.Reset(WorkPageCapacity);
	VelocityY.Reset(WorkPageCapacity);
	VelocityZ.Reset(WorkPageCapacity);
	AngularX.Reset(WorkPageCapacity);
	AngularY.Reset(WorkPageCapacity);
	AngularZ.Reset(WorkPageCapacity);
	StartRotations.Reset(WorkPageCapacity);
	Scales.Reset(WorkPageCapacity);
	VisualRadii.Reset(WorkPageCapacity);
	StartTimes.Reset(WorkPageCapacity);
	ValidFromTimes.Reset(WorkPageCapacity);
}

bool FMeteorWorkPage::Append(const FMeteorDebrisSeed& Seed)
{
	if (!Handle.IsSet() || IsFull() || !Seed.IsValid()
		|| RenderArchetypeId != Seed.RenderArchetypeId)
	{
		return false;
	}
	if (Ordinals.IsEmpty())
	{
		ProductLocalBounds = Seed.ProductLocalBounds;
	}
	else if (!ProductLocalBounds.Min.Equals(Seed.ProductLocalBounds.Min, UE_KINDA_SMALL_NUMBER)
		|| !ProductLocalBounds.Max.Equals(Seed.ProductLocalBounds.Max, UE_KINDA_SMALL_NUMBER))
	{
		return false;
	}
	const FVector3d Local = FVector3d(Seed.StartPosition) - PageOrigin;
	if (!FMath::IsFinite(Local.X) || !FMath::IsFinite(Local.Y) || !FMath::IsFinite(Local.Z)
		|| FMath::Abs(Local.X) > TNumericLimits<float>::Max()
		|| FMath::Abs(Local.Y) > TNumericLimits<float>::Max()
		|| FMath::Abs(Local.Z) > TNumericLimits<float>::Max())
	{
		return false;
	}
	Ordinals.Add(Seed.Key.DebrisOrdinal);
	WorldEntityIds.Add(Seed.WorldEntityId);
	StartX.Add(static_cast<float>(Local.X));
	StartY.Add(static_cast<float>(Local.Y));
	StartZ.Add(static_cast<float>(Local.Z));
	VelocityX.Add(static_cast<float>(Seed.InitialVelocity.X));
	VelocityY.Add(static_cast<float>(Seed.InitialVelocity.Y));
	VelocityZ.Add(static_cast<float>(Seed.InitialVelocity.Z));
	AngularX.Add(static_cast<float>(Seed.AngularVelocityDegrees.X));
	AngularY.Add(static_cast<float>(Seed.AngularVelocityDegrees.Y));
	AngularZ.Add(static_cast<float>(Seed.AngularVelocityDegrees.Z));
	StartRotations.Add(FQuat4f(Seed.StartRotation));
	Scales.Add(Seed.Scale);
	VisualRadii.Add(Seed.VisualRadius);
	StartTimes.Add(Seed.StartTimeSeconds);
	ValidFromTimes.Add(Seed.ValidFromSeconds);
	EarliestDeadlineSeconds = FMath::Min(EarliestDeadlineSeconds, Seed.LatestComputeStartSeconds);
	return true;
}

bool FMeteorWorkPage::IsStructurallyValid() const
{
	const int32 Count = Num();
	return Handle.IsSet() && Count > 0 && Count <= WorkPageCapacity
		&& !RenderArchetypeId.IsNone() && !PageOrigin.ContainsNaN()
		&& ProductLocalBounds.IsValid && !ProductLocalBounds.ContainsNaN()
		&& WorldEntityIds.Num() == Count
		&& StartX.Num() == Count && StartY.Num() == Count && StartZ.Num() == Count
		&& VelocityX.Num() == Count && VelocityY.Num() == Count && VelocityZ.Num() == Count
		&& AngularX.Num() == Count && AngularY.Num() == Count && AngularZ.Num() == Count
		&& StartRotations.Num() == Count && Scales.Num() == Count && VisualRadii.Num() == Count
		&& StartTimes.Num() == Count && ValidFromTimes.Num() == Count
		&& FMath::IsFinite(EarliestDeadlineSeconds);
}

bool FMeteorTrajectoryPage::IsValid() const
{
	const int32 Count = Num();
	const bool bStructureValid = BurstId.IsSet() && PageId != 0 && Revision != 0
		&& FormatVersion == TrajectoryPayloadFormatVersion
		&& Kernel == EMeteorTrajectoryKernel::BallisticGroundPlane
		&& !RenderArchetypeId.IsNone() && !PageOrigin.ContainsNaN()
		&& FMath::IsFinite(ValidFromSeconds) && FMath::IsFinite(ValidUntilSeconds)
		&& ValidUntilSeconds >= ValidFromSeconds && SweptBounds.IsValid
		&& Count > 0 && Count <= WorkPageCapacity
		&& WorldEntityIds.Num() == Count
		&& LocalStarts.Num() == Count && InitialVelocities.Num() == Count
		&& Accelerations.Num() == Count && AngularVelocitiesDegrees.Num() == Count
		&& StartRotations.Num() == Count && Scales.Num() == Count && VisualRadii.Num() == Count
		&& StartTimeOffsets.Num() == Count && ImpactDurations.Num() == Count
		&& SettlingDurations.Num() == Count && LocalImpactEndpoints.Num() == Count
		&& LocalRestEndpoints.Num() == Count && RestRotations.Num() == Count
		&& SettlingLiftHeights.Num() == Count;
	if (!bStructureValid) return false;
	TSet<FWorldEntityId> Ids;
	TSet<uint32> LaneOrdinals;
	for (int32 Lane = 0; Lane < Count; ++Lane)
	{
		if (!WorldEntityIds[Lane].IsSet() || Ids.Contains(WorldEntityIds[Lane])
			|| Ordinals[Lane] == MAX_uint32 || LaneOrdinals.Contains(Ordinals[Lane])
			|| LocalStarts[Lane].ContainsNaN() || InitialVelocities[Lane].ContainsNaN()
			|| Accelerations[Lane].ContainsNaN() || AngularVelocitiesDegrees[Lane].ContainsNaN()
			|| !StartRotations[Lane].IsNormalized() || !RestRotations[Lane].IsNormalized()
			|| Scales[Lane].ContainsNaN() || Scales[Lane].GetMin() <= UE_SMALL_NUMBER
			|| LocalImpactEndpoints[Lane].ContainsNaN() || LocalRestEndpoints[Lane].ContainsNaN()
			|| !FMath::IsFinite(StartTimeOffsets[Lane])
			|| !FMath::IsFinite(VisualRadii[Lane]) || VisualRadii[Lane] <= 0.0f
			|| !FMath::IsFinite(ImpactDurations[Lane]) || ImpactDurations[Lane] < 0.0f
			|| !FMath::IsFinite(SettlingDurations[Lane]) || SettlingDurations[Lane] < 0.0f
			|| !FMath::IsFinite(SettlingLiftHeights[Lane]) || SettlingLiftHeights[Lane] < 0.0f)
		{
			return false;
		}
		Ids.Add(WorldEntityIds[Lane]);
		LaneOrdinals.Add(Ordinals[Lane]);
	}
	return true;
}

bool FMeteorTrajectoryPage::SerializeToBytes(TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	if (!IsValid())
	{
		return false;
	}
	FMemoryWriter Writer(OutBytes, true);
	uint32 Magic = PayloadMagic;
	uint32 Version = FormatVersion;
	uint64 BurstValue = BurstId.Value;
	uint64 MutablePageId = PageId;
	uint32 MutableRevision = Revision;
	uint8 KernelValue = static_cast<uint8>(Kernel);
	Writer << Magic << Version << BurstValue << MutablePageId << MutableRevision << KernelValue;
	FName MutableProductId = RenderArchetypeId;
	FVector3d MutableOrigin = PageOrigin;
	double MutableValidFrom = ValidFromSeconds;
	double MutableValidUntil = ValidUntilSeconds;
	FBox3f MutableSweptBounds = SweptBounds;
	Writer << MutableProductId << MutableOrigin << MutableValidFrom << MutableValidUntil << MutableSweptBounds;
	TArray<uint32> MutableOrdinals = Ordinals;
	TArray<FWorldEntityId> MutableIds = WorldEntityIds;
	TArray<FVector3f> MutableStarts = LocalStarts;
	TArray<FVector3f> MutableVelocities = InitialVelocities;
	TArray<FVector3f> MutableAccelerations = Accelerations;
	TArray<FVector3f> MutableAngular = AngularVelocitiesDegrees;
	TArray<FQuat4f> MutableRotations = StartRotations;
	TArray<FVector3f> MutableScales = Scales;
	TArray<float> MutableVisualRadii = VisualRadii;
	TArray<float> MutableStartOffsets = StartTimeOffsets;
	TArray<float> MutableImpactDurations = ImpactDurations;
	TArray<float> MutableSettlingDurations = SettlingDurations;
	TArray<FVector3f> MutableImpactEndpoints = LocalImpactEndpoints;
	TArray<FVector3f> MutableRestEndpoints = LocalRestEndpoints;
	TArray<FQuat4f> MutableRestRotations = RestRotations;
	TArray<float> MutableSettlingLiftHeights = SettlingLiftHeights;
	SerializeArray(Writer, MutableOrdinals, Num());
	SerializeArray(Writer, MutableIds, Num());
	SerializeArray(Writer, MutableStarts, Num());
	SerializeArray(Writer, MutableVelocities, Num());
	SerializeArray(Writer, MutableAccelerations, Num());
	SerializeArray(Writer, MutableAngular, Num());
	SerializeArray(Writer, MutableRotations, Num());
	SerializeArray(Writer, MutableScales, Num());
	SerializeArray(Writer, MutableVisualRadii, Num());
	SerializeArray(Writer, MutableStartOffsets, Num());
	SerializeArray(Writer, MutableImpactDurations, Num());
	SerializeArray(Writer, MutableSettlingDurations, Num());
	SerializeArray(Writer, MutableImpactEndpoints, Num());
	SerializeArray(Writer, MutableRestEndpoints, Num());
	SerializeArray(Writer, MutableRestRotations, Num());
	SerializeArray(Writer, MutableSettlingLiftHeights, Num());
	return !Writer.IsError();
}

bool FMeteorTrajectoryPage::DeserializeFromBytes(
	const TConstArrayView<uint8> Bytes,
	FMeteorTrajectoryPage& OutPage)
{
	OutPage = {};
	if (Bytes.IsEmpty() || Bytes.Num() > 4 * 1024 * 1024)
	{
		return false;
	}
	TArray<uint8> OwnedBytes;
	OwnedBytes.Append(Bytes.GetData(), Bytes.Num());
	FMemoryReader Reader(OwnedBytes, true);
	uint32 Magic = 0;
	uint32 Version = 0;
	uint64 BurstValue = 0;
	uint8 KernelValue = 0;
	Reader << Magic << Version << BurstValue << OutPage.PageId << OutPage.Revision << KernelValue;
	if (Magic != PayloadMagic || Version != TrajectoryPayloadFormatVersion
		|| KernelValue != static_cast<uint8>(EMeteorTrajectoryKernel::BallisticGroundPlane))
	{
		return false;
	}
	OutPage.BurstId.Value = BurstValue;
	OutPage.FormatVersion = Version;
	OutPage.Kernel = static_cast<EMeteorTrajectoryKernel>(KernelValue);
	Reader << OutPage.RenderArchetypeId << OutPage.PageOrigin
		<< OutPage.ValidFromSeconds << OutPage.ValidUntilSeconds << OutPage.SweptBounds;
	SerializeArray(Reader, OutPage.Ordinals, -1);
	const int32 Count = OutPage.Ordinals.Num();
	SerializeArray(Reader, OutPage.WorldEntityIds, Count);
	SerializeArray(Reader, OutPage.LocalStarts, Count);
	SerializeArray(Reader, OutPage.InitialVelocities, Count);
	SerializeArray(Reader, OutPage.Accelerations, Count);
	SerializeArray(Reader, OutPage.AngularVelocitiesDegrees, Count);
	SerializeArray(Reader, OutPage.StartRotations, Count);
	SerializeArray(Reader, OutPage.Scales, Count);
	SerializeArray(Reader, OutPage.VisualRadii, Count);
	SerializeArray(Reader, OutPage.StartTimeOffsets, Count);
	SerializeArray(Reader, OutPage.ImpactDurations, Count);
	SerializeArray(Reader, OutPage.SettlingDurations, Count);
	SerializeArray(Reader, OutPage.LocalImpactEndpoints, Count);
	SerializeArray(Reader, OutPage.LocalRestEndpoints, Count);
	SerializeArray(Reader, OutPage.RestRotations, Count);
	SerializeArray(Reader, OutPage.SettlingLiftHeights, Count);
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize() || !OutPage.IsValid())
	{
		OutPage = {};
		return false;
	}
	return true;
}

bool FMeteorTrajectoryPage::BuildOrdinalSubset(
	const TConstArrayView<uint32> ActivatedOrdinals,
	FMeteorTrajectoryPage& OutPage) const
{
	OutPage = {};
	if (!IsValid() || ActivatedOrdinals.IsEmpty())
	{
		return false;
	}
	TSet<uint32> Requested;
	Requested.Reserve(ActivatedOrdinals.Num());
	for (const uint32 Ordinal : ActivatedOrdinals)
	{
		if (Ordinal == MAX_uint32 || Requested.Contains(Ordinal)) return false;
		Requested.Add(Ordinal);
	}
	OutPage.BurstId = BurstId;
	OutPage.PageId = PageId;
	OutPage.Revision = Revision;
	OutPage.FormatVersion = FormatVersion;
	OutPage.Kernel = Kernel;
	OutPage.RenderArchetypeId = RenderArchetypeId;
	OutPage.PageOrigin = PageOrigin;
	OutPage.ValidFromSeconds = ValidFromSeconds;
	OutPage.ValidUntilSeconds = ValidUntilSeconds;
	// 子集的原包围盒仍是保守包围，避免抽取热路径重新求轨迹极值。
	OutPage.SweptBounds = SweptBounds;
	for (int32 Lane = 0; Lane < Num(); ++Lane)
	{
		if (!Requested.Remove(Ordinals[Lane])) continue;
		OutPage.Ordinals.Add(Ordinals[Lane]);
		OutPage.WorldEntityIds.Add(WorldEntityIds[Lane]);
		OutPage.LocalStarts.Add(LocalStarts[Lane]);
		OutPage.InitialVelocities.Add(InitialVelocities[Lane]);
		OutPage.Accelerations.Add(Accelerations[Lane]);
		OutPage.AngularVelocitiesDegrees.Add(AngularVelocitiesDegrees[Lane]);
		OutPage.StartRotations.Add(StartRotations[Lane]);
		OutPage.Scales.Add(Scales[Lane]);
		OutPage.VisualRadii.Add(VisualRadii[Lane]);
		OutPage.StartTimeOffsets.Add(StartTimeOffsets[Lane]);
		OutPage.ImpactDurations.Add(ImpactDurations[Lane]);
		OutPage.SettlingDurations.Add(SettlingDurations[Lane]);
		OutPage.LocalImpactEndpoints.Add(LocalImpactEndpoints[Lane]);
		OutPage.LocalRestEndpoints.Add(LocalRestEndpoints[Lane]);
		OutPage.RestRotations.Add(RestRotations[Lane]);
		OutPage.SettlingLiftHeights.Add(SettlingLiftHeights[Lane]);
	}
	if (!Requested.IsEmpty() || !OutPage.IsValid())
	{
		OutPage = {};
		return false;
	}
	return true;
}

bool FMeteorRuntimeConfig::IsValid() const
{
	return FMath::IsFinite(MeteorHeight) && MeteorHeight > 0.0f
			&& FMath::IsFinite(MeteorDiameter) && MeteorDiameter > 0.0f
			&& FMath::IsFinite(MeteorFallSeconds) && MeteorFallSeconds > 0.0f
			&& FMath::IsFinite(MeteorShowcaseMinimumDistance)
			&& MeteorShowcaseMinimumDistance > 0.0f
			&& FMath::IsFinite(MeteorShowcasePreferredDistance)
			&& MeteorShowcasePreferredDistance >= MeteorShowcaseMinimumDistance
			&& FMath::IsFinite(MeteorShowcaseMaximumDistance)
			&& MeteorShowcaseMaximumDistance >= MeteorShowcasePreferredDistance
			&& FMath::IsFinite(MeteorApproachHorizontalDistance)
			&& MeteorApproachHorizontalDistance > 0.0f
		&& FMath::IsFinite(ShockwaveRadius) && ShockwaveRadius > 0.0f
		&& FMath::IsFinite(ImpactCoreRadius) && ImpactCoreRadius > 0.0f
		&& ImpactCoreRadius <= ShockwaveRadius
		&& FMath::IsFinite(ShockwaveSpeed) && ShockwaveSpeed > 0.0f
			&& FMath::IsFinite(RadialStrength) && RadialStrength >= 0.0f
			&& FMath::IsFinite(UpwardStrength) && UpwardStrength >= 0.0f
			&& !DebrisSpeedRange.ContainsNaN() && DebrisSpeedRange.X > 0.0f
			&& DebrisSpeedRange.Y >= DebrisSpeedRange.X
			&& !DebrisLowElevationDegrees.ContainsNaN() && DebrisLowElevationDegrees.X >= 0.0f
			&& DebrisLowElevationDegrees.Y >= DebrisLowElevationDegrees.X
			&& !DebrisMediumElevationDegrees.ContainsNaN()
			&& DebrisMediumElevationDegrees.X >= DebrisLowElevationDegrees.Y
			&& DebrisMediumElevationDegrees.Y >= DebrisMediumElevationDegrees.X
			&& !DebrisHighElevationDegrees.ContainsNaN()
			&& DebrisHighElevationDegrees.X >= DebrisMediumElevationDegrees.Y
			&& DebrisHighElevationDegrees.Y >= DebrisHighElevationDegrees.X
			&& DebrisHighElevationDegrees.Y < 90.0f
			&& FMath::IsFinite(DebrisMediumArcFraction) && DebrisMediumArcFraction >= 0.0f
			&& FMath::IsFinite(DebrisHighArcFraction) && DebrisHighArcFraction >= 0.0f
			&& DebrisMediumArcFraction + DebrisHighArcFraction <= 1.0f
				&& FMath::IsFinite(DebrisAirMaximumAzimuthDeviationDegrees)
				&& DebrisAirMaximumAzimuthDeviationDegrees >= 0.0f
				&& DebrisAirMaximumAzimuthDeviationDegrees < 90.0f
				&& FMath::IsFinite(DebrisGroundMaximumAzimuthDeviationDegrees)
				&& DebrisGroundMaximumAzimuthDeviationDegrees
					>= DebrisAirMaximumAzimuthDeviationDegrees
				&& DebrisGroundMaximumAzimuthDeviationDegrees < 90.0f
			&& FMath::IsFinite(DebrisGroundScatterFraction)
			&& DebrisGroundScatterFraction >= 0.0f && DebrisGroundScatterFraction <= 1.0f
			&& !DebrisGroundElevationDegrees.ContainsNaN()
			&& DebrisGroundElevationDegrees.X > -90.0f
			&& DebrisGroundElevationDegrees.Y >= DebrisGroundElevationDegrees.X
			&& DebrisGroundElevationDegrees.Y < 90.0f
			&& !DebrisGroundSpeedMultiplier.ContainsNaN()
			&& DebrisGroundSpeedMultiplier.X > 0.0f
			&& DebrisGroundSpeedMultiplier.Y >= DebrisGroundSpeedMultiplier.X
		&& FMath::IsFinite(GravityZ) && GravityZ < 0.0f
		&& FMath::IsFinite(GroundPlaneZ)
		&& FMath::IsFinite(DebrisSettlingSeconds) && DebrisSettlingSeconds > 0.0f
		&& FMath::IsFinite(NetworkLeadSeconds)
		&& NetworkLeadSeconds > 0.0f && FMath::IsFinite(EncodingEstimateSeconds)
		&& EncodingEstimateSeconds >= 0.0f && FMath::IsFinite(QueueSafetySeconds)
		&& QueueSafetySeconds >= 0.0f && MaximumWorkPages > 0
		&& MaximumQueryTilesPerPump > 0 && MaximumDestructionTargetsPerPump > 0
		&& MaximumDestructionTargetsPerSnapshotBatch > 0
		&& MaximumDestructionTargetsPerSnapshotBatch <= MaximumDestructionTargetsPerPump
		&& FMath::IsFinite(LocalServerGameplayBudgetMilliseconds)
		&& LocalServerGameplayBudgetMilliseconds > 0.0f
		&& FMath::IsFinite(DedicatedServerGameplayBudgetMilliseconds)
		&& DedicatedServerGameplayBudgetMilliseconds > 0.0f
		&& LocalServerWorkerConcurrency > 0 && DedicatedServerWorkerConcurrency > 0
					&& MaximumSettlementBatchSize > 0
					&& FMath::IsFinite(SettlementPriorityCellSize)
					&& SettlementPriorityCellSize > 0.0f
					&& FMath::IsFinite(SettlementPriorityRadius)
					&& SettlementPriorityRadius >= SettlementPriorityCellSize
					&& MinimumGlobalSettlementCountPerBatch >= 0
					&& MinimumGlobalSettlementCountPerBatch <= MaximumSettlementBatchSize
		&& FMath::IsFinite(ClientGameThreadBudgetMilliseconds)
		&& ClientGameThreadBudgetMilliseconds > 0.0f;
}

FVector FMeteorRuntimeConfig::ComputeMeteorStartLocation(
	const FVector& ImpactLocation,
	const FVector& ViewerLocation) const
{
	FVector OutwardFromViewer(
		ImpactLocation.X - ViewerLocation.X,
		ImpactLocation.Y - ViewerLocation.Y,
		0.0);
	OutwardFromViewer = OutwardFromViewer.GetSafeNormal(
		UE_SMALL_NUMBER, FVector::ForwardVector);
	return ImpactLocation
		+ OutwardFromViewer * MeteorApproachHorizontalDistance
		+ FVector::UpVector * MeteorHeight;
}

double FMeteorRuntimeConfig::ComputeShockwaveArrivalTime(
	const double ImpactTimeSeconds,
	const double Distance) const
{
	return ImpactTimeSeconds
		+ FMath::Max(0.0, Distance - static_cast<double>(ImpactCoreRadius)) / ShockwaveSpeed;
}

double FMeteorRuntimeConfig::ComputeShockwaveRadius(const double SecondsAfterImpact) const
{
	if (SecondsAfterImpact < 0.0)
	{
		return 0.0;
	}
	return FMath::Min<double>(
		ShockwaveRadius,
		ImpactCoreRadius + SecondsAfterImpact * ShockwaveSpeed);
}
}
