#pragma once

#include "CoreMinimal.h"

#include "CharacterSnapshotHandle.generated.h"

class UCharacterQuerySnapshotSubsystem;

/** 普通 Character Actor 的进程本地查询投影身份；只用于拒绝 Slot 复用后的迟到结果。 */
USTRUCT()
struct ELEMENTSANDBOXCHARACTERS_API FCharacterSnapshotHandle final
{
	GENERATED_BODY()

public:
	FCharacterSnapshotHandle() = default;

	bool IsSet() const { return RegistryId != 0 && Slot != INDEX_NONE && Generation != 0; }
	uint32 GetRegistryId() const { return RegistryId; }
	int32 GetSlot() const { return Slot; }
	uint32 GetGeneration() const { return Generation; }

	friend bool operator==(const FCharacterSnapshotHandle& Left, const FCharacterSnapshotHandle& Right)
	{
		return Left.RegistryId == Right.RegistryId && Left.Slot == Right.Slot
			&& Left.Generation == Right.Generation;
	}

	friend bool operator!=(const FCharacterSnapshotHandle& Left, const FCharacterSnapshotHandle& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FCharacterSnapshotHandle& Left, const FCharacterSnapshotHandle& Right)
	{
		if (Left.RegistryId != Right.RegistryId) return Left.RegistryId < Right.RegistryId;
		if (Left.Slot != Right.Slot) return Left.Slot < Right.Slot;
		return Left.Generation < Right.Generation;
	}

	friend uint32 GetTypeHash(const FCharacterSnapshotHandle& Handle)
	{
		return HashCombineFast(HashCombineFast(GetTypeHash(Handle.RegistryId), GetTypeHash(Handle.Slot)),
			GetTypeHash(Handle.Generation));
	}

private:
	FCharacterSnapshotHandle(const uint32 InRegistryId, const int32 InSlot, const uint32 InGeneration)
		: RegistryId(InRegistryId), Slot(InSlot), Generation(InGeneration)
	{
	}

	UPROPERTY()
	uint32 RegistryId = 0;
	UPROPERTY()
	int32 Slot = INDEX_NONE;
	UPROPERTY()
	uint32 Generation = 0;

	friend class UCharacterQuerySnapshotSubsystem;
};
