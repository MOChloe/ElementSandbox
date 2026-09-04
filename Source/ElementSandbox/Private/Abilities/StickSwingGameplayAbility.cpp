#include "Abilities/StickSwingGameplayAbility.h"

#include "Characters/ElementSandboxCharacter.h"
#include "ElementGameplayWorldSubsystem.h"
#include "Equipment/EquipmentComponent.h"
#include "Items/StickEquippedItemActor.h"
#include "Projection/WorldObjectProxyComponent.h"

void UStickSwingGameplayAbility::OnSwingStarted(
	const FGameplayAbilityActorInfo* ActorInfo)
{
	SetAuthorityFireInteraction(ActorInfo, true);
}

void UStickSwingGameplayAbility::OnSwingEnded(
	const FGameplayAbilityActorInfo* ActorInfo)
{
	SetAuthorityFireInteraction(ActorInfo, false);
}

void UStickSwingGameplayAbility::SetAuthorityFireInteraction(
	const FGameplayAbilityActorInfo* ActorInfo,
	const bool bActive)
{
	if (bActive && (!ActorInfo || !ActorInfo->IsNetAuthority()))
	{
		return;
	}

	UWorld* World = ActorInfo && ActorInfo->AvatarActor.IsValid()
		? ActorInfo->AvatarActor->GetWorld()
		: GetWorld();
	UElementGameplayWorldSubsystem* Fire = World
		? World->GetSubsystem<UElementGameplayWorldSubsystem>()
		: nullptr;
	if (!bActive)
	{
		const FWorldEntityId Entity = FireInteractionEntity;
		FireInteractionEntity = {};
		if (Fire && Entity.IsSet())
		{
			Fire->SetStickFireInteractionState(Entity, false);
		}
		return;
	}

	AActor* Avatar = ActorInfo->AvatarActor.Get();
	const UEquipmentComponent* Equipment = Avatar
		? Avatar->FindComponentByClass<UEquipmentComponent>()
		: nullptr;
	const AStickEquippedItemActor* Stick = Equipment
		? Cast<AStickEquippedItemActor>(Equipment->GetEquippedActor())
		: nullptr;
	const UWorldObjectProxyComponent* Proxy = Stick
		? Stick->GetWorldObjectProxyComponent()
		: nullptr;
	const FWorldEntityId Entity = Proxy ? Proxy->GetWorldEntityId() : FWorldEntityId();
	if (Fire
		&& Entity.IsSet()
		&& Fire->SetStickFireInteractionState(Entity, true))
	{
		FireInteractionEntity = Entity;
	}
}
