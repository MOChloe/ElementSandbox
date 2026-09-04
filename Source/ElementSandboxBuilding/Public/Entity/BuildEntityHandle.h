#pragma once

#include "CoreMinimal.h"

class FBuildEntityRegistry;

/**
 * Building ECS Entity 的稳定值身份。
 *
 * Handle 只对创建它的 Registry 有意义。Index 允许复用，但 RegistryId 与 Generation
 * 共同保证跨 Registry Handle 和已销毁 Handle 不会误命中新 Entity。
 */
struct ELEMENTSANDBOXBUILDING_API FBuildEntityHandle final
{
public:
	FBuildEntityHandle() = default;

	bool IsSet() const
	{
		return RegistryId != 0 && Index != INDEX_NONE && Generation != 0;
	}

	int32 GetIndex() const
	{
		return Index;
	}

	uint32 GetGeneration() const
	{
		return Generation;
	}

	uint32 GetRegistryId() const
	{
		return RegistryId;
	}

	friend bool operator==(const FBuildEntityHandle& Left, const FBuildEntityHandle& Right)
	{
		return Left.RegistryId == Right.RegistryId
			&& Left.Index == Right.Index
			&& Left.Generation == Right.Generation;
	}

	friend bool operator!=(const FBuildEntityHandle& Left, const FBuildEntityHandle& Right)
	{
		return !(Left == Right);
	}

	friend uint32 GetTypeHash(const FBuildEntityHandle& Handle)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(Handle.RegistryId), GetTypeHash(Handle.Index)),
			GetTypeHash(Handle.Generation));
	}

private:
	FBuildEntityHandle(const uint32 InRegistryId, const int32 InIndex, const uint32 InGeneration)
		: RegistryId(InRegistryId)
		, Index(InIndex)
		, Generation(InGeneration)
	{
	}

	uint32 RegistryId = 0;
	int32 Index = INDEX_NONE;
	uint32 Generation = 0;

	friend FBuildEntityRegistry;
};
