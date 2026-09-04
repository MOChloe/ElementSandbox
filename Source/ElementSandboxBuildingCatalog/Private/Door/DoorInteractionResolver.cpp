#include "Door/DoorInteractionResolver.h"

#include "Door/DoorStateFragment.h"
#include "Entity/BuildEntityRegistry.h"

bool TryResolveBuildDoorInteraction(
	const FBuildEntityRegistry& Registry,
	const FBuildEntityHandle Entity,
	EBuildDoorInteractionIntent& OutIntent)
{
	OutIntent = EBuildDoorInteractionIntent::None;
	if (!Registry.IsAlive(Entity))
	{
		return false;
	}

	const FBuildDoorStateFragment* DoorState =
		Registry.FindFragment<FBuildDoorStateFragment>(Entity);
	if (!DoorState)
	{
		return false;
	}
	switch (DoorState->State)
	{
	case EBuildDoorState::Closed:
		OutIntent = EBuildDoorInteractionIntent::Open;
		return true;
	case EBuildDoorState::Open:
		OutIntent = EBuildDoorInteractionIntent::Close;
		return true;
	case EBuildDoorState::Opening:
	case EBuildDoorState::Closing:
	default:
		return false;
	}
}
