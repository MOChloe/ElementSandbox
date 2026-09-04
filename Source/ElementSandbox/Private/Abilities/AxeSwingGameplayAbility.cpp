#include "Abilities/AxeSwingGameplayAbility.h"

#include "Characters/ElementSandboxCharacter.h"
#include "WorldDestructionAuthorityService.h"

void UAxeSwingGameplayAbility::OnAuthorityImpact(
	const FGameplayAbilityActorInfo* ActorInfo)
{
	AElementSandboxCharacter* Character = ActorInfo
		? Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get())
		: nullptr;
	UWorld* World = Character ? Character->GetWorld() : nullptr;
	if (Character && World && Character->HasAuthority())
	{
		FVector ViewOrigin;
		FRotator ViewRotation;
		Character->GetActorEyesViewPoint(ViewOrigin, ViewRotation);
		UE::ElementSandbox::Destruction::FWorldDestructionTarget Target;
		if (!UE::ElementSandbox::Destruction::FWorldDestructionAuthorityService::TryResolveNearestTarget(
			*World,
			ViewOrigin,
			ViewRotation.Vector(),
			Character->GetActorLocation(),
			Character->GetFocusDistance(),
			Target))
		{
			return;
		}

		UE::ElementSandbox::Destruction::FWorldDestructionRequest Request;
		Request.Target = Target;
		Request.DamageMode = UE::ElementSandbox::Destruction::EWorldDestructionDamageMode::Additive;
		Request.Damage = 25.0f;
		UE::ElementSandbox::Destruction::FWorldDestructionAuthorityService::TryApplyRequest(
			*World,
			Request);
	}
}
