#pragma once

#include "CoreMinimal.h"

class FElementEntityRegistry;

/** Element Authority Registry 内的进程本地身份；Slot 可复用，但 Generation 不可跨代复用。 */
struct ELEMENTSANDBOXSIMULATION_API FElementEntityHandle final
{
public:
	FElementEntityHandle() = default;

	bool IsSet() const
	{
		return RegistryId != 0 && Slot != INDEX_NONE && Generation != 0;
	}

	uint32 GetRegistryId() const { return RegistryId; }
	int32 GetSlot() const { return Slot; }
	uint32 GetGeneration() const { return Generation; }

	friend bool operator==(const FElementEntityHandle& Left, const FElementEntityHandle& Right)
	{
		return Left.RegistryId == Right.RegistryId
			&& Left.Slot == Right.Slot
			&& Left.Generation == Right.Generation;
	}

	friend bool operator!=(const FElementEntityHandle& Left, const FElementEntityHandle& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FElementEntityHandle& Left, const FElementEntityHandle& Right)
	{
		if (Left.RegistryId != Right.RegistryId) return Left.RegistryId < Right.RegistryId;
		if (Left.Slot != Right.Slot) return Left.Slot < Right.Slot;
		return Left.Generation < Right.Generation;
	}

	friend uint32 GetTypeHash(const FElementEntityHandle& Handle)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(Handle.RegistryId), GetTypeHash(Handle.Slot)),
			GetTypeHash(Handle.Generation));
	}

private:
	FElementEntityHandle(uint32 InRegistryId, int32 InSlot, uint32 InGeneration)
		: RegistryId(InRegistryId), Slot(InSlot), Generation(InGeneration)
	{
	}

	uint32 RegistryId = 0;
	int32 Slot = INDEX_NONE;
	uint32 Generation = 0;

	friend FElementEntityRegistry;
};

