#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "ElementCharacterAttributeSet.generated.h"

#define ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/** 玩家和假人共享的最小角色属性；元素世界物件不创建此 AttributeSet。 */
UCLASS()
class ELEMENTSANDBOXABILITIES_API UElementCharacterAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UElementCharacterAttributeSet();

	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Health, Category="Vitals")
	FGameplayAttributeData Health;
	ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(UElementCharacterAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_MaxHealth, Category="Vitals")
	FGameplayAttributeData MaxHealth;
	ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(UElementCharacterAttributeSet, MaxHealth)

	/**
	 * 所有伤害 Execution 的统一输出元属性。它只在服务器的一次 Effect 执行中暂存数值，
	 * 随即由 PostGameplayEffectExecute 扣除 Health 并清零，因此不复制也不表示稳定状态。
	 */
	UPROPERTY(BlueprintReadOnly, Category="Vitals")
	FGameplayAttributeData IncomingDamage;
	ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(UElementCharacterAttributeSet, IncomingDamage)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Stamina, Category="Vitals")
	FGameplayAttributeData Stamina;
	ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(UElementCharacterAttributeSet, Stamina)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_MaxStamina, Category="Vitals")
	FGameplayAttributeData MaxStamina;
	ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(UElementCharacterAttributeSet, MaxStamina)

	/** 归一化抗性，0 表示无减免，1 表示完全免疫；具体伤害公式由 ExecutionCalculation 决定。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_FireResistance, Category="Resistance")
	FGameplayAttributeData FireResistance;
	ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(UElementCharacterAttributeSet, FireResistance)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_IceResistance, Category="Resistance")
	FGameplayAttributeData IceResistance;
	ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS(UElementCharacterAttributeSet, IceResistance)

private:
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& PreviousValue);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& PreviousValue);

	UFUNCTION()
	void OnRep_Stamina(const FGameplayAttributeData& PreviousValue);

	UFUNCTION()
	void OnRep_MaxStamina(const FGameplayAttributeData& PreviousValue);

	UFUNCTION()
	void OnRep_FireResistance(const FGameplayAttributeData& PreviousValue);

	UFUNCTION()
	void OnRep_IceResistance(const FGameplayAttributeData& PreviousValue);
};

#undef ELEMENTSANDBOX_ATTRIBUTE_ACCESSORS
