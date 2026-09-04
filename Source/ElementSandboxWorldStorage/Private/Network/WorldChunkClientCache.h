#pragma once

#include "CoreMinimal.h"
#include "Chunk/WorldChunkTypes.h"

namespace UE::ElementSandbox::WorldStorage::Private
{
	class FWorldChunkClientCache final
	{
	public:
		static FString MakeFilename(const FString& Root, const FWorldChunkCoord& Coord);
		static bool Load(
			const FString& Root,
			const FWorldChunkOffer& Offer,
			FWorldCompressedChunk& OutChunk);
		static bool Save(
			const FString& Root,
			const FGuid& WorldId,
			const FWorldCompressedChunk& Chunk);
	};

	enum class EWorldChunkSegmentAcceptResult : uint8
	{
		Rejected,
		Duplicate,
		Accepted,
		Completed
	};

	/** 纯值、可乱序接收的单 Chunk 分段装配器。可靠 RPC 仍需显式 ACK 控制发送窗口。 */
	class FWorldChunkSegmentAssembly final
	{
	public:
		EWorldChunkSegmentAcceptResult Accept(
			const FWorldChunkPayloadSegment& Segment,
			int32 SegmentPayloadBytes);
		bool IsComplete() const;
			/** 完成后把连续缓冲区直接移交给 Chunk，不再在 RPC GameThread 复制整块 Payload。 */
			bool Build(FWorldCompressedChunk& OutChunk);
		const FWorldChunkOffer& GetOffer() const { return Offer; }

	private:
		FWorldChunkOffer Offer;
		TArray<uint8> Bytes;
		TBitArray<> Received;
		bool bInitialized = false;
	};
}
