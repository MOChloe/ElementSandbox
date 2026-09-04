#include "Visual/ElementVisualShardKey.h"

FElementVisualShardKey FElementVisualShardKey::FromWorldLocation(
	const FVector& WorldLocation,
	const double ShardSize)
{
	FElementVisualShardKey Result;
	if (WorldLocation.ContainsNaN() || !FMath::IsFinite(ShardSize) || ShardSize <= UE_DOUBLE_SMALL_NUMBER)
	{
		return Result;
	}
	Result.Coordinates = FIntVector(
		FMath::FloorToInt(WorldLocation.X / ShardSize),
		FMath::FloorToInt(WorldLocation.Y / ShardSize),
		FMath::FloorToInt(WorldLocation.Z / ShardSize));
	return Result;
}
