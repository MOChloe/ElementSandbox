#pragma once

#include "Runtime/ElementRuntimeTypes.h"

enum class EElementFirePersistentHostDomain : uint8
{
	Building = 1,
	WorldObject = 2
};

struct FElementFirePersistentSource final
{
	double Intensity = 0.0;
	double RangeCentimeters = 0.0;
	uint8 Policy = 0;
	int64 ExpireTimeMilliseconds = 0;

	bool IsValid() const
	{
		return FMath::IsFinite(Intensity) && Intensity > 0.0
			&& FMath::IsFinite(RangeCentimeters) && RangeCentimeters > 0.0
			&& ExpireTimeMilliseconds >= 0;
	}
};

/** Element WorldStorage Adapter 与 Fire 装配层之间的版本无关纯值。 */
struct FElementFirePersistentRecord final
{
	FWorldEntityId ElementId;
	FWorldEntityId HostId;
	EElementFirePersistentHostDomain HostDomain = EElementFirePersistentHostDomain::Building;
	FVector WorldLocation = FVector::ZeroVector;
	uint32 StateRevision = 0;
	FElementAuthorityTargetStateSnapshot AuthorityState;
	TOptional<FElementFirePersistentSource> Source;

	bool IsValid() const
	{
		return ElementId.IsSet() && HostId.IsSet() && !WorldLocation.ContainsNaN()
			&& StateRevision != 0 && AuthorityState.IsValid()
			&& (!Source.IsSet() || Source->IsValid());
	}
};

enum class EElementPersistentRemovalSemantic : uint8
{
	RuntimeEvict,
	GameplayDestroy,
	LeaveInterest,
	FailedRestoreRollback
};
