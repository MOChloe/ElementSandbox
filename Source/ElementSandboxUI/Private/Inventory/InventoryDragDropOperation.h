#pragma once

#include "CoreMinimal.h"
#include "Blueprint/DragDropOperation.h"
#include "Inventory/InventoryTypes.h"
#include "InventoryDragDropOperation.generated.h"

class UInventoryComponent;

/** 一次客户端槽位拖放的临时载荷；只保存源地址，不携带可被服务器信任的 Item 指针。 */
UCLASS()
class UInventoryDragDropOperation : public UDragDropOperation
{
	GENERATED_BODY()

public:
	UPROPERTY(Transient)
	TObjectPtr<UInventoryComponent> Inventory;

	UPROPERTY(Transient)
	FInventorySlotAddress SourceAddress;
};
