#pragma once

#include "CoreMinimal.h"
#include "Item/ItemFeature.h"
#include "ItemStackFeature.generated.h"

/** Feature 的存在表示该道具允许按同一 Definition 合并。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class ELEMENTSANDBOXITEMS_API UItemStackFeature : public UItemFeature
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	int32 GetQuantity() const { return Quantity; }
	int32 GetMaxStackSize() const { return MaxStackSize; }
	int32 GetAvailableSpace() const { return FMath::Max(0, MaxStackSize - Quantity); }

	/** 服务器初始化或合并堆叠时调用，数量始终被约束在 [1, MaxStackSize]。 */
	void SetQuantity(int32 InQuantity);

	/** 返回实际加入的数量。 */
	int32 AddQuantity(int32 Amount);

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_StackData, Category="Stack", meta=(ClampMin="1"))
	int32 MaxStackSize = 1;

private:
	UPROPERTY(ReplicatedUsing=OnRep_StackData)
	int32 Quantity = 1;

	UFUNCTION()
	void OnRep_StackData();
};
