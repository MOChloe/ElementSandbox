#include "Attributes/ElementCharacterAttributeSet.h"

#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

UElementCharacterAttributeSet::UElementCharacterAttributeSet()
{
	InitMaxHealth(100.0f);
	InitHealth(100.0f);
	InitIncomingDamage(0.0f);
	InitMaxStamina(100.0f);
	InitStamina(100.0f);
	InitFireResistance(0.0f);
	InitIceResistance(0.0f);
}

void UElementCharacterAttributeSet::PreAttributeChange(
	const FGameplayAttribute& Attribute,
	float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetMaxHealthAttribute() || Attribute == GetMaxStaminaAttribute())
	{
		NewValue = FMath::Max(1.0f, NewValue);
	}
	else if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	else if (Attribute == GetStaminaAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxStamina());
	}
	else if (Attribute == GetFireResistanceAttribute() || Attribute == GetIceResistanceAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
}

void UElementCharacterAttributeSet::PostGameplayEffectExecute(
	const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
	{
		const float Damage = FMath::Max(0.0f, GetIncomingDamage());
		SetIncomingDamage(0.0f);
		if (Damage > 0.0f)
		{
			SetHealth(FMath::Clamp(GetHealth() - Damage, 0.0f, GetMaxHealth()));
		}
	}
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute()
		|| Data.EvaluatedData.Attribute == GetMaxHealthAttribute())
	{
		SetHealth(FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));
	}
	else if (Data.EvaluatedData.Attribute == GetStaminaAttribute()
		|| Data.EvaluatedData.Attribute == GetMaxStaminaAttribute())
	{
		SetStamina(FMath::Clamp(GetStamina(), 0.0f, GetMaxStamina()));
	}
	else if (Data.EvaluatedData.Attribute == GetFireResistanceAttribute())
	{
		SetFireResistance(FMath::Clamp(GetFireResistance(), 0.0f, 1.0f));
	}
	else if (Data.EvaluatedData.Attribute == GetIceResistanceAttribute())
	{
		SetIceResistance(FMath::Clamp(GetIceResistance(), 0.0f, 1.0f));
	}
}

void UElementCharacterAttributeSet::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UElementCharacterAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UElementCharacterAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UElementCharacterAttributeSet, Stamina, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UElementCharacterAttributeSet, MaxStamina, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UElementCharacterAttributeSet, FireResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UElementCharacterAttributeSet, IceResistance, COND_None, REPNOTIFY_Always);
}

void UElementCharacterAttributeSet::OnRep_Health(const FGameplayAttributeData& PreviousValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UElementCharacterAttributeSet, Health, PreviousValue);
}

void UElementCharacterAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& PreviousValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UElementCharacterAttributeSet, MaxHealth, PreviousValue);
}

void UElementCharacterAttributeSet::OnRep_Stamina(const FGameplayAttributeData& PreviousValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UElementCharacterAttributeSet, Stamina, PreviousValue);
}

void UElementCharacterAttributeSet::OnRep_MaxStamina(const FGameplayAttributeData& PreviousValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UElementCharacterAttributeSet, MaxStamina, PreviousValue);
}

void UElementCharacterAttributeSet::OnRep_FireResistance(const FGameplayAttributeData& PreviousValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UElementCharacterAttributeSet, FireResistance, PreviousValue);
}

void UElementCharacterAttributeSet::OnRep_IceResistance(const FGameplayAttributeData& PreviousValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UElementCharacterAttributeSet, IceResistance, PreviousValue);
}
