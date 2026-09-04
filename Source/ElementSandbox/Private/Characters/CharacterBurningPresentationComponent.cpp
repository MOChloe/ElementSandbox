#include "Characters/CharacterBurningPresentationComponent.h"

#include "AbilitySystemComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Tags/ElementGameplayTags.h"

UCharacterBurningPresentationComponent::UCharacterBurningPresentationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicated(false);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	SetCastShadow(false);
	bCastDynamicShadow = false;
	bCastStaticShadow = false;
	SetReceivesDecals(false);
	SetRelativeLocation(FVector::ZeroVector);
	SetRelativeScale3D(FVector(0.65, 0.65, 1.80));
	SetHiddenInGame(true);
	SetVisibility(false);

}

void UCharacterBurningPresentationComponent::OnRegister()
{
	Super::OnRegister();
	SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone")));
	if (UMaterialInterface* FlameMaterial = LoadObject<UMaterialInterface>(
		nullptr,
		TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame")))
	{
		SetMaterial(0, FlameMaterial);
	}
}

void UCharacterBurningPresentationComponent::InitializeAbilitySystem(
	UAbilitySystemComponent* NewAbilitySystem)
{
	UAbilitySystemComponent* PreviousAbilitySystem = AbilitySystem.Get();
	if (PreviousAbilitySystem == NewAbilitySystem)
	{
		UpdateBurningVisual(
			NewAbilitySystem
			&& NewAbilitySystem->HasMatchingGameplayTag(
				ElementSandboxGameplayTags::State_Burning));
		return;
	}

	if (PreviousAbilitySystem && BurningTagChangedHandle.IsValid())
	{
		PreviousAbilitySystem->UnregisterGameplayTagEvent(
			BurningTagChangedHandle,
			ElementSandboxGameplayTags::State_Burning,
			EGameplayTagEventType::NewOrRemoved);
	}
	BurningTagChangedHandle.Reset();
	AbilitySystem = NewAbilitySystem;

	if (NewAbilitySystem)
	{
		BurningTagChangedHandle = NewAbilitySystem->RegisterGameplayTagEvent(
			ElementSandboxGameplayTags::State_Burning,
			EGameplayTagEventType::NewOrRemoved).AddUObject(
				this,
				&UCharacterBurningPresentationComponent::HandleBurningTagChanged);
	}
	UpdateBurningVisual(
		NewAbilitySystem
		&& NewAbilitySystem->HasMatchingGameplayTag(
			ElementSandboxGameplayTags::State_Burning));
}

void UCharacterBurningPresentationComponent::OnUnregister()
{
	InitializeAbilitySystem(nullptr);
	Super::OnUnregister();
}

void UCharacterBurningPresentationComponent::HandleBurningTagChanged(
	const FGameplayTag,
	const int32 NewCount)
{
	UpdateBurningVisual(NewCount > 0);
}

void UCharacterBurningPresentationComponent::UpdateBurningVisual(
	const bool bBurning)
{
	SetHiddenInGame(!bBurning, true);
	SetVisibility(bBurning, true);
}
