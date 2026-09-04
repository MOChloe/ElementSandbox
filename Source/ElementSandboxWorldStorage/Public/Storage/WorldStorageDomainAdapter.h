#pragma once

#include "CoreMinimal.h"
#include "Chunk/WorldChunkTypes.h"

class UScriptStruct;

enum class EWorldStorageRestorePhase : uint8
{
	Primary,
	Dependent
};

enum class EWorldFragmentPersistence : uint8
{
	Persistent,
	Derived,
	RuntimeOnly
};

/**
 * WorldStorage 不依赖具体 ECS。每个领域通过这一批量契约捕获稳定值、恢复投影并执行明确语义的移除。
 * 所有批量入口都必须是 Game Thread 上的全有或全无操作。四种移除语义不得互相转发后靠布尔值猜测。
 */
class ELEMENTSANDBOXWORLDSTORAGE_API IWorldStorageDomainAdapter
{
public:
	virtual ~IWorldStorageDomainAdapter() = default;

	virtual EWorldEntityDomain GetDomain() const = 0;
	virtual EWorldStorageRestorePhase GetRestorePhase() const = 0;
	virtual bool CaptureBatch(
		TConstArrayView<FWorldEntityId> EntityIds,
		TArray<FWorldPersistentEntityRecord>& OutRecords,
		FString& OutError) const = 0;
	virtual bool RestoreBatch(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldPersistentEntityRecord> Records,
		FString& OutError) = 0;
	virtual bool RuntimeEvictBatch(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldEntityId> EntityIds,
		FString& OutError) = 0;
	virtual bool GameplayDestroyBatch(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldEntityId> EntityIds,
		FString& OutError) = 0;
	virtual bool LeaveInterestBatch(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldEntityId> EntityIds,
		FString& OutError) = 0;
	virtual bool RollbackRestoreBatch(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldEntityId> EntityIds,
		FString& OutError) = 0;
	virtual bool CanRuntimeEvict(FWorldEntityId EntityId) const { return EntityId.IsSet(); }
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldFragmentPersistenceKey final
{
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
	FName StructPath = NAME_None;

	friend bool operator==(const FWorldFragmentPersistenceKey& Left, const FWorldFragmentPersistenceKey& Right)
	{
		return Left.Domain == Right.Domain && Left.StructPath == Right.StructPath;
	}

	friend uint32 GetTypeHash(const FWorldFragmentPersistenceKey& Key)
	{
		return HashCombine(
			::GetTypeHash(static_cast<uint8>(Key.Domain)),
			Key.StructPath.GetComparisonIndex().ToUnstableInt());
	}
};
