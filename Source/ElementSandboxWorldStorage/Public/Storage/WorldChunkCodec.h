#pragma once

#include "CoreMinimal.h"
#include "Chunk/WorldChunkTypes.h"

/** Chunk 二进制格式：Definition 分组、ID 差值、默认值省略与 Chunk 相对量化 Transform。 */
class ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkCodec final
{
public:
	static constexpr uint16 FormatVersion = 1;
	static constexpr uint16 CodecVersion = 2;

	/** 纯值：采用与 Chunk 往返相同的位置、旋转和缩放精度，供需要提前确定持久终态的调用方使用。 */
	static FTransform QuantizeTransform(const FTransform& Transform);
	static bool Encode(const FWorldChunkData& Chunk, TArray<uint8>& OutBytes, FString& OutError);
	static bool Decode(TConstArrayView<uint8> Bytes, FWorldChunkData& OutChunk, FString& OutError);
	static bool Compress(const FWorldChunkData& Chunk, FWorldCompressedChunk& OutChunk, FString& OutError);
	static bool Decompress(const FWorldCompressedChunk& Compressed, FWorldChunkData& OutChunk, FString& OutError);
	static FWorldChunkContentHash Hash(TConstArrayView<uint8> Bytes);
};
