#pragma once

#include "CoreMinimal.h"
#include "Chunk/WorldChunkCoordinates.h"
#include "Entity/WorldEntityId.h"

#include "WorldChunkTypes.generated.h"

UENUM()
enum class EWorldEntityDomain : uint8
{
	Invalid,
	Building,
	WorldObject,
	Element,
	Character
};

USTRUCT()
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkContentHash final
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 High = 0;

	UPROPERTY()
	uint64 Low = 0;

	bool IsSet() const { return High != 0 || Low != 0; }

	friend bool operator==(const FWorldChunkContentHash& Left, const FWorldChunkContentHash& Right)
	{
		return Left.High == Right.High && Left.Low == Right.Low;
	}

	friend bool operator!=(const FWorldChunkContentHash& Left, const FWorldChunkContentHash& Right)
	{
		return !(Left == Right);
	}
};

/** Codec 与领域适配器交换的显式持久化记录；Payload 只包含该领域稳定状态。 */
USTRUCT()
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldPersistentEntityRecord final
{
	GENERATED_BODY()

	UPROPERTY()
	FWorldEntityId EntityId;

	UPROPERTY()
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;

	UPROPERTY()
	FName DefinitionId = NAME_None;

	UPROPERTY()
	FTransform WorldTransform = FTransform::Identity;

	UPROPERTY()
	uint32 StateRevision = 1;

	UPROPERTY()
	TArray<uint8> Payload;

	bool IsValid() const;
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkData final
{
	FWorldChunkCoord Coord;
	uint32 Revision = 1;
	TArray<FWorldPersistentEntityRecord> Records;

	bool IsValid() const;
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldCompressedChunk final
{
	FWorldChunkCoord Coord;
	uint32 Revision = 0;
	FWorldChunkContentHash ContentHash;
	int32 UncompressedSize = 0;
	TArray<uint8> Bytes;
	int32 BuildingEntityCount = 0;
	int32 WorldObjectEntityCount = 0;
	int32 ElementEntityCount = 0;

	bool IsValid() const;
};

USTRUCT()
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkOffer final
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid WorldId;

	UPROPERTY()
	FWorldChunkCoord Coord;

	UPROPERTY()
	uint32 Revision = 0;

	UPROPERTY()
	FWorldChunkContentHash ContentHash;

	UPROPERTY()
	int32 CompressedSize = 0;

	UPROPERTY()
	int32 UncompressedSize = 0;

	/** 由 Authority 按当前 Pawn 中心判定；Client 不用本地预测位置重新解释启动优先级。 */
	UPROPERTY()
	bool bActivationCore = false;
};

UENUM()
enum class EWorldChunkClientResponse : uint8
{
	Have,
	Request
};

USTRUCT()
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkOfferResponse final
{
	GENERATED_BODY()

	UPROPERTY()
	FWorldChunkCoord Coord;

	UPROPERTY()
	uint32 Revision = 0;

	UPROPERTY()
	EWorldChunkClientResponse Response = EWorldChunkClientResponse::Request;
};

USTRUCT()
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkPayloadSegment final
{
	GENERATED_BODY()

	UPROPERTY()
	FWorldChunkOffer Offer;

	UPROPERTY()
	int32 SegmentIndex = 0;

	UPROPERTY()
	int32 SegmentCount = 0;

	UPROPERTY()
	TArray<uint8> Bytes;
};

enum class EWorldStorageMutationKind : uint8
{
	Upsert,
	Move,
	GameplayTombstone
};

/** Authority 内部事件；网络层据订阅关系投影成 Live Delta。 */
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageEntityMutation final
{
	EWorldStorageMutationKind Kind = EWorldStorageMutationKind::Upsert;
	FWorldEntityId EntityId;
	FWorldChunkCoord PreviousChunk;
	FWorldChunkCoord CurrentChunk;
	uint32 StateRevision = 0;
};

UENUM()
enum class EWorldChunkLiveDeltaKind : uint8
{
	Upsert,
	ProjectionRemove,
	GameplayTombstone
};

/** Snapshot ACK 之后、仅向仍订阅相关 Chunk 的 Owner Client 发送。 */
USTRUCT()
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkLiveDelta final
{
	GENERATED_BODY()

	UPROPERTY()
	EWorldChunkLiveDeltaKind Kind = EWorldChunkLiveDeltaKind::Upsert;

	UPROPERTY()
	FWorldChunkCoord ChunkCoord;

	UPROPERTY()
	FWorldEntityId EntityId;

	UPROPERTY()
	uint32 StateRevision = 0;

	UPROPERTY()
	FWorldPersistentEntityRecord Record;
};
