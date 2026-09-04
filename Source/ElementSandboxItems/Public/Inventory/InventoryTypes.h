#pragma once

#include "CoreMinimal.h"

#include "InventoryTypes.generated.h"

/** 当前固定槽位所属的库存区域。 */
UENUM()
enum class EInventoryContainer : uint8
{
	Quickbar,
	Backpack
};

/** 一个固定库存槽位的稳定地址，不包含可持久化 ID 或客户端可信对象引用。 */
USTRUCT()
struct ELEMENTSANDBOXITEMS_API FInventorySlotAddress
{
	GENERATED_BODY()

	UPROPERTY()
	EInventoryContainer Container = EInventoryContainer::Backpack;

	UPROPERTY()
	int32 Index = INDEX_NONE;

	FInventorySlotAddress() = default;
	FInventorySlotAddress(const EInventoryContainer InContainer, const int32 InIndex)
		: Container(InContainer)
		, Index(InIndex)
	{
	}

	bool operator==(const FInventorySlotAddress& Other) const
	{
		return Container == Other.Container && Index == Other.Index;
	}
};
