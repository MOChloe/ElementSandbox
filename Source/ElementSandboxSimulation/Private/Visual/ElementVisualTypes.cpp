#include "Visual/ElementVisualTypes.h"

namespace
{
	bool IsFiniteColor(const FLinearColor& Color)
	{
		return FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G)
			&& FMath::IsFinite(Color.B) && FMath::IsFinite(Color.A);
	}
}

FElementVisualKey FElementVisualKey::MakePersistent(
	const FWorldEntityId WorldEntityId,
	const FName VisualKind,
	const int32 VisualPartId)
{
	FElementVisualKey Result;
	Result.WorldEntityId = WorldEntityId;
	Result.VisualKind = VisualKind;
	Result.VisualPartId = VisualPartId;
	return Result;
}

FElementVisualKey FElementVisualKey::MakeRuntime(
	const uint64 RuntimeLeaseId,
	const uint32 RuntimeGeneration,
	const FName VisualKind,
	const int32 VisualPartId)
{
	FElementVisualKey Result;
	Result.RuntimeLeaseId = RuntimeLeaseId;
	Result.RuntimeGeneration = RuntimeGeneration;
	Result.VisualKind = VisualKind;
	Result.VisualPartId = VisualPartId;
	return Result;
}

bool FElementVisualKey::IsSet() const
{
	const bool bPersistent = WorldEntityId.IsSet();
	const bool bRuntime = RuntimeLeaseId != 0 && RuntimeGeneration != 0;
	return bPersistent != bRuntime && !VisualKind.IsNone() && VisualPartId >= 0;
}

bool operator<(const FElementVisualKey& Left, const FElementVisualKey& Right)
{
	if (Left.WorldEntityId != Right.WorldEntityId)
	{
		return Left.WorldEntityId < Right.WorldEntityId;
	}
	if (Left.RuntimeLeaseId != Right.RuntimeLeaseId)
	{
		return Left.RuntimeLeaseId < Right.RuntimeLeaseId;
	}
	if (Left.RuntimeGeneration != Right.RuntimeGeneration)
	{
		return Left.RuntimeGeneration < Right.RuntimeGeneration;
	}
	if (Left.VisualKind != Right.VisualKind)
	{
		return Left.VisualKind.LexicalLess(Right.VisualKind);
	}
	return Left.VisualPartId < Right.VisualPartId;
}

bool FElementVisualDescriptor::IsValid() const
{
	const FVector Scale = WorldTransform.GetScale3D();
	return Key.IsSet() && !VisualDefinitionId.IsNone() && Revision != 0
		&& !WorldTransform.ContainsNaN() && !Scale.ContainsNaN()
		&& Scale.GetAbsMax() > UE_DOUBLE_SMALL_NUMBER
			&& WorldBounds.IsValid != 0 && !WorldBounds.Min.ContainsNaN() && !WorldBounds.Max.ContainsNaN()
		&& IsFiniteColor(Color) && FMath::IsFinite(Intensity) && Intensity >= 0.0f
		&& StartTimeMilliseconds >= 0
		&& (EndTimeMilliseconds == 0 || EndTimeMilliseconds > StartTimeMilliseconds);
}

bool FElementVisualDescriptor::IsEquivalent(const FElementVisualDescriptor& Other) const
{
	return Key == Other.Key
			&& VisualDefinitionId == Other.VisualDefinitionId
			&& Shard == Other.Shard
			&& WorldTransform.Equals(Other.WorldTransform, 0.001)
		&& WorldBounds.Equals(Other.WorldBounds, 0.001)
		&& Color.Equals(Other.Color, UE_KINDA_SMALL_NUMBER)
		&& FMath::IsNearlyEqual(Intensity, Other.Intensity)
		&& StartTimeMilliseconds == Other.StartTimeMilliseconds
		&& EndTimeMilliseconds == Other.EndTimeMilliseconds
		&& Revision == Other.Revision;
}

const FElementVisualDescriptorArray& FElementVisualShardSnapshot::GetDescriptors() const
{
	static const FElementVisualDescriptorArray Empty;
	return Descriptors.IsValid() ? *Descriptors : Empty;
}
