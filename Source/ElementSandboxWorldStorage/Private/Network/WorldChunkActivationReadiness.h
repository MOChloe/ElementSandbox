#pragma once

#include "Chunk/WorldChunkTypes.h"
#include "CoreMinimal.h"

namespace UE::ElementSandbox::WorldStorage::Private
{
	/**
	 * Activation Core 是可玩门禁，不允许用空集合的 0 == 0 冒充完成。
	 * Authority 必须先打开世界、完成 Client 端点握手，并完成全部 Core 基线。
	 */
	inline bool IsActivationCoreGateComplete(
		const bool bStorageReady,
		const bool bClientEndpointReady,
		const int32 CoreChunkCount,
		const int32 AcknowledgedCoreChunkCount,
		const int32 AuthorityReadyCoreChunkCount)
	{
		return bStorageReady
			&& bClientEndpointReady
			&& CoreChunkCount > 0
			&& AcknowledgedCoreChunkCount == CoreChunkCount
			&& AuthorityReadyCoreChunkCount == CoreChunkCount;
	}

	/** 首次可玩门禁是单向闩锁；当前兴趣 Core 的后台迁移进度不能把已进入游戏的玩家重新踢回加载态。 */
	inline bool IsInitialActivationGateSatisfied(
		const bool bPreviouslySatisfied,
		const bool bCurrentCoreComplete)
	{
		return bPreviouslySatisfied || bCurrentCoreComplete;
	}

	/**
	 * 交互距离最多跨越相邻 Chunk；固定预建 3x3x3 Snapshot 基线，包含空 Chunk。
	 * 这样第一次在空地创建 Entity 时已经存在 ACK 订阅，可以直接发送 Live Delta。
	 */
	inline void BuildDenseActivationCore(
		const FWorldChunkCoord& Center,
		TArray<FWorldChunkCoord>& OutCoreCoords)
	{
		OutCoreCoords.Reset(27);
		for (int32 Z = -1; Z <= 1; ++Z)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 X = -1; X <= 1; ++X)
				{
					OutCoreCoords.Emplace(Center.X + X, Center.Y + Y, Center.Z + Z);
				}
			}
		}
	}
}
