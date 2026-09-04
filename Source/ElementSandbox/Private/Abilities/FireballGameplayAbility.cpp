#include "Abilities/FireballGameplayAbility.h"

#include "Animation/AnimSequenceBase.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "ElementGameplayWorldSubsystem.h"
#include "Elements/Fire/FireballProjectile.h"
#include "Equipment/EquipmentComponent.h"
#include "Game/ElementSandboxPlayerState.h"
#include "GameplayPrediction.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryConsumptionReceipt.h"
#include "Inventory/InventoryTypes.h"
#include "Item/ItemInstance.h"
#include "Items/FireballEquippedItemActor.h"
#include "Tags/ElementGameplayTags.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
bool BuildFireballLaunch(AElementSandboxCharacter& Character, FVector& OutLocation, FVector& OutDirection)
{
	FVector EyeLocation;
	FRotator AimRotation;
	Character.GetActorEyesViewPoint(EyeLocation, AimRotation);
	OutDirection = AimRotation.Vector().GetSafeNormal();
	if (OutDirection.IsNearlyZero())
	{
		return false;
	}
	const USkeletalMeshComponent* CharacterMesh = Character.GetMesh();
	OutLocation = CharacterMesh && CharacterMesh->DoesSocketExist(TEXT("hand_r"))
					  ? CharacterMesh->GetSocketLocation(TEXT("hand_r")) + OutDirection * 24.0
					  : EyeLocation + OutDirection * 80.0;
	return !OutLocation.ContainsNaN();
}
} // namespace

UFireballGameplayAbility::UFireballGameplayAbility()
{
	static ConstructorHelpers::FObjectFinder<UAnimSequenceBase> DefaultThrowAnimation(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01.MM_Attack_01"));
	ThrowAnimation = DefaultThrowAnimation.Object;

	FGameplayTagContainer FireballAbilityTags;
	FireballAbilityTags.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem_Fireball);
	SetAssetTags(FireballAbilityTags);
	ActivationOwnedTags.AddTag(ElementSandboxGameplayTags::Ability_State_UsingEquippedItem);
	BlockAbilitiesWithTag.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem);
}

bool UFireballGameplayAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags) || !ActorInfo ||
		!Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get()) || !IsValid(ThrowAnimation))
	{
		return false;
	}
	if (!ActorInfo->IsNetAuthority())
	{
		return true;
	}

	AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get());
	const UItemInstance* SourceItem = Cast<UItemInstance>(GetSourceObject(Handle, ActorInfo));
	const UEquipmentComponent* Equipment = Character ? Character->GetEquipmentComponent() : nullptr;
	const AElementSandboxPlayerState* PlayerState =
		Character ? Character->GetPlayerState<AElementSandboxPlayerState>() : nullptr;
	const UInventoryComponent* Inventory = PlayerState ? PlayerState->GetInventoryComponent() : nullptr;
	const int32 SelectedIndex = Inventory ? Inventory->GetSelectedQuickbarIndex() : INDEX_NONE;
	const UItemInstance* SelectedItem =
		Inventory && SelectedIndex != INDEX_NONE
			? Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Quickbar, SelectedIndex))
			: nullptr;
	const UElementGameplayWorldSubsystem* FireSubsystem =
		Character && Character->GetWorld()
			? Character->GetWorld()->GetSubsystem<UElementGameplayWorldSubsystem>() : nullptr;
	return IsValid(SourceItem) && Equipment && Equipment->GetCurrentEquippedItem() == SourceItem &&
		   SelectedItem == SourceItem && FireSubsystem && FireSubsystem->IsRuntimeAssemblyActive();
}

void UFireballGameplayAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	AElementSandboxCharacter* Character =
		ActorInfo ? Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (!Character || !Character->PlayPredictedUpperBodyAnimation(ThrowAnimation, 0.08f, 0.12f, PlayRate))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	bStartedAnimation = true;
	if (UEquipmentComponent* Equipment = Character->GetEquipmentComponent())
	{
		if (AFireballEquippedItemActor* EquippedProjection =
				Cast<AFireballEquippedItemActor>(Equipment->GetEquippedActor()))
		{
			EquippedProjection->SetActorHiddenInGame(true);
			HiddenEquippedProjection = EquippedProjection;
		}
	}

	const FPredictionKey PredictionKey = ActivationInfo.GetActivationPredictionKey();
	if (ActorInfo->IsLocallyControlled() && !ActorInfo->IsNetAuthority() &&
		!SpawnLocalPredictedProjectile(*ActorInfo, PredictionKey))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	if (ActorInfo->IsNetAuthority() && !SpawnAuthorityProjectile(Handle, *ActorInfo, PredictionKey))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	if (!IsActive())
	{
		return;
	}
	Character->GetWorldTimerManager().SetTimer(
		EndTimerHandle, this, &UFireballGameplayAbility::FinishThrow, UseLockSeconds, false);
}

void UFireballGameplayAbility::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const bool bReplicateEndAbility,
	const bool bWasCancelled)
{
	if (AFireballEquippedItemActor* EquippedProjection = HiddenEquippedProjection.Get())
	{
		EquippedProjection->SetActorHiddenInGame(false);
	}
	HiddenEquippedProjection.Reset();
	if (ActorInfo)
	{
		if (AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get()))
		{
			Character->GetWorldTimerManager().ClearTimer(EndTimerHandle);
			if (bWasCancelled && bStartedAnimation)
			{
				Character->StopPredictedUpperBodyAnimation();
			}
		}
	}
	bStartedAnimation = false;
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

bool UFireballGameplayAbility::SpawnAuthorityProjectile(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo& ActorInfo,
	const FPredictionKey& PredictionKey)
{
	AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo.AvatarActor.Get());
	UItemInstance* SourceItem = Cast<UItemInstance>(GetSourceObject(Handle, &ActorInfo));
	AElementSandboxPlayerState* PlayerState =
		Character ? Character->GetPlayerState<AElementSandboxPlayerState>() : nullptr;
	UInventoryComponent* Inventory = PlayerState ? PlayerState->GetInventoryComponent() : nullptr;
	const int32 SelectedIndex = Inventory ? Inventory->GetSelectedQuickbarIndex() : INDEX_NONE;
	const FInventorySlotAddress Address(EInventoryContainer::Quickbar, SelectedIndex);
	if (!Character || !Character->HasAuthority() || !Inventory || SelectedIndex == INDEX_NONE ||
		Inventory->GetItem(Address) != SourceItem)
	{
		return false;
	}

	FInventoryConsumptionReceipt Receipt;
	if (!Inventory->BeginConsumeItemQuantity(Address, 1, Receipt))
	{
		return false;
	}

	FVector SpawnLocation;
	FVector Direction;
	if (!BuildFireballLaunch(*Character, SpawnLocation, Direction))
	{
		Inventory->RollbackItemConsumption(Receipt);
		return false;
	}
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = Character;
	SpawnParameters.Instigator = Character;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFireballProjectile* Projectile = Character->GetWorld()->SpawnActor<AFireballProjectile>(
		AFireballProjectile::StaticClass(), FTransform(Direction.Rotation(), SpawnLocation), SpawnParameters);
	if (!IsValid(Projectile) || !Projectile->LaunchAuthority(Direction, PredictionKey))
	{
		if (IsValid(Projectile))
		{
			Projectile->Destroy();
		}
		Inventory->RollbackItemConsumption(Receipt);
		return false;
	}
	if (!Inventory->CommitItemConsumption(Receipt))
	{
		Projectile->Destroy();
		Inventory->RollbackItemConsumption(Receipt);
		return false;
	}
	return true;
}

AFireballProjectile* UFireballGameplayAbility::SpawnLocalPredictedProjectile(const FGameplayAbilityActorInfo& ActorInfo,
	const FPredictionKey& PredictionKey)
{
	AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo.AvatarActor.Get());
	if (!Character || !Character->IsLocallyControlled() || Character->HasAuthority() ||
		!PredictionKey.IsLocalClientKey())
	{
		return nullptr;
	}

	FVector SpawnLocation;
	FVector Direction;
	if (!BuildFireballLaunch(*Character, SpawnLocation, Direction))
	{
		return nullptr;
	}
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = Character;
	SpawnParameters.Instigator = Character;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFireballProjectile* Projectile = Character->GetWorld()->SpawnActor<AFireballProjectile>(
		AFireballProjectile::StaticClass(), FTransform(Direction.Rotation(), SpawnLocation), SpawnParameters);
	if (!IsValid(Projectile) || !Projectile->LaunchLocalPrediction(Direction, PredictionKey))
	{
		if (IsValid(Projectile))
		{
			Projectile->Destroy();
		}
		return nullptr;
	}

	FPredictionKey MutablePredictionKey = PredictionKey;
	MutablePredictionKey.NewRejectedDelegate().BindUObject(Projectile, &AFireballProjectile::RejectLocalPrediction);
	return Projectile;
}

void UFireballGameplayAbility::FinishThrow()
{
	if (IsActive())
	{
		EndAbility(GetCurrentAbilitySpecHandle(), GetCurrentActorInfo(), GetCurrentActivationInfo(), true, false);
	}
}
