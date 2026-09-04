#pragma once

#include "CoreMinimal.h"
#include "Item/ItemFeature.h"
#include "ItemDisplayFeature.generated.h"

class UTexture2D;

/** 道具 UI 所需的展示数据；不参与任何 Gameplay 判断。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class ELEMENTSANDBOXITEMS_API UItemDisplayFeature : public UItemFeature
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_DisplayData, Category="Display")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_DisplayData, Category="Display")
	TSoftObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_DisplayData, Category="Display")
	FLinearColor Tint = FLinearColor::White;

private:
	UFUNCTION()
	void OnRep_DisplayData();
};
