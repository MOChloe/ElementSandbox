#pragma once

#include "Components/ActorComponent.h"

#include "PlayerHealthCoordinatorComponent.generated.h"

class UCharacterDeathWidget;
class UCharacterHealthBarWidget;
class UElementAbilitySystemComponent;
struct FOnAttributeChangeData;

/**
 * PlayerState ASC 的生命观察、死亡封口、UI 投影与 Authority 重生事务。
 * PlayerController 只保留输入意图和 Server RPC 入口。
 */
UCLASS(ClassGroup = (Gameplay))
class UPlayerHealthCoordinatorComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UPlayerHealthCoordinatorComponent();

	/** PlayerState/Pawn 变化后重绑 ASC；重复调用幂等。 */
	void RefreshBinding();
	bool IsHealthDepleted() const;
	bool IsLocalDeathPresentationActive() const { return bLocalDeathPresentationActive; }
	void RequestRespawn();
	bool TryRespawnAtPlayerStart();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UElementAbilitySystemComponent* ResolveAbilitySystem() const;
	void UnbindAbilitySystem();
	void RefreshHealthState();
	void HandleHealthAttributeChanged(const FOnAttributeChangeData& ChangeData);
	void ApplyLocalDeathPresentation(bool bDead);
	void ScheduleAuthorityDeath();
	void EnterAuthorityDeathState();

	UPROPERTY(Transient)
	TObjectPtr<UCharacterHealthBarWidget> HealthBar;

	UPROPERTY(Transient)
	TObjectPtr<UCharacterDeathWidget> DeathWidget;

	UPROPERTY(Transient)
	TObjectPtr<UElementAbilitySystemComponent> ObservedAbilitySystem;

	FDelegateHandle HealthChangedHandle;
	FDelegateHandle MaxHealthChangedHandle;
	bool bLocalDeathPresentationActive = false;
	bool bAuthorityDeathPending = false;
	bool bAuthorityDeathActive = false;
	bool bRespawnInProgress = false;
};
