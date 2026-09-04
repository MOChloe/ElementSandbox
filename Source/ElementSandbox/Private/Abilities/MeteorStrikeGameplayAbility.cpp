#include "Abilities/MeteorStrikeGameplayAbility.h"

#include "Animation/AnimSequenceBase.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Game/ElementSandboxPlayerState.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryConsumptionReceipt.h"
#include "Inventory/InventoryTypes.h"
#include "Item/ItemInstance.h"
#include "Meteor/MeteorStrikeActor.h"
#include "MeteorWorldSubsystem.h"
#include "Tags/ElementGameplayTags.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

using namespace UE::ElementSandbox::Meteor;

DEFINE_LOG_CATEGORY_STATIC(LogMeteorStrikeAbility, Log, All);

UMeteorStrikeGameplayAbility::UMeteorStrikeGameplayAbility()
{
	static ConstructorHelpers::FObjectFinder<UAnimSequenceBase> Animation(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01.MM_Attack_01"));
	UseAnimation = Animation.Object;
	FGameplayTagContainer Tags;
	Tags.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem_MeteorStrike);
	SetAssetTags(Tags);
	ActivationOwnedTags.AddTag(ElementSandboxGameplayTags::Ability_State_UsingEquippedItem);
	BlockAbilitiesWithTag.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem);
}

bool UMeteorStrikeGameplayAbility::CanActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)
		|| !ActorInfo || !IsValid(UseAnimation)
		|| !Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get())) return false;
	if (!ActorInfo->IsNetAuthority()) return true;
	const AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get());
	const UMeteorWorldSubsystem* Meteor = Character && Character->GetWorld()
		? Character->GetWorld()->GetSubsystem<UMeteorWorldSubsystem>() : nullptr;
	return Meteor && !Meteor->HasActiveBurst();
}

void UMeteorStrikeGameplayAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	AElementSandboxCharacter* Character = ActorInfo
		? Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (!Character || !Character->PlayPredictedUpperBodyAnimation(UseAnimation, 0.08f, 0.12f, 1.0f)
		|| (ActorInfo->IsNetAuthority() && !ExecuteAuthorityStrike(Handle, *ActorInfo)))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	Character->GetWorldTimerManager().SetTimer(
		EndTimerHandle, this, &UMeteorStrikeGameplayAbility::FinishUse, 0.45f, false);
}

void UMeteorStrikeGameplayAbility::EndAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const bool bReplicateEndAbility,
	const bool bWasCancelled)
{
	if (ActorInfo)
	{
		if (AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get()))
		{
			Character->GetWorldTimerManager().ClearTimer(EndTimerHandle);
			if (bWasCancelled) Character->StopPredictedUpperBodyAnimation();
		}
	}
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

bool UMeteorStrikeGameplayAbility::ExecuteAuthorityStrike(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo& ActorInfo)
{
	AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo.AvatarActor.Get());
	AElementSandboxPlayerState* PlayerState = Character
		? Character->GetPlayerState<AElementSandboxPlayerState>() : nullptr;
	UInventoryComponent* Inventory = PlayerState ? PlayerState->GetInventoryComponent() : nullptr;
	UItemInstance* SourceItem = Cast<UItemInstance>(GetSourceObject(Handle, &ActorInfo));
	const int32 SelectedIndex = Inventory ? Inventory->GetSelectedQuickbarIndex() : INDEX_NONE;
	const FInventorySlotAddress Address(EInventoryContainer::Quickbar, SelectedIndex);
	UMeteorWorldSubsystem* Meteor = Character && Character->GetWorld()
		? Character->GetWorld()->GetSubsystem<UMeteorWorldSubsystem>() : nullptr;
	if (!Character || !Character->HasAuthority() || !Inventory || !Meteor || SelectedIndex == INDEX_NONE
		|| Inventory->GetItem(Address) != SourceItem || Meteor->HasActiveBurst())
	{
		UE_LOG(LogMeteorStrikeAbility, Warning,
			TEXT("Authority 拒绝陨石：Character=%d Authority=%d Inventory=%d Meteor=%d Selected=%d SourceMatches=%d ActiveBurst=%d。"),
			Character != nullptr,
			Character && Character->HasAuthority(),
			Inventory != nullptr,
			Meteor != nullptr,
			SelectedIndex,
			Inventory && SelectedIndex != INDEX_NONE && Inventory->GetItem(Address) == SourceItem,
			Meteor && Meteor->HasActiveBurst());
		return false;
	}

	const FMeteorRuntimeConfig Config = Meteor->GetRuntimeConfig();
	const FVector ViewerLocation = Character->GetActorLocation();
	const FVector ViewerForward = Character->GetActorForwardVector();
	FVector Impact;
	if (!Meteor->TryGetMapImpactLocation(ViewerLocation, ViewerForward, Impact))
	{
		UE_LOG(LogMeteorStrikeAbility, Warning, TEXT("Authority 拒绝陨石：当前地图没有可用落点。"));
		return false;
	}

	FInventoryConsumptionReceipt Receipt;
	if (!Inventory->BeginConsumeItemQuantity(Address, 1, Receipt))
	{
		UE_LOG(LogMeteorStrikeAbility, Warning, TEXT("Authority 拒绝陨石：唯一核心无法进入消耗事务。"));
		return false;
	}
	const double StartTime = Character->GetWorld()->GetTimeSeconds();
	const double ImpactTime = StartTime + Config.MeteorFallSeconds;
	const FVector StartLocation = Config.ComputeMeteorStartLocation(Impact, ViewerLocation);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = Character;
	SpawnParameters.Instigator = Character;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMeteorStrikeActor* Visual = Character->GetWorld()->SpawnActor<AMeteorStrikeActor>(
		AMeteorStrikeActor::StaticClass(), FTransform(FRotator::ZeroRotator, StartLocation), SpawnParameters);
	FMeteorBurstId BurstId;
	if (!Visual || !Visual->LaunchAuthority(
		StartLocation, Impact, StartTime, ImpactTime, Config.MeteorDiameter)
		|| !Meteor->ScheduleStrike(Impact, ImpactTime, BurstId))
	{
		UE_LOG(LogMeteorStrikeAbility, Warning,
			TEXT("Authority 陨石排程失败：Visual=%d，Start=(%.0f,%.0f,%.0f)，Impact=(%.0f,%.0f,%.0f)。"),
			Visual != nullptr,
			StartLocation.X, StartLocation.Y, StartLocation.Z,
			Impact.X, Impact.Y, Impact.Z);
		if (Visual) Visual->Destroy();
		Inventory->RollbackItemConsumption(Receipt);
		return false;
	}
	if (!Inventory->CommitItemConsumption(Receipt))
	{
		UE_LOG(LogMeteorStrikeAbility, Warning, TEXT("Authority 陨石核心提交失败；撤销尚未命中的 Burst。"));
		Meteor->CancelScheduledStrike(BurstId);
		Visual->Destroy();
		Inventory->RollbackItemConsumption(Receipt);
		return false;
	}
	UE_LOG(LogMeteorStrikeAbility, Display,
		TEXT("唯一陨石核心已触发 Burst=%llu：直径=%.0fm，起点=(%.0f,%.0f,%.0f)，落点=(%.0f,%.0f,%.0f)。"),
		BurstId.Value,
		Config.MeteorDiameter / 100.0f,
		StartLocation.X, StartLocation.Y, StartLocation.Z,
		Impact.X, Impact.Y, Impact.Z);
	return true;
}

void UMeteorStrikeGameplayAbility::FinishUse()
{
	if (IsActive())
	{
		EndAbility(GetCurrentAbilitySpecHandle(), GetCurrentActorInfo(), GetCurrentActivationInfo(), true, false);
	}
}
