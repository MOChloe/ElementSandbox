#pragma once

#include "CoreMinimal.h"
#include "Item/ItemFeature.h"
#include "EquippableItemFeature.generated.h"

class AEquippedItemActor;

/** 把一个 ItemInstance 投影为角色手中的复制 Actor。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class ELEMENTSANDBOXITEMS_API UEquippableItemFeature : public UItemFeature
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_EquipmentData, Category="Equipment")
	TSubclassOf<AEquippedItemActor> EquippedActorClass;

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_EquipmentData, Category="Equipment")
	FName AttachmentSocket = TEXT("hand_r");

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_EquipmentData, Category="Equipment")
	FTransform AttachmentTransform = FTransform::Identity;

private:
	UFUNCTION()
	void OnRep_EquipmentData();
};
