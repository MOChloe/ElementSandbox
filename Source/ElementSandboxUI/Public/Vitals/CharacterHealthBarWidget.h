#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CharacterHealthBarWidget.generated.h"

class UProgressBar;
class UTextBlock;

/** 只表现外部提交的角色生命值；不拥有 GAS 属性或伤害规则。 */
UCLASS()
class ELEMENTSANDBOXUI_API UCharacterHealthBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetHealth(float InHealth, float InMaxHealth);

	float GetDisplayedHealth() const { return DisplayedHealth; }
	float GetDisplayedMaxHealth() const { return DisplayedMaxHealth; }
	float GetDisplayedPercent() const;

protected:
	virtual void NativeOnInitialized() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UProgressBar> HealthProgressBar;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> HealthValueText;

	float DisplayedHealth = 0.0f;
	float DisplayedMaxHealth = 0.0f;

	void Refresh();
};
