#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "GameplayTagContainer.h"

#include "CharacterBurningPresentationComponent.generated.h"

class UAbilitySystemComponent;

/**
 * GAS Burning Tag 的纯表现投影。
 *
 * 组件不复制状态，也不读取 Character/Element ECS；服务器和客户端各自根据 ASC
 * 已复制的 State.Burning Tag 显示同一个占位火焰模型。
 */
UCLASS()
class UCharacterBurningPresentationComponent final : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UCharacterBurningPresentationComponent();

	/** ASC Avatar 建立或清除时调用；重复绑定同一 ASC 幂等。 */
	void InitializeAbilitySystem(UAbilitySystemComponent* NewAbilitySystem);

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

private:
	void HandleBurningTagChanged(FGameplayTag Tag, int32 NewCount);
	void UpdateBurningVisual(bool bBurning);

	TWeakObjectPtr<UAbilitySystemComponent> AbilitySystem;
	FDelegateHandle BurningTagChangedHandle;
};
