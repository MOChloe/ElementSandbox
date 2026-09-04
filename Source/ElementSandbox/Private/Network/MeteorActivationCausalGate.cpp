#include "Network/MeteorActivationCausalGate.h"

#include "WorldStorageSubsystem.h"

bool ApplyMeteorSourceTombstone(
	UWorldStorageSubsystem& Storage,
	const FWorldEntityId SourceWorldEntityId,
	const uint32 SourceTombstoneRevision)
{
	const FWorldNetworkEntityRemoval Removal{
		SourceWorldEntityId, SourceTombstoneRevision, true};
	return ApplyMeteorSourceTombstones(Storage, MakeArrayView(&Removal, 1));
}

bool ApplyMeteorSourceTombstones(
	UWorldStorageSubsystem& Storage,
	const TConstArrayView<FWorldNetworkEntityRemoval> Removals)
{
	if (Removals.IsEmpty())
	{
		return false;
	}
	for (const FWorldNetworkEntityRemoval& Removal : Removals)
	{
		if (!Removal.IsValid() || !Removal.bGameplayDestroy)
		{
			return false;
		}
	}
	return Storage.ApplyNetworkRemoveBatch(Removals);
}
