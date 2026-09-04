#pragma once

#include "CoreMinimal.h"

#include "WorldChunkCoordinates.generated.h"

/** 三维逻辑 Chunk 坐标；每格固定 100m。 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkCoord final
{
	GENERATED_BODY()

	static constexpr double EdgeCentimeters = 10000.0;

	UPROPERTY()
	int32 X = 0;

	UPROPERTY()
	int32 Y = 0;

	UPROPERTY()
	int32 Z = 0;

	FWorldChunkCoord() = default;
	FWorldChunkCoord(const int32 InX, const int32 InY, const int32 InZ) : X(InX), Y(InY), Z(InZ) {}

	static FWorldChunkCoord FromWorldLocation(const FVector& Location);
	FVector GetWorldMinimum() const;

	friend bool operator==(const FWorldChunkCoord& Left, const FWorldChunkCoord& Right)
	{
		return Left.X == Right.X && Left.Y == Right.Y && Left.Z == Right.Z;
	}

	friend bool operator!=(const FWorldChunkCoord& Left, const FWorldChunkCoord& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FWorldChunkCoord& Left, const FWorldChunkCoord& Right)
	{
		return Left.X != Right.X ? Left.X < Right.X : Left.Y != Right.Y ? Left.Y < Right.Y : Left.Z < Right.Z;
	}

	friend uint32 GetTypeHash(const FWorldChunkCoord& Coord)
	{
		return HashCombine(HashCombine(::GetTypeHash(Coord.X), ::GetTypeHash(Coord.Y)), ::GetTypeHash(Coord.Z));
	}
};

/** 物理 Pack 坐标；每格聚合 10x10x10 个逻辑 Chunk。 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkPackCoord final
{
	GENERATED_BODY()

	static constexpr int32 ChunksPerEdge = 10;

	UPROPERTY()
	int32 X = 0;

	UPROPERTY()
	int32 Y = 0;

	UPROPERTY()
	int32 Z = 0;

	FWorldChunkPackCoord() = default;
	FWorldChunkPackCoord(const int32 InX, const int32 InY, const int32 InZ) : X(InX), Y(InY), Z(InZ) {}

	static FWorldChunkPackCoord FromChunk(const FWorldChunkCoord& Chunk);
	static int32 GetLocalChunkIndex(const FWorldChunkCoord& Chunk);

	friend bool operator==(const FWorldChunkPackCoord& Left, const FWorldChunkPackCoord& Right)
	{
		return Left.X == Right.X && Left.Y == Right.Y && Left.Z == Right.Z;
	}

	friend bool operator!=(const FWorldChunkPackCoord& Left, const FWorldChunkPackCoord& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FWorldChunkPackCoord& Left, const FWorldChunkPackCoord& Right)
	{
		return Left.X != Right.X ? Left.X < Right.X : Left.Y != Right.Y ? Left.Y < Right.Y : Left.Z < Right.Z;
	}

	friend uint32 GetTypeHash(const FWorldChunkPackCoord& Coord)
	{
		return HashCombine(HashCombine(::GetTypeHash(Coord.X), ::GetTypeHash(Coord.Y)), ::GetTypeHash(Coord.Z));
	}
};

/** 半开 Chunk Box：[Minimum, MaximumExclusive)。 */
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkBox final
{
	FWorldChunkCoord Minimum;
	FWorldChunkCoord MaximumExclusive;

	static FWorldChunkBox Centered(const FWorldChunkCoord& Center, int32 EdgeChunks);
	bool Contains(const FWorldChunkCoord& Coord) const;
};
