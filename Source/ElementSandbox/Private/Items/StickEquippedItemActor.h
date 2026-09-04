#pragma once

#include "CoreMinimal.h"
#include "Equipment/EquippedItemActor.h"
#include "Fire/ElementFireWorldObjectProjection.h"
#include "StickEquippedItemActor.generated.h"

class UCapsuleComponent;
class UStaticMeshComponent;
class UWorldObjectProxyComponent;

/** 木棍的共享装备/投掷表现；燃烧事实仍由 Authority 的 ECS 持有。 */
UCLASS()
class AStickEquippedItemActor : public AEquippedItemActor, public IElementFireWorldObjectProjection
{
	GENERATED_BODY()

public:
	AStickEquippedItemActor();
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UWorldObjectProxyComponent* GetWorldObjectProxyComponent() const
	{
		return WorldObjectProxyComponent;
	}

	/** 装备系统完成附着后，把 Actor 根从握点平移到棍身中心。 */
	void ApplyHeldGripOffset();
	bool CanBeginServerThrow() const;
	bool BeginServerThrow(const FVector& InitialVelocity);
	bool IsBurning() const { return bBurning; }

	/** 只允许 Authority 的 WorldObject Element Bridge 投影 ECS 燃烧事实。 */
	void SetBurning(bool bNewBurning);
	virtual void ApplyElementFireBurning(bool bNewBurning) override { SetBurning(bNewBurning); }
	virtual void QueryElementFireContext(
		bool& bOutEquipped,
		ACharacter*& OutHolderCharacter) const override;

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void OnRep_Burning();
	void EnsureFirePresentation();
	void UpdateBurningVisual();

	UPROPERTY(VisibleAnywhere, Category="World Object")
	TObjectPtr<UCapsuleComponent> PhysicsRoot;

	UPROPERTY(VisibleAnywhere, Category="World Object")
	TObjectPtr<UWorldObjectProxyComponent> WorldObjectProxyComponent;

	UPROPERTY(VisibleAnywhere, Category="Fire")
	TObjectPtr<UStaticMeshComponent> LowerFlame;

	UPROPERTY(VisibleAnywhere, Category="Fire")
	TObjectPtr<UStaticMeshComponent> UpperFlame;

	UPROPERTY(ReplicatedUsing=OnRep_Burning)
	bool bBurning = false;

	bool bHeldGripOffsetApplied = false;
};
