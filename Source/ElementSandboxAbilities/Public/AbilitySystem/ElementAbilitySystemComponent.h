#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/ElementAbilitySet.h"
#include "ElementAbilitySystemComponent.generated.h"

/**
 * 项目的 GAS 入口：使用 GameplayTag 驱动输入，并为外部来源提供成组 grant/revoke。
 * 组件不认识背包、装备、木棍或世界元素系统。
 */
UCLASS()
class ELEMENTSANDBOXABILITIES_API UElementAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UElementAbilitySystemComponent();

	/** 本地输入入口；匹配 AbilitySpec 的动态 InputTag，并走 GAS 原生预测激活。 */
	void AbilityInputTagPressed(FGameplayTag InputTag);
	void AbilityInputTagReleased(FGameplayTag InputTag);

	/**
	 * 仅服务器调用。整组配置先完成校验再授予，返回值必须由来源持有并用于精确回收。
	 * SourceObject 会写入 AbilitySpec，供 Ability 在服务器重新验证装备来源。
	 */
	TArray<FGameplayAbilitySpecHandle> GrantAbilitySet(
		const FElementAbilitySet& AbilitySet,
		UObject* SourceObject);

	/** 仅服务器调用；取消仍在执行的 Ability 后清除全部 Spec，并清空传入数组。 */
	void RevokeAbilitySet(TArray<FGameplayAbilitySpecHandle>& GrantedHandles);

#if WITH_DEV_AUTOMATION_TESTS
	/** 只供黑盒测试触发一个真实 GAS 周期，不进入生产 Gameplay 调用路径。 */
	void ExecutePeriodicEffectForAutomation(FActiveGameplayEffectHandle Handle)
	{
		ExecutePeriodicEffect(Handle);
	}
#endif

private:
	void ForwardReplicatedInputEvent(
		EAbilityGenericReplicatedEvent::Type EventType,
		const FGameplayAbilitySpec& AbilitySpec);
};
