#include "Query/ElementCentralQuery.h"

#include "Shape/ElementShapeKernels.h"

bool FElementCentralQuery::EvaluateMotion(
	const FElementMotionSubmission& Motion,
	const FElementTargetSnapshot& Target,
	const FElementInfluenceSnapshot& Influence,
	const EElementSpatialWeightMode WeightMode,
	FElementQueryStatistics& OutStatistics)
{
	OutStatistics = {};
	if (!Motion.IsValid() || !Target.IsValid() || !Influence.IsValid()
		|| Motion.Target != Target.Target || Motion.ExpectedTargetRevision != Target.Revision)
	{
		return false;
	}
	OutStatistics.Source = Influence.Source;
	OutStatistics.Target = Target.Target;
	OutStatistics.ProcessorId = Influence.ProcessorId;
	OutStatistics.SourceRevision = Influence.FragmentRevision;
	OutStatistics.TargetRevision = Target.Revision;
	OutStatistics.WindowStartMilliseconds = Motion.Segments[0].StartTimeMilliseconds;
	OutStatistics.WindowEndMilliseconds = Motion.Segments.Last().EndTimeMilliseconds;
	OutStatistics.SourcePayload = Influence.Payload;
	OutStatistics.TargetMetadata = Target.Metadata;
	OutStatistics.bContinuousMotion = true;
	for (const FElementMotionSegment& Segment : Motion.Segments)
	{
		FElementSweptShapeResult Result;
		if (!FElementShapeKernels::Sweep(
			Influence.Shape, Target.Shape, Segment, WeightMode, Result,
			WeightMode == EElementSpatialWeightMode::SurfaceDistanceFalloff
				? &Influence.FalloffOriginShape : nullptr,
			Influence.FalloffDistanceCentimeters)) continue;
		OutStatistics.ContactDurationSeconds += Result.ContactDurationSeconds;
		OutStatistics.IntegratedWeightSeconds += Result.IntegratedWeightSeconds;
		OutStatistics.MaximumWeight = FMath::Max(OutStatistics.MaximumWeight, Result.MaximumWeight);
		OutStatistics.EndWeight = Result.EndWeight;
	}
	return OutStatistics.ContactDurationSeconds > 0.0 && OutStatistics.IsValid();
}

bool FElementCentralQuery::EvaluateStatic(
	const FElementTargetSnapshot& Target,
	const FElementInfluenceSnapshot& Influence,
	const EElementSpatialWeightMode WeightMode,
	const int64 WorldTimeMilliseconds,
	FElementQueryStatistics& OutStatistics)
{
	OutStatistics = {};
	if (!Target.IsValid() || !Influence.IsValid() || WorldTimeMilliseconds < 0) return false;
	const double Weight = WeightMode == EElementSpatialWeightMode::SurfaceDistanceFalloff
		? FElementShapeKernels::CalculateSurfaceDistanceWeight(
			Influence.FalloffOriginShape, Target.Shape, Influence.FalloffDistanceCentimeters)
		: FElementShapeKernels::CalculateWeight(Influence.Shape, Target.Shape, WeightMode);
	if (Weight <= 0.0) return false;
	OutStatistics.Source = Influence.Source;
	OutStatistics.Target = Target.Target;
	OutStatistics.ProcessorId = Influence.ProcessorId;
	OutStatistics.SourceRevision = Influence.FragmentRevision;
	OutStatistics.TargetRevision = Target.Revision;
	OutStatistics.WindowStartMilliseconds = WorldTimeMilliseconds;
	OutStatistics.WindowEndMilliseconds = WorldTimeMilliseconds;
	OutStatistics.MaximumWeight = Weight;
	OutStatistics.EndWeight = Weight;
	OutStatistics.SourcePayload = Influence.Payload;
	OutStatistics.TargetMetadata = Target.Metadata;
	OutStatistics.bContinuousMotion = false;
	return OutStatistics.IsValid();
}
