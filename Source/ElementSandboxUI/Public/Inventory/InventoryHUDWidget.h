#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InventoryHUDWidget.generated.h"

class UInventoryBackpackWidget;
class UInventoryComponent;
class UInventoryQuickbarWidget;

/** 组合快捷栏与背包视图，不拥有背包业务状态。 */
UCLASS()
class ELEMENTSANDBOXUI_API UInventoryHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetInventory(UInventoryComponent* InInventory);
	void SetBackpackOpen(bool bOpen);
	bool IsBackpackOpen() const { return bBackpackOpen; }

protected:
	virtual void NativeOnInitialized() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInventoryQuickbarWidget> QuickbarWidget;

	UPROPERTY(Transient)
	TObjectPtr<UInventoryBackpackWidget> BackpackWidget;

	bool bBackpackOpen = false;
};
