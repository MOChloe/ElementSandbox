#pragma once

#include "Definition/WorldObjectDefinition.h"
#include "WorldObjectWorldRuntime.h"

#include "Engine/World.h"

namespace UE::ElementSandbox::WorldObjects::Private
{
/** Definition 或实例 Bounds 到 World AABB 的唯一校验入口。 */
inline bool TryCalculateWorldBounds(const UWorldObjectDefinition& Definition, const FBox* InstanceInteractionBounds,
									const FTransform& Transform, FBox& OutBounds)
{
	if (!Definition.IsDefinitionValid() || Transform.ContainsNaN())
	{
		return false;
	}
	const FBox& LocalBounds =
		InstanceInteractionBounds ? *InstanceInteractionBounds : Definition.InteractionLocalBounds;
	if (LocalBounds.IsValid == 0 || LocalBounds.ContainsNaN())
	{
		return false;
	}
	OutBounds = LocalBounds.TransformBy(Transform);
	return OutBounds.IsValid != 0 && !OutBounds.ContainsNaN();
}

inline uint32 NextRevision(const uint32 Current) { return Current == MAX_uint32 ? 1 : Current + 1; }

inline uint64 NextRevision64(const uint64 Current) { return Current == MAX_uint64 ? 1 : Current + 1; }

inline bool IsActorActiveState(const EWorldObjectMotionState State)
{
	return State == EWorldObjectMotionState::Attached || State == EWorldObjectMotionState::Physics;
}

inline int64 GetEffectiveTimeMilliseconds(const UWorld& World)
{
	return FMath::Max<int64>(0, FMath::RoundToInt64(World.GetTimeSeconds() * 1000.0));
}

inline bool AreWorldObjectShapesEqual(const FWorldObjectShapeDefinition& Left, const FWorldObjectShapeDefinition& Right)
{
	return Left.Kind == Right.Kind && Left.Center.Equals(Right.Center, 0.01) &&
		   Left.Rotation.Equals(Right.Rotation, 0.0001) && Left.HalfExtents.Equals(Right.HalfExtents, 0.01) &&
		   FMath::IsNearlyEqual(Left.Radius, Right.Radius, 0.01) &&
		   Left.CapsuleAxis.Equals(Right.CapsuleAxis, 0.0001) &&
		   FMath::IsNearlyEqual(Left.CapsuleSegmentHalfLength, Right.CapsuleSegmentHalfLength, 0.01) &&
		   Left.TemplateRevision == Right.TemplateRevision;
}

} // namespace UE::ElementSandbox::WorldObjects::Private
