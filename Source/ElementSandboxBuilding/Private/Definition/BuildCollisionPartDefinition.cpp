#include "Definition/BuildCollisionPartDefinition.h"

FName FBuildCollisionPartDefinition::GetEffectiveCollisionProfileName() const
{
	if (!CollisionProfileName.IsNone())
	{
		return CollisionProfileName;
	}

	return Mobility == EBuildCollisionMobility::Kinematic
		? FName(TEXT("BlockAllDynamic"))
		: FName(TEXT("BlockAll"));
}
