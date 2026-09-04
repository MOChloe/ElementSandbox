#include "Chunk/WorldChunkCoordinates.h"

namespace
{
	int32 FloorDivide(const int32 Value, const int32 Divisor)
	{
		check(Divisor > 0);
		const int32 Quotient = Value / Divisor;
		const int32 Remainder = Value % Divisor;
		return Remainder < 0 ? Quotient - 1 : Quotient;
	}

	int32 FloorModulo(const int32 Value, const int32 Divisor)
	{
		const int32 Result = Value % Divisor;
		return Result < 0 ? Result + Divisor : Result;
	}
}

FWorldChunkCoord FWorldChunkCoord::FromWorldLocation(const FVector& Location)
{
	const auto ToChunkAxis = [](const double Axis)
	{
		const int64 ChunkAxis = FMath::FloorToInt64(Axis / EdgeCentimeters);
		checkf(ChunkAxis >= MIN_int32 && ChunkAxis <= MAX_int32, TEXT("World location is outside the supported Chunk coordinate range."));
		return static_cast<int32>(ChunkAxis);
	};
	return {
		ToChunkAxis(Location.X),
		ToChunkAxis(Location.Y),
		ToChunkAxis(Location.Z)
	};
}

FVector FWorldChunkCoord::GetWorldMinimum() const
{
	return FVector(X * EdgeCentimeters, Y * EdgeCentimeters, Z * EdgeCentimeters);
}

FWorldChunkPackCoord FWorldChunkPackCoord::FromChunk(const FWorldChunkCoord& Chunk)
{
	return {
		FloorDivide(Chunk.X, ChunksPerEdge),
		FloorDivide(Chunk.Y, ChunksPerEdge),
		FloorDivide(Chunk.Z, ChunksPerEdge)
	};
}

int32 FWorldChunkPackCoord::GetLocalChunkIndex(const FWorldChunkCoord& Chunk)
{
	const int32 LocalX = FloorModulo(Chunk.X, ChunksPerEdge);
	const int32 LocalY = FloorModulo(Chunk.Y, ChunksPerEdge);
	const int32 LocalZ = FloorModulo(Chunk.Z, ChunksPerEdge);
	return LocalX + ChunksPerEdge * (LocalY + ChunksPerEdge * LocalZ);
}

FWorldChunkBox FWorldChunkBox::Centered(const FWorldChunkCoord& Center, const int32 EdgeChunks)
{
	check(EdgeChunks > 0);
	const int32 NegativeHalf = EdgeChunks / 2;
	const int32 PositiveHalf = EdgeChunks - NegativeHalf;
	return {
		FWorldChunkCoord(Center.X - NegativeHalf, Center.Y - NegativeHalf, Center.Z - NegativeHalf),
		FWorldChunkCoord(Center.X + PositiveHalf, Center.Y + PositiveHalf, Center.Z + PositiveHalf)
	};
}

bool FWorldChunkBox::Contains(const FWorldChunkCoord& Coord) const
{
	return Coord.X >= Minimum.X && Coord.X < MaximumExclusive.X
		&& Coord.Y >= Minimum.Y && Coord.Y < MaximumExclusive.Y
		&& Coord.Z >= Minimum.Z && Coord.Z < MaximumExclusive.Z;
}
