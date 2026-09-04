#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EquipmentComponent.generated.h"

class AEquippedItemActor;
class UEquippableItemFeature;
class UItemInstance;

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnEquippedItemChanged,
	UItemInstance* /* PreviousItem */,
	UItemInstance* /* NewItem */);

/** 服务器拥有角色当前装备的生命周期；客户端只接收装备 Actor 投影。 */
UCLASS(ClassGroup=(Inventory), meta=(BlueprintSpawnableComponent))
class ELEMENTSANDBOXITEMS_API UEquipmentComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEquipmentComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 仅服务器调用；相同 ItemInstance 已装备时直接成功，不重复 Spawn。 */
	bool EquipItem(UItemInstance* ItemInstance, const UEquippableItemFeature& EquippableFeature);

	/** 仅服务器调用；销毁旧投影并清空服务器侧 ItemInstance 引用。 */
	bool UnequipItem();

	/**
	 * 仅供服务器上的背包事务调用。清除当前装备关系并撤销装备事件，
	 * 但把既有 Actor 交还给调用方，不销毁它。
	 */
	bool ReleaseEquippedActor(
		UItemInstance* ExpectedItem,
		AEquippedItemActor*& OutReleasedActor);

	UItemInstance* GetCurrentEquippedItem() const { return CurrentEquippedItem; }
	AEquippedItemActor* GetEquippedActor() const { return EquippedActor; }
	FOnEquippedItemChanged& OnEquippedItemChanged() { return EquippedItemChangedEvent; }

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 只在服务器保存；背包 UObject 不向旁观客户端暴露。 */
	UPROPERTY(Transient)
	TObjectPtr<UItemInstance> CurrentEquippedItem;

	/** Actor 指针对所有相关客户端复制，具体附着关系由 Actor replication 同步。 */
	UPROPERTY(Replicated)
	TObjectPtr<AEquippedItemActor> EquippedActor;

	/** 仅在权威装备状态真正变化后广播；跨系统能力由装配层订阅。 */
	FOnEquippedItemChanged EquippedItemChangedEvent;
};
