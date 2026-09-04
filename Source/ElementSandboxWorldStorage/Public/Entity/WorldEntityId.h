#pragma once

#include "CoreMinimal.h"

#include "WorldEntityId.generated.h"

namespace UE::ElementSandbox::WorldStorage
{
	/** FArchive 只内建 uint32 packed；永久 ID 与 ID 差值使用受校验的 64 位 VarUInt。 */
	inline bool SerializePackedUint64(FArchive& Archive, uint64& Value)
	{
		if (Archive.IsSaving())
		{
			uint64 Remaining = Value;
			do
			{
				uint8 Byte = static_cast<uint8>(Remaining & 0x7f);
				Remaining >>= 7;
				Byte |= Remaining != 0 ? 0x80 : 0;
				Archive.Serialize(&Byte, 1);
			}
			while (Remaining != 0 && !Archive.IsError());
			return !Archive.IsError();
		}

		Value = 0;
		for (uint32 ByteIndex = 0; ByteIndex < 10; ++ByteIndex)
		{
			uint8 Byte = 0;
			Archive.Serialize(&Byte, 1);
			if (Archive.IsError() || (ByteIndex == 9 && Byte > 1))
			{
				Archive.SetError();
				return false;
			}
			Value |= static_cast<uint64>(Byte & 0x7f) << (ByteIndex * 7);
			if ((Byte & 0x80) == 0)
			{
				return true;
			}
		}
		Archive.SetError();
		return false;
	}
}

/**
 * 跨 ECS、存档与网络边界使用的永久身份。0 无效；Authority 单调分配且永不复用。
 * 各领域的 Generation Handle 仍只用于本地 Registry。
 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldEntityId final
{
	GENERATED_BODY()

public:
	FWorldEntityId() = default;
	explicit FWorldEntityId(const uint64 InValue) : Value(InValue) {}

	bool IsSet() const { return Value != 0; }
	uint64 GetValue() const { return Value; }

	bool NetSerialize(FArchive& Archive, UPackageMap* Map, bool& bOutSuccess)
	{
		(void)Map;
		UE::ElementSandbox::WorldStorage::SerializePackedUint64(Archive, Value);
		bOutSuccess = !Archive.IsError();
		return true;
	}

	friend FArchive& operator<<(FArchive& Archive, FWorldEntityId& Id)
	{
		UE::ElementSandbox::WorldStorage::SerializePackedUint64(Archive, Id.Value);
		return Archive;
	}

	friend bool operator==(const FWorldEntityId& Left, const FWorldEntityId& Right)
	{
		return Left.Value == Right.Value;
	}

	friend bool operator!=(const FWorldEntityId& Left, const FWorldEntityId& Right)
	{
		return !(Left == Right);
	}

	friend bool operator<(const FWorldEntityId& Left, const FWorldEntityId& Right)
	{
		return Left.Value < Right.Value;
	}

	friend uint32 GetTypeHash(const FWorldEntityId& Id)
	{
		return ::GetTypeHash(Id.Value);
	}

private:
	UPROPERTY()
	uint64 Value = 0;
};

template <>
struct TStructOpsTypeTraits<FWorldEntityId> : TStructOpsTypeTraitsBase2<FWorldEntityId>
{
	enum { WithNetSerializer = true };
};
