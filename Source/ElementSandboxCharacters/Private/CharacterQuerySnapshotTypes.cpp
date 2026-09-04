#include "CharacterQuerySnapshotTypes.h"

FBox FCharacterCapsuleSnapshot::CalculateBounds() const
{
	if (!IsValid()) return FBox(ForceInit);
	const FVector Segment = Axis * GetSegmentHalfLength();
	return FBox(Center - Segment - FVector(Radius), Center + Segment + FVector(Radius));
}

bool FCharacterCapsuleSnapshot::IsValid() const
{
	return !Center.ContainsNaN() && !Axis.ContainsNaN() && Axis.IsNormalized()
		&& FMath::IsFinite(Radius) && Radius > UE_DOUBLE_SMALL_NUMBER
		&& FMath::IsFinite(HalfHeight) && HalfHeight >= Radius;
}

bool FCharacterCapsuleSnapshot::Equals(
	const FCharacterCapsuleSnapshot& Other,
	const double Tolerance) const
{
	return Center.Equals(Other.Center, Tolerance) && Axis.Equals(Other.Axis, Tolerance)
		&& FMath::IsNearlyEqual(Radius, Other.Radius, Tolerance)
		&& FMath::IsNearlyEqual(HalfHeight, Other.HalfHeight, Tolerance);
}

bool FCharacterQuerySnapshotChange::IsValid() const
{
	if (!Handle.IsSet()) return false;
	switch (Kind)
	{
	case ECharacterQuerySnapshotChangeKind::Upsert:
		return !Previous.IsSet() && Current.IsSet() && Current->IsValid() && Current->Handle == Handle;
	case ECharacterQuerySnapshotChangeKind::Motion:
		return Previous.IsSet() && Current.IsSet() && Previous->IsValid() && Current->IsValid()
			&& Previous->Handle == Handle && Current->Handle == Handle
			&& Current->Revision > Previous->Revision
			&& Current->EffectiveTimeMilliseconds >= Previous->EffectiveTimeMilliseconds;
	case ECharacterQuerySnapshotChangeKind::Remove:
		return Previous.IsSet() && Previous->IsValid() && Previous->Handle == Handle && !Current.IsSet();
	default:
		return false;
	}
}

bool FCharacterQuerySnapshotBatch::IsValid() const
{
	if (Sequence == 0 || Changes.IsEmpty()) return false;
	TSet<FCharacterSnapshotHandle> Handles;
	for (const FCharacterQuerySnapshotChange& Change : Changes)
	{
		if (!Change.IsValid() || Handles.Contains(Change.Handle)) return false;
		Handles.Add(Change.Handle);
	}
	return true;
}

