#include "Storage/WorldChunkCodec.h"

#include "Algo/Sort.h"
#include "Misc/Compression.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr uint32 ChunkMagic = 0x4b484357; // WCHK
	constexpr int32 MaximumChunkRecordCount = 1000000;
	constexpr int32 MaximumDefinitionCount = 65535;
	constexpr int32 MaximumPayloadBytes = 64 * 1024 * 1024;
	constexpr double PositionQuantizationSteps = static_cast<double>(MAX_uint16) + 1.0;

	struct FSortedRecordRef final
	{
		const FWorldPersistentEntityRecord* Record = nullptr;
	};

	bool RecordLess(const FSortedRecordRef& Left, const FSortedRecordRef& Right)
	{
		if (Left.Record->Domain != Right.Record->Domain)
		{
			return static_cast<uint8>(Left.Record->Domain) < static_cast<uint8>(Right.Record->Domain);
		}
		const int32 DefinitionCompare = Left.Record->DefinitionId.Compare(Right.Record->DefinitionId);
		return DefinitionCompare != 0
			? DefinitionCompare < 0
			: Left.Record->EntityId < Right.Record->EntityId;
	}

	uint16 QuantizePosition(const double Value)
	{
		const double Normalized = Value / FWorldChunkCoord::EdgeCentimeters;
		const int32 Quantized = FMath::RoundToInt(Normalized * PositionQuantizationSteps);
		return static_cast<uint16>(FMath::Clamp(Quantized, 0, static_cast<int32>(MAX_uint16)));
	}

	double DequantizePosition(const uint16 Value)
	{
		// 65536 个量化格覆盖半开区间 [0, Edge)，最大码也绝不会解码到相邻 Chunk。
		return static_cast<double>(Value) * FWorldChunkCoord::EdgeCentimeters / PositionQuantizationSteps;
	}

	int16 QuantizeQuaternion(const double Value)
	{
		return static_cast<int16>(FMath::RoundToInt(FMath::Clamp(Value, -1.0, 1.0) * MAX_int16));
	}

	double DequantizeQuaternion(const int16 Value)
	{
		return static_cast<double>(Value) / MAX_int16;
	}

	FQuat DecodeRotation(int16 X, int16 Y, int16 Z, int16 W)
	{
		// 量化后的四元数必须重新归一化，否则 HISM 从矩阵还原时会把误差混进缩放。
		return FQuat(DequantizeQuaternion(X), DequantizeQuaternion(Y),
			DequantizeQuaternion(Z), DequantizeQuaternion(W)).GetNormalized();
	}

	void WriteTransform(FArchive& Archive, const FWorldChunkCoord& Coord, const FTransform& Transform)
	{
		const FVector Relative = Transform.GetLocation() - Coord.GetWorldMinimum();
		uint16 PositionX = QuantizePosition(Relative.X);
		uint16 PositionY = QuantizePosition(Relative.Y);
		uint16 PositionZ = QuantizePosition(Relative.Z);
		Archive << PositionX << PositionY << PositionZ;

		const FQuat Rotation = Transform.GetRotation().GetNormalized();
		int16 RotationX = QuantizeQuaternion(Rotation.X);
		int16 RotationY = QuantizeQuaternion(Rotation.Y);
		int16 RotationZ = QuantizeQuaternion(Rotation.Z);
		int16 RotationW = QuantizeQuaternion(Rotation.W);
		Archive << RotationX << RotationY << RotationZ << RotationW;

		const FVector Scale = Transform.GetScale3D();
		uint8 HasNonDefaultScale = Scale.Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER) ? 0 : 1;
		Archive << HasNonDefaultScale;
		if (HasNonDefaultScale != 0)
		{
			float ScaleX = static_cast<float>(Scale.X);
			float ScaleY = static_cast<float>(Scale.Y);
			float ScaleZ = static_cast<float>(Scale.Z);
			Archive << ScaleX << ScaleY << ScaleZ;
		}
	}

	bool ReadTransform(FArchive& Archive, const FWorldChunkCoord& Coord, FTransform& OutTransform)
	{
		uint16 PositionX = 0;
		uint16 PositionY = 0;
		uint16 PositionZ = 0;
		Archive << PositionX << PositionY << PositionZ;
		int16 RotationX = 0;
		int16 RotationY = 0;
		int16 RotationZ = 0;
		int16 RotationW = 0;
		Archive << RotationX << RotationY << RotationZ << RotationW;
		uint8 HasNonDefaultScale = 0;
		Archive << HasNonDefaultScale;
		FVector Scale = FVector::OneVector;
		if (HasNonDefaultScale != 0)
		{
			float ScaleX = 1.0f;
			float ScaleY = 1.0f;
			float ScaleZ = 1.0f;
			Archive << ScaleX << ScaleY << ScaleZ;
			Scale = FVector(ScaleX, ScaleY, ScaleZ);
		}

		const FQuat Rotation = DecodeRotation(RotationX, RotationY, RotationZ, RotationW);
		const FVector Location = Coord.GetWorldMinimum() + FVector(
			DequantizePosition(PositionX),
			DequantizePosition(PositionY),
			DequantizePosition(PositionZ));
		OutTransform = FTransform(Rotation, Location, Scale);
		return !Archive.IsError() && !OutTransform.ContainsNaN();
	}
}

FTransform FWorldChunkCodec::QuantizeTransform(const FTransform& Transform)
{
	const FWorldChunkCoord Coord = FWorldChunkCoord::FromWorldLocation(Transform.GetLocation());
	const FVector Relative = Transform.GetLocation() - Coord.GetWorldMinimum();
	const FVector Location = Coord.GetWorldMinimum() + FVector(
		DequantizePosition(QuantizePosition(Relative.X)),
		DequantizePosition(QuantizePosition(Relative.Y)),
		DequantizePosition(QuantizePosition(Relative.Z)));
	const FQuat Rotation = Transform.GetRotation().GetNormalized();
	const FVector Scale = Transform.GetScale3D().Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER)
		? FVector::OneVector : FVector(FVector3f(Transform.GetScale3D()));
	return FTransform(DecodeRotation(QuantizeQuaternion(Rotation.X), QuantizeQuaternion(Rotation.Y),
		QuantizeQuaternion(Rotation.Z), QuantizeQuaternion(Rotation.W)), Location, Scale);
}

bool FWorldChunkCodec::Encode(const FWorldChunkData& Chunk, TArray<uint8>& OutBytes, FString& OutError)
{
	OutBytes.Reset();
	OutError.Reset();
	if (!Chunk.IsValid() || Chunk.Records.Num() > MaximumChunkRecordCount)
	{
		OutError = TEXT("Chunk 数据无效或记录数超过 v1 上限。");
		return false;
	}

	TArray<FSortedRecordRef> Sorted;
	Sorted.Reserve(Chunk.Records.Num());
	TArray<FName> Definitions;
	TSet<FName> SeenDefinitions;
	for (const FWorldPersistentEntityRecord& Record : Chunk.Records)
	{
		Sorted.Add({&Record});
		if (!SeenDefinitions.Contains(Record.DefinitionId))
		{
			SeenDefinitions.Add(Record.DefinitionId);
			Definitions.Add(Record.DefinitionId);
		}
	}
	Algo::Sort(Sorted, RecordLess);
	Algo::Sort(Definitions, [](const FName Left, const FName Right)
	{
		return Left.Compare(Right) < 0;
	});
	if (Definitions.Num() > MaximumDefinitionCount)
	{
		OutError = TEXT("单个 Chunk 的 Definition 字典超过 v1 上限。");
		return false;
	}
	TMap<FName, uint16> DefinitionIndices;
	DefinitionIndices.Reserve(Definitions.Num());
	for (int32 Index = 0; Index < Definitions.Num(); ++Index)
	{
		DefinitionIndices.Add(Definitions[Index], static_cast<uint16>(Index));
	}

		FMemoryWriter Writer(OutBytes, true);
		uint32 Magic = ChunkMagic;
		uint16 Version = FormatVersion;
		uint16 Codec = CodecVersion;
		Writer << Magic << Version << Codec;
		int32 ChunkX = Chunk.Coord.X;
		int32 ChunkY = Chunk.Coord.Y;
		int32 ChunkZ = Chunk.Coord.Z;
		Writer << ChunkX << ChunkY << ChunkZ;
	uint32 Revision = Chunk.Revision;
	uint32 RecordCount = Chunk.Records.Num();
	uint16 DefinitionCount = Definitions.Num();
	Writer << Revision << RecordCount << DefinitionCount;
	for (const FName Definition : Definitions)
	{
		FString DefinitionString = Definition.ToString();
		Writer << DefinitionString;
	}

	int32 GroupCountOffset = Writer.Tell();
	uint32 GroupCount = 0;
	Writer << GroupCount;
	int32 RecordIndex = 0;
	while (RecordIndex < Sorted.Num())
	{
		const FWorldPersistentEntityRecord& First = *Sorted[RecordIndex].Record;
		int32 GroupEnd = RecordIndex + 1;
		while (GroupEnd < Sorted.Num()
			&& Sorted[GroupEnd].Record->Domain == First.Domain
			&& Sorted[GroupEnd].Record->DefinitionId == First.DefinitionId)
		{
			++GroupEnd;
		}
		++GroupCount;
		uint8 Domain = static_cast<uint8>(First.Domain);
		uint16 DefinitionIndex = DefinitionIndices.FindChecked(First.DefinitionId);
		uint32 GroupRecordCount = GroupEnd - RecordIndex;
		Writer << Domain << DefinitionIndex << GroupRecordCount;

		uint64 PreviousEntityValue = 0;
		for (; RecordIndex < GroupEnd; ++RecordIndex)
		{
			const FWorldPersistentEntityRecord& Record = *Sorted[RecordIndex].Record;
			const uint64 CurrentEntityValue = Record.EntityId.GetValue();
			uint64 IdDelta = CurrentEntityValue - PreviousEntityValue;
			uint64 StateRevision = Record.StateRevision;
			uint64 PayloadSize = Record.Payload.Num();
			UE::ElementSandbox::WorldStorage::SerializePackedUint64(Writer, IdDelta);
			UE::ElementSandbox::WorldStorage::SerializePackedUint64(Writer, StateRevision);
			WriteTransform(Writer, Chunk.Coord, Record.WorldTransform);
			UE::ElementSandbox::WorldStorage::SerializePackedUint64(Writer, PayloadSize);
				if (PayloadSize > 0)
				{
					Writer.Serialize(const_cast<uint8*>(Record.Payload.GetData()), static_cast<int64>(PayloadSize));
				}
			PreviousEntityValue = CurrentEntityValue;
		}
	}
	const int64 EndOffset = Writer.Tell();
	Writer.Seek(GroupCountOffset);
	Writer << GroupCount;
	Writer.Seek(EndOffset);
	if (Writer.IsError())
	{
		OutBytes.Reset();
		OutError = TEXT("写入 Chunk 数据失败。");
		return false;
	}
	return true;
}

bool FWorldChunkCodec::Decode(const TConstArrayView<uint8> Bytes, FWorldChunkData& OutChunk, FString& OutError)
{
	OutChunk = {};
	OutError.Reset();
	if (Bytes.IsEmpty() || Bytes.Num() > MaximumPayloadBytes)
	{
		OutError = TEXT("Chunk Blob 为空或超过 v1 解码上限。");
		return false;
	}
	FMemoryReaderView Reader(Bytes, true);
	uint32 Magic = 0;
	uint16 Version = 0;
	uint16 Codec = 0;
	Reader << Magic << Version << Codec;
	if (Magic != ChunkMagic || Version != FormatVersion || Codec != CodecVersion)
	{
		OutError = FString::Printf(
			TEXT("Chunk 格式或 Codec 版本不匹配（实际 %u/%u，需要 %u/%u）；请重新生成种子存档。"),
			Version, Codec, FormatVersion, CodecVersion);
		return false;
	}
	Reader << OutChunk.Coord.X << OutChunk.Coord.Y << OutChunk.Coord.Z;
	uint32 RecordCount = 0;
	uint16 DefinitionCount = 0;
	Reader << OutChunk.Revision << RecordCount << DefinitionCount;
	if (OutChunk.Revision == 0 || RecordCount > MaximumChunkRecordCount || DefinitionCount > MaximumDefinitionCount)
	{
		OutError = TEXT("Chunk Header 数值超出 v1 上限。");
		return false;
	}
	TArray<FName> Definitions;
	Definitions.Reserve(DefinitionCount);
	for (uint16 Index = 0; Index < DefinitionCount; ++Index)
	{
		FString DefinitionString;
		Reader << DefinitionString;
		if (DefinitionString.IsEmpty())
		{
			OutError = TEXT("Chunk Definition 字典包含空项。");
			return false;
		}
		Definitions.Add(FName(*DefinitionString));
	}

	uint32 GroupCount = 0;
	Reader << GroupCount;
	if (GroupCount > RecordCount)
	{
		OutError = TEXT("Chunk Group 数超过记录数。");
		return false;
	}
	OutChunk.Records.Reserve(RecordCount);
	for (uint32 GroupIndex = 0; GroupIndex < GroupCount; ++GroupIndex)
	{
		uint8 DomainValue = 0;
		uint16 DefinitionIndex = 0;
		uint32 GroupRecordCount = 0;
		Reader << DomainValue << DefinitionIndex << GroupRecordCount;
		const EWorldEntityDomain Domain = static_cast<EWorldEntityDomain>(DomainValue);
		if (!Definitions.IsValidIndex(DefinitionIndex) || Domain == EWorldEntityDomain::Invalid
			|| Domain == EWorldEntityDomain::Character || GroupRecordCount > RecordCount - OutChunk.Records.Num())
		{
			OutError = TEXT("Chunk Group Header 无效。");
			return false;
		}

		uint64 PreviousEntityValue = 0;
		for (uint32 GroupRecordIndex = 0; GroupRecordIndex < GroupRecordCount; ++GroupRecordIndex)
		{
			uint64 IdDelta = 0;
			uint64 StateRevision = 0;
			UE::ElementSandbox::WorldStorage::SerializePackedUint64(Reader, IdDelta);
			UE::ElementSandbox::WorldStorage::SerializePackedUint64(Reader, StateRevision);
			if (IdDelta == 0 || PreviousEntityValue > MAX_uint64 - IdDelta || StateRevision == 0 || StateRevision > MAX_uint32)
			{
				OutError = TEXT("Chunk Entity ID 差值或 Revision 无效。");
				return false;
			}
			FWorldPersistentEntityRecord& Record = OutChunk.Records.AddDefaulted_GetRef();
			PreviousEntityValue += IdDelta;
			Record.EntityId = FWorldEntityId(PreviousEntityValue);
			Record.Domain = Domain;
			Record.DefinitionId = Definitions[DefinitionIndex];
			Record.StateRevision = static_cast<uint32>(StateRevision);
			if (!ReadTransform(Reader, OutChunk.Coord, Record.WorldTransform))
			{
				OutError = TEXT("Chunk Transform 解码失败。");
				return false;
			}
			uint64 PayloadSize = 0;
			UE::ElementSandbox::WorldStorage::SerializePackedUint64(Reader, PayloadSize);
			if (PayloadSize > static_cast<uint64>(MaximumPayloadBytes) || PayloadSize > static_cast<uint64>(Reader.TotalSize() - Reader.Tell()))
			{
				OutError = TEXT("Chunk Entity Payload 长度无效。");
				return false;
			}
			Record.Payload.SetNumUninitialized(static_cast<int32>(PayloadSize));
			if (PayloadSize > 0)
			{
				Reader.Serialize(Record.Payload.GetData(), PayloadSize);
			}
		}
	}
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize() || OutChunk.Records.Num() != static_cast<int32>(RecordCount)
		|| !OutChunk.IsValid())
	{
		OutChunk = {};
		OutError = TEXT("Chunk 数据未完整消费或一致性校验失败。");
		return false;
	}
	return true;
}

bool FWorldChunkCodec::Compress(const FWorldChunkData& Chunk, FWorldCompressedChunk& OutChunk, FString& OutError)
{
	OutChunk = {};
	TArray<uint8> Uncompressed;
	if (!Encode(Chunk, Uncompressed, OutError))
	{
		return false;
	}
	const int64 Bound = FCompression::CompressMemoryBound(NAME_Zlib, Uncompressed.Num());
	if (Bound <= 0 || Bound > MAX_int32)
	{
		OutError = TEXT("无法计算 Chunk 压缩缓冲大小。");
		return false;
	}
	OutChunk.Bytes.SetNumUninitialized(static_cast<int32>(Bound));
	int64 CompressedSize = Bound;
	if (!FCompression::CompressMemory(
			NAME_Zlib,
			OutChunk.Bytes.GetData(),
			CompressedSize,
			Uncompressed.GetData(),
			Uncompressed.Num(),
			COMPRESS_BiasSpeed))
	{
		OutChunk = {};
		OutError = TEXT("Chunk Zlib 压缩失败。");
		return false;
	}
	OutChunk.Bytes.SetNum(static_cast<int32>(CompressedSize), EAllowShrinking::Yes);
	OutChunk.Coord = Chunk.Coord;
	OutChunk.Revision = Chunk.Revision;
	OutChunk.UncompressedSize = Uncompressed.Num();
	OutChunk.ContentHash = Hash(OutChunk.Bytes);
	for (const FWorldPersistentEntityRecord& Record : Chunk.Records)
	{
		switch (Record.Domain)
		{
		case EWorldEntityDomain::Building: ++OutChunk.BuildingEntityCount; break;
		case EWorldEntityDomain::WorldObject: ++OutChunk.WorldObjectEntityCount; break;
		case EWorldEntityDomain::Element: ++OutChunk.ElementEntityCount; break;
		default: break;
		}
	}
	return OutChunk.IsValid();
}

bool FWorldChunkCodec::Decompress(const FWorldCompressedChunk& Compressed, FWorldChunkData& OutChunk, FString& OutError)
{
	OutChunk = {};
	OutError.Reset();
	if (!Compressed.IsValid() || Hash(Compressed.Bytes) != Compressed.ContentHash
		|| Compressed.UncompressedSize > MaximumPayloadBytes)
	{
		OutError = TEXT("压缩 Chunk 元数据或 ContentHash 无效。");
		return false;
	}
	TArray<uint8> Uncompressed;
	Uncompressed.SetNumUninitialized(Compressed.UncompressedSize);
	if (!FCompression::UncompressMemory(
			NAME_Zlib,
			Uncompressed.GetData(),
			Uncompressed.Num(),
			Compressed.Bytes.GetData(),
			Compressed.Bytes.Num()))
	{
		OutError = TEXT("Chunk Zlib 解压失败。");
		return false;
	}
	if (!Decode(Uncompressed, OutChunk, OutError) || OutChunk.Coord != Compressed.Coord
		|| OutChunk.Revision != Compressed.Revision)
	{
		OutChunk = {};
		if (OutError.IsEmpty())
		{
			OutError = TEXT("压缩 Chunk Header 与 TOC 元数据不一致。");
		}
		return false;
	}
	return true;
}

FWorldChunkContentHash FWorldChunkCodec::Hash(const TConstArrayView<uint8> Bytes)
{
	uint8 Digest[FSHA1::DigestSize] = {};
	if (!Bytes.IsEmpty())
	{
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
	}
	FWorldChunkContentHash Result;
	FMemory::Memcpy(&Result.High, Digest, sizeof(Result.High));
	FMemory::Memcpy(&Result.Low, Digest + sizeof(Result.High), sizeof(Result.Low));
	return Result;
}
