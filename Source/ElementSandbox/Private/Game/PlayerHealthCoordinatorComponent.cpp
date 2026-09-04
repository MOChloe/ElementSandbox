#include "Game/PlayerHealthCoordinatorComponent.h"

#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Attributes/ElementCharacterAttributeSet.h"
#include "Characters/ElementSandboxCharacter.h"
#include "CharacterQuerySnapshotSubsystem.h"
#include "Game/ElementSandboxPlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameplayEffect.h"
#include "Inventory/InventoryHUDWidget.h"
#include "Tags/ElementGameplayTags.h"
#include "TimerManager.h"
#include "Vitals/CharacterDeathWidget.h"
#include "Vitals/CharacterHealthBarWidget.h"

namespace
{
void RemoveTemporaryGameplayEffects(UAbilitySystemComponent& AbilitySystem)
{
	const TArray<FActiveGameplayEffectHandle> ActiveEffectHandles =
		AbilitySystem.GetActiveEffects(FGameplayEffectQuery());
	for (const FActiveGameplayEffectHandle EffectHandle : ActiveEffectHandles)
	{
		const FActiveGameplayEffect* ActiveEffect = AbilitySystem.GetActiveGameplayEffect(EffectHandle);
		if (ActiveEffect && ActiveEffect->Spec.Def &&
			ActiveEffect->Spec.Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
		{
			AbilitySystem.RemoveActiveGameplayEffect(EffectHandle);
		}
	}
}
} // namespace

UPlayerHealthCoordinatorComponent::UPlayerHealthCoordinatorComponent() { PrimaryComponentTick.bCanEverTick = false; }

void UPlayerHealthCoordinatorComponent::BeginPlay()
{
	Super::BeginPlay();
	RefreshBinding();
}

void UPlayerHealthCoordinatorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindAbilitySystem();
	if (HealthBar)
	{
		HealthBar->RemoveFromParent();
		HealthBar = nullptr;
	}
	if (DeathWidget)
	{
		DeathWidget->RemoveFromParent();
		DeathWidget = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

UElementAbilitySystemComponent* UPlayerHealthCoordinatorComponent::ResolveAbilitySystem() const
{
	const AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	return Controller ? Controller->GetElementAbilitySystemComponent() : nullptr;
}

void UPlayerHealthCoordinatorComponent::RefreshBinding()
{
	AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	UElementAbilitySystemComponent* AbilitySystem = ResolveAbilitySystem();
	if (!Controller || !AbilitySystem ||
		!AbilitySystem->HasAttributeSetForAttribute(UElementCharacterAttributeSet::GetHealthAttribute()) ||
		!AbilitySystem->HasAttributeSetForAttribute(UElementCharacterAttributeSet::GetMaxHealthAttribute()))
	{
		UnbindAbilitySystem();
		if (HealthBar)
			HealthBar->SetVisibility(ESlateVisibility::Collapsed);
		if (DeathWidget)
			DeathWidget->SetDeathShown(false);
		ApplyLocalDeathPresentation(false);
		return;
	}

	if (ObservedAbilitySystem != AbilitySystem)
	{
		UnbindAbilitySystem();
		ObservedAbilitySystem = AbilitySystem;
		HealthChangedHandle =
			AbilitySystem->GetGameplayAttributeValueChangeDelegate(UElementCharacterAttributeSet::GetHealthAttribute())
				.AddUObject(this, &UPlayerHealthCoordinatorComponent::HandleHealthAttributeChanged);
		MaxHealthChangedHandle =
			AbilitySystem
				->GetGameplayAttributeValueChangeDelegate(UElementCharacterAttributeSet::GetMaxHealthAttribute())
				.AddUObject(this, &UPlayerHealthCoordinatorComponent::HandleHealthAttributeChanged);
	}

	if (Controller->IsLocalPlayerController() && Controller->GetLocalPlayer())
	{
		if (!HealthBar)
		{
			HealthBar = CreateWidget<UCharacterHealthBarWidget>(Controller, UCharacterHealthBarWidget::StaticClass());
			if (HealthBar)
				HealthBar->AddToViewport(10);
		}
		if (HealthBar)
			HealthBar->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (!DeathWidget)
		{
			DeathWidget = CreateWidget<UCharacterDeathWidget>(Controller, UCharacterDeathWidget::StaticClass());
			if (DeathWidget)
				DeathWidget->AddToViewport(100);
		}
	}
	RefreshHealthState();
}

void UPlayerHealthCoordinatorComponent::UnbindAbilitySystem()
{
	if (ObservedAbilitySystem)
	{
		if (HealthChangedHandle.IsValid())
		{
			ObservedAbilitySystem
				->GetGameplayAttributeValueChangeDelegate(UElementCharacterAttributeSet::GetHealthAttribute())
				.Remove(HealthChangedHandle);
		}
		if (MaxHealthChangedHandle.IsValid())
		{
			ObservedAbilitySystem
				->GetGameplayAttributeValueChangeDelegate(UElementCharacterAttributeSet::GetMaxHealthAttribute())
				.Remove(MaxHealthChangedHandle);
		}
	}
	HealthChangedHandle.Reset();
	MaxHealthChangedHandle.Reset();
	ObservedAbilitySystem = nullptr;
}

void UPlayerHealthCoordinatorComponent::RefreshHealthState()
{
	AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (!Controller || !ObservedAbilitySystem)
		return;
	const float Health =
		ObservedAbilitySystem->GetNumericAttribute(UElementCharacterAttributeSet::GetHealthAttribute());
	const float MaxHealth =
		ObservedAbilitySystem->GetNumericAttribute(UElementCharacterAttributeSet::GetMaxHealthAttribute());
	if (HealthBar)
		HealthBar->SetHealth(Health, MaxHealth);
	const bool bDead = Health <= 0.0f;
	ApplyLocalDeathPresentation(bDead);
	if (Controller->HasAuthority() && bDead)
		ScheduleAuthorityDeath();
}

void UPlayerHealthCoordinatorComponent::HandleHealthAttributeChanged(const FOnAttributeChangeData&)
{
	RefreshHealthState();
}

void UPlayerHealthCoordinatorComponent::ApplyLocalDeathPresentation(const bool bDead)
{
	AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalPlayerController())
		return;
	if (DeathWidget)
		DeathWidget->SetDeathShown(bDead);
	if (bLocalDeathPresentationActive == bDead)
		return;
	bLocalDeathPresentationActive = bDead;
	if (bDead)
	{
		if (Controller->InventoryHUD && Controller->InventoryHUD->IsBackpackOpen())
		{
			Controller->SetInventoryOpen(false);
		}
		Controller->ReleaseUseEquippedItem();
		Controller->RefreshLocalGameplayInputMode();
		if (AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(Controller->GetPawn()))
		{
			if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
			{
				Movement->StopMovementImmediately();
				Movement->DisableMovement();
			}
		}
		return;
	}
	Controller->RefreshLocalGameplayInputMode();
}

void UPlayerHealthCoordinatorComponent::ScheduleAuthorityDeath()
{
	AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (!Controller || !Controller->HasAuthority() || bAuthorityDeathPending || bAuthorityDeathActive ||
		bRespawnInProgress || !IsHealthDepleted())
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
		return;
	bAuthorityDeathPending = true;
	World->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateUObject(this, &UPlayerHealthCoordinatorComponent::EnterAuthorityDeathState));
}

void UPlayerHealthCoordinatorComponent::EnterAuthorityDeathState()
{
	bAuthorityDeathPending = false;
	AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (!Controller || !Controller->HasAuthority() || bAuthorityDeathActive || bRespawnInProgress ||
		!IsHealthDepleted())
	{
		return;
	}
	bAuthorityDeathActive = true;
	if (UElementAbilitySystemComponent* AbilitySystem = ResolveAbilitySystem())
	{
		AbilitySystem->AbilityInputTagReleased(ElementSandboxGameplayTags::Input_Use_Primary);
		AbilitySystem->CancelAllAbilities();
	}
	if (AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(Controller->GetPawn()))
	{
		// 死亡 Pawn 仍保留用于镜头，但已不再是可参与 Gameplay 查询的角色目标。
		// 走中性的 Snapshot Remove，让 Element 等消费者撤销自身投影；这里不识别 Fire。
		if (UWorld* World = Character->GetWorld())
		{
			if (UCharacterQuerySnapshotSubsystem* Snapshots =
				World->GetSubsystem<UCharacterQuerySnapshotSubsystem>())
			{
				const FCharacterSnapshotHandle Snapshot = Snapshots->FindSnapshot(*Character);
				if (Snapshot.IsSet()) Snapshots->UnregisterCharacter(Snapshot);
			}
		}
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
			Movement->DisableMovement();
		}
	}
}

void UPlayerHealthCoordinatorComponent::RequestRespawn()
{
	AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController() || !IsHealthDepleted())
		return;
	if (Controller->HasAuthority())
	{
		TryRespawnAtPlayerStart();
	}
	else
	{
		Controller->ServerRequestRespawn();
	}
}

bool UPlayerHealthCoordinatorComponent::TryRespawnAtPlayerStart()
{
	AElementSandboxPlayerController* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (!Controller || !Controller->HasAuthority() || bRespawnInProgress || !bAuthorityDeathActive ||
		!IsHealthDepleted())
	{
		return false;
	}
	UWorld* World = GetWorld();
	UElementAbilitySystemComponent* AbilitySystem = ResolveAbilitySystem();
	AGameModeBase* GameMode = World ? World->GetAuthGameMode<AGameModeBase>() : nullptr;
	if (!World || !AbilitySystem || !GameMode)
		return false;

	bRespawnInProgress = true;
	APawn* PawnToDestroy = Controller->GetPawn();
	if (PawnToDestroy)
	{
		Controller->UnPossess();
		PawnToDestroy->Destroy();
	}

	AbilitySystem->CancelAllAbilities();
	RemoveTemporaryGameplayEffects(*AbilitySystem);
	AbilitySystem->SetNumericAttributeBase(UElementCharacterAttributeSet::GetIncomingDamageAttribute(), 0.0f);
	AActor* RespawnStart = GameMode->FindPlayerStart(Controller);
	if (!IsValid(Cast<APlayerStart>(RespawnStart)))
	{
		AbilitySystem->SetNumericAttributeBase(UElementCharacterAttributeSet::GetHealthAttribute(), 0.0f);
		bRespawnInProgress = false;
		return false;
	}
	const float MaxHealth = AbilitySystem->GetNumericAttribute(UElementCharacterAttributeSet::GetMaxHealthAttribute());
	const float MaxStamina =
		AbilitySystem->GetNumericAttribute(UElementCharacterAttributeSet::GetMaxStaminaAttribute());
	AbilitySystem->SetNumericAttributeBase(UElementCharacterAttributeSet::GetHealthAttribute(), MaxHealth);
	AbilitySystem->SetNumericAttributeBase(UElementCharacterAttributeSet::GetStaminaAttribute(), MaxStamina);

	GameMode->RestartPlayer(Controller);
	const bool bRestarted = IsValid(Controller->GetPawn());
	if (bRestarted)
	{
		bAuthorityDeathActive = false;
		Controller->bServerInventoryOpen = false;
	}
	else
	{
		AbilitySystem->SetNumericAttributeBase(UElementCharacterAttributeSet::GetHealthAttribute(), 0.0f);
	}
	bRespawnInProgress = false;
	return bRestarted;
}

bool UPlayerHealthCoordinatorComponent::IsHealthDepleted() const
{
	const UElementAbilitySystemComponent* AbilitySystem =
		ObservedAbilitySystem ? ObservedAbilitySystem.Get() : ResolveAbilitySystem();
	return AbilitySystem &&
		   AbilitySystem->HasAttributeSetForAttribute(UElementCharacterAttributeSet::GetHealthAttribute()) &&
		   AbilitySystem->GetNumericAttribute(UElementCharacterAttributeSet::GetHealthAttribute()) <= 0.0f;
}
