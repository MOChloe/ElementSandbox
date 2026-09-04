#pragma once

#include "CoreMinimal.h"

#include "WorldObjectEntityHandle.generated.h"

/** 单个 WorldObject Registry 内的进程本地身份，不允许跨 World 或通过网络传递。 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectEntityHandle final
{
	GENERATED_BODY()

public:
	FWorldObjectEntityHandle() = default;

	bool IsSet() const
	{
		return RegistryId != 0 && Slot != INDEX_NONE && Generation != 0;
	}

	uint32 GetRegistryId() const { return RegistryId; }
	int32 GetSlot() const { return Slot; }
	uint32 GetGeneration() const { return Generation; }

	friend bool operator==(
		const FWorldObjectEntityHandle& Left,
		const FWorldObjectEntityHandle& Right)
	{
		return Left.RegistryId == Right.RegistryId
			&& Left.Slot == Right.Slot
			&& Left.Generation == Right.Generation;
	}

	friend bool operator!=(
		const FWorldObjectEntityHandle& Left,
		const FWorldObjectEntityHandle& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(
		const FWorldObjectEntityHandle& Left,
		const FWorldObjectEntityHandle& Right)
	{
		if (Left.RegistryId != Right.RegistryId)
		{
			return Left.RegistryId < Right.RegistryId;
		}
		if (Left.Slot != Right.Slot)
		{
			return Left.Slot < Right.Slot;
		}
		return Left.Generation < Right.Generation;
	}

	friend uint32 GetTypeHash(const FWorldObjectEntityHandle& Handle)
	{
		return HashCombine(
			HashCombine(::GetTypeHash(Handle.RegistryId), ::GetTypeHash(Handle.Slot)),
			::GetTypeHash(Handle.Generation));
	}

private:
	FWorldObjectEntityHandle(
		const uint32 InRegistryId,
		const int32 InSlot,
		const uint32 InGeneration)
		: RegistryId(InRegistryId)
		, Slot(InSlot)
		, Generation(InGeneration)
	{
	}

	UPROPERTY()
	uint32 RegistryId = 0;

	UPROPERTY()
	int32 Slot = INDEX_NONE;

	UPROPERTY()
	uint32 Generation = 0;

	friend class FWorldObjectEntityRegistry;
};
