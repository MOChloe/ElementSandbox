#pragma once

#include "CoreMinimal.h"
#include "Item/InventoryItemDefinition.h"
#include "UObject/Object.h"
#include "ItemInstance.generated.h"

class UItemFeature;

/**
 * 一个运行时道具实例，由所属 InventoryComponent 管理生命周期与复制。
 * 它通过少量 UObject Feature 组合能力。
 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXITEMS_API UItemInstance : public UObject
{
	GENERATED_BODY()

public:
	virtual UWorld* GetWorld() const override;
	virtual bool IsSupportedForNetworking() const override { return true; }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 从 Definition 复制 Feature 模板。失败时实例保持为空，不保留部分初始化状态。 */
	bool Initialize(TScriptInterface<IInventoryItemDefinition> InDefinition);

	const TScriptInterface<IInventoryItemDefinition>& GetDefinition() const { return Definition; }
	const TArray<TObjectPtr<UItemFeature>>& GetFeatures() const { return Features; }

	template <typename FeatureType>
	FeatureType* FindFeature()
	{
		for (UItemFeature* Feature : Features)
		{
			if (FeatureType* TypedFeature = Cast<FeatureType>(Feature))
			{
				return TypedFeature;
			}
		}
		return nullptr;
	}

	template <typename FeatureType>
	const FeatureType* FindFeature() const
	{
		for (const UItemFeature* Feature : Features)
		{
			if (const FeatureType* TypedFeature = Cast<FeatureType>(Feature))
			{
				return TypedFeature;
			}
		}
		return nullptr;
	}

	template <typename FeatureType>
	bool HasFeature() const
	{
		return FindFeature<FeatureType>() != nullptr;
	}

	/** 供 Feature 的 OnRep 将局部变化汇总到背包的单一刷新事件。 */
	void NotifyOwningInventoryChanged() const;

private:
	UPROPERTY(ReplicatedUsing=OnRep_ItemData)
	TScriptInterface<IInventoryItemDefinition> Definition;

	UPROPERTY(ReplicatedUsing=OnRep_ItemData)
	TArray<TObjectPtr<UItemFeature>> Features;

	UFUNCTION()
	void OnRep_ItemData();
};
