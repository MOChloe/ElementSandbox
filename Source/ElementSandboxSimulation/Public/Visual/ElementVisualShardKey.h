#pragma once

#include "CoreMinimal.h"

/**
 * 只服务客户端/网络表现覆盖的空间页键。它不参与 Authority 查询、Relation 或调度，
 * 分片尺寸由表现配置独立决定。
 */
struct ELEMENTSANDBOXSIMULATION_API FElementVisualShardKey final
{
	static constexpr double DefaultSize = 10000.0;

	FIntVector Coordinates = FIntVector::ZeroValue;

	static FElementVisualShardKey FromWorldLocation(
		const FVector& WorldLocation,
		double ShardSize = DefaultSize);

	friend bool operator==(const FElementVisualShardKey& Left, const FElementVisualShardKey& Right)
	{
		return Left.Coordinates == Right.Coordinates;
	}

	friend bool operator!=(const FElementVisualShardKey& Left, const FElementVisualShardKey& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FElementVisualShardKey& Left, const FElementVisualShardKey& Right)
	{
		if (Left.Coordinates.X != Right.Coordinates.X) return Left.Coordinates.X < Right.Coordinates.X;
		if (Left.Coordinates.Y != Right.Coordinates.Y) return Left.Coordinates.Y < Right.Coordinates.Y;
		return Left.Coordinates.Z < Right.Coordinates.Z;
	}

	friend uint32 GetTypeHash(const FElementVisualShardKey& Key)
	{
		return GetTypeHash(Key.Coordinates);
	}
};
