#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Inventory/InventoryTypes.h"
#include "Item/InventoryItemDefinition.h"
#include "InventoryComponent.generated.h"

class APawn;
class AEquippedItemActor;
struct FInventoryAdditionReceipt;
class UItemInstance;
struct FInventoryConsumptionReceipt;

DECLARE_MULTICAST_DELEGATE(FOnInventoryChanged);

/** 一次原子的权威快捷栏选择；Revision 同时承担客户端预测确认。 */
USTRUCT()
struct FQuickbarSelectionState final
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Index = INDEX_NONE;

	UPROPERTY()
	uint32 Revision = 0;
};

/**
 * PlayerState 持有的服务器权威背包。快捷栏与背包均是固定槽位，直接保存 ItemInstance UObject。
 */
UCLASS(ClassGroup=(Inventory), meta=(BlueprintSpawnableComponent))
class ELEMENTSANDBOXITEMS_API UInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	static constexpr int32 QuickbarSlotCount = 10;
	static constexpr int32 BackpackSlotCount = 30;

	UInventoryComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void ReadyForReplication() override;

	const TArray<TObjectPtr<UItemInstance>>& GetQuickbarSlots() const { return QuickbarSlots; }
	const TArray<TObjectPtr<UItemInstance>>& GetBackpackSlots() const { return BackpackSlots; }
	UItemInstance* GetItem(const FInventorySlotAddress& Address) const;
	int32 GetSelectedQuickbarIndex() const
	{
		return PredictedQuickbarSelection.Revision > QuickbarSelection.Revision
			? PredictedQuickbarSelection.Index
			: QuickbarSelection.Index;
	}

	/**
	 * 仅服务器调用。先合并同 Definition 的可堆叠实例，再占用空槽；返回实际加入数量。
	 */
	int32 AddItem(
		TScriptInterface<IInventoryItemDefinition> Definition,
		int32 Quantity = 1,
		EInventoryContainer PreferredContainer = EInventoryContainer::Backpack);

	/**
	 * 仅服务器调用。立即应用一次可回滚的增加，并记录所有被扩充的堆叠和新建槽位；
	 * 调用方必须在同一同步调用栈内 Commit 或 Rollback。
	 */
	bool BeginAddItem(
		TScriptInterface<IInventoryItemDefinition> Definition,
		int32 Quantity,
		EInventoryContainer PreferredContainer,
		FInventoryAdditionReceipt& OutReceipt,
		int32& OutAddedQuantity);
	bool CommitItemAddition(FInventoryAdditionReceipt& Receipt);
	bool RollbackItemAddition(FInventoryAdditionReceipt& Receipt);

	/** 只读容量校验；与 AddItem 使用相同的堆叠和空槽规则。 */
	bool CanAddItem(
		TScriptInterface<IInventoryItemDefinition> Definition,
		int32 Quantity = 1) const;

	/** 仅服务器用于默认装备等确定性初始化；目标格非空时拒绝，保证重复发放幂等。 */
	bool GrantItemToQuickbar(
		TScriptInterface<IInventoryItemDefinition> Definition,
		int32 QuickbarIndex,
		int32 Quantity = 1);

	/** 仅服务器调用；同类堆叠优先合并，否则交换两个固定槽位。 */
	bool MoveItem(const FInventorySlotAddress& From, const FInventorySlotAddress& To);

	/** 本地 UI 提交源/目标槽位；客户端只发送地址，服务器重新校验并执行移动。 */
	void RequestMoveItem(const FInventorySlotAddress& From, const FInventorySlotAddress& To);

	/** 仅服务器调用；移除整个实例，不提供分堆语义。 */
	bool RemoveItem(const FInventorySlotAddress& Address);

	/**
	 * 仅服务器调用。扣除指定槽中的数量并生成同步回滚凭据；调用方必须随后 Commit
	 * 或 Rollback，不能把 Receipt 保存到下一帧。
	 */
	bool BeginConsumeItemQuantity(
		const FInventorySlotAddress& Address,
		int32 Quantity,
		FInventoryConsumptionReceipt& OutReceipt);
	bool CommitItemConsumption(FInventoryConsumptionReceipt& Receipt);
	bool RollbackItemConsumption(FInventoryConsumptionReceipt& Receipt);

	/**
	 * 仅供服务器调用。原子移除当前快捷栏中的装备实例、撤销装备关系，
	 * 并把既有 Equipped Actor 交给调用方而不销毁。
	 */
	bool ExtractSelectedEquippedItem(
		UItemInstance*& OutItemInstance,
		AEquippedItemActor*& OutEquippedActor);

	/** 本地玩家选择快捷栏槽位；服务器从权威槽位决定装备或收起，不触发道具 Use。 */
	void SelectQuickbarSlot(int32 QuickbarIndex);

	/** Item/Feature 的 OnRep 统一经此处触发 UI 刷新，不使用 Widget Tick。 */
	void NotifyItemDataChanged();

	FOnInventoryChanged& OnInventoryChanged() { return InventoryChangedEvent; }

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * 可靠 RPC，因为一次按键对应一次离散装备状态变更。非法索引、无 Pawn 或非权威槽位会被服务器拒绝。
	 */
	UFUNCTION(Server, Reliable)
	void ServerSelectQuickbarSlot(int32 QuickbarIndex, uint32 SelectionRevision);

	/**
	 * 拖放是一次离散槽位事务，因此使用 Reliable RPC。服务器拒绝非法地址、空源槽和同槽移动。
	 */
	UFUNCTION(Server, Reliable)
	void ServerMoveItem(FInventorySlotAddress From, FInventorySlotAddress To);

private:
	UPROPERTY(ReplicatedUsing=OnRep_Slots)
	TArray<TObjectPtr<UItemInstance>> QuickbarSlots;

	UPROPERTY(ReplicatedUsing=OnRep_Slots)
	TArray<TObjectPtr<UItemInstance>> BackpackSlots;

	UPROPERTY(ReplicatedUsing=OnRep_QuickbarSelection)
	FQuickbarSelectionState QuickbarSelection;

	/** 只供 owning client 的即时 UI/Placement 预测；权威 Revision 到达后自动失效。 */
	FQuickbarSelectionState PredictedQuickbarSelection;
	uint32 NextQuickbarSelectionRevision = 1;

	FOnInventoryChanged InventoryChangedEvent;

	UFUNCTION()
	void OnRep_Slots();

	UFUNCTION()
	void OnRep_QuickbarSelection();

	bool IsAddressValid(const FInventorySlotAddress& Address) const;
	TArray<TObjectPtr<UItemInstance>>* ResolveSlots(EInventoryContainer Container);
	const TArray<TObjectPtr<UItemInstance>>* ResolveSlots(EInventoryContainer Container) const;
	UItemInstance* CreateItemInstance(
		TScriptInterface<IInventoryItemDefinition> Definition,
		int32 Quantity);
	void RegisterItemForReplication(UItemInstance& ItemInstance);
	void UnregisterItemForReplication(UItemInstance& ItemInstance);
	void ClearSelectionIfMovedSlotWasSelected(
		const FInventorySlotAddress& From,
		const FInventorySlotAddress& To);
	void SelectQuickbarSlotAuthority(int32 QuickbarIndex, uint32 RequestedRevision = 0);
	void SetAuthorityQuickbarSelection(int32 QuickbarIndex, uint32 RequestedRevision = 0);
	APawn* GetOwningPawn() const;
};
