#pragma once

#include "CoreMinimal.h"
#include "Item/ItemFeature.h"

#include "DemolitionToolItemFeature.generated.h"

class UItemDefinition;

/** 一个可拆 Building Definition 与返还背包物品之间的显式装配关系。 */
USTRUCT()
struct FBuildingDismantleReward final
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Dismantle")
	FName BuildingDefinitionId = NAME_None;

	UPROPERTY(EditDefaultsOnly, Category="Dismantle")
	TObjectPtr<UItemDefinition> ItemDefinition = nullptr;

	UPROPERTY(EditDefaultsOnly, Category="Dismantle", meta=(ClampMin="1"))
	int32 Quantity = 1;

	/** true 时返还独立构件实例，并把目标 Entity 的 Rotation/Scale 写入 BuildingItemFeature。 */
	UPROPERTY(EditDefaultsOnly, Category="Dismantle")
	bool bPreservePlacementShape = false;

	bool IsValid() const;
};

/**
 * 拆除锤 ItemInstance 的跨域装配数据。实际目标校验、销毁与返还事务由项目 Authority Service 执行。
 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UDemolitionToolItemFeature final : public UItemFeature
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	bool TryResolveReward(
		FName BuildingDefinitionId,
		UItemDefinition*& OutItemDefinition,
		int32& OutQuantity) const;
	const FBuildingDismantleReward* FindReward(FName BuildingDefinitionId) const;

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_DismantleRewards, Category="Dismantle")
	TArray<FBuildingDismantleReward> Rewards;

private:
	UFUNCTION()
	void OnRep_DismantleRewards();
};
