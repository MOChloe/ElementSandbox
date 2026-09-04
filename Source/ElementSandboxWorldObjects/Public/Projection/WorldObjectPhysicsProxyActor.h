#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "GameFramework/Actor.h"

#include "WorldObjectPhysicsProxyActor.generated.h"

class UBoxComponent;
class UPhysicalMaterial;
class UWorldObjectProxyComponent;

/** 少量可动物理 WorldObject 的 Chaos 投影；静止后由 ECS 回收。 */
UCLASS(NotBlueprintable, NotPlaceable, Transient)
class ELEMENTSANDBOXWORLDOBJECTS_API AWorldObjectPhysicsProxyActor final : public AActor
{
	GENERATED_BODY()

public:
	AWorldObjectPhysicsProxyActor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostNetReceive() override;
	virtual void OnRep_ReplicatedMovement() override;

	UWorldObjectProxyComponent* GetWorldObjectProxyComponent() const
	{
		return WorldObjectProxyComponent;
	}

	UBoxComponent* GetPhysicsBox() const { return PhysicsBox; }

	/** Authority 在 Entity 创建事务中只配置刚体；事务发布成功后必须显式调度释放。 */
	bool ConfigurePhysics(
		const FBox& LocalBounds,
		float MassKg,
		EWorldObjectPhysicsCollisionPolicy CollisionPolicy,
		const FVector& LinearVelocity,
		const FVector& AngularVelocityDegrees,
		uint32 ActivationRevision);
	/**
	 * Authority 仅在 Entity/Lifecycle/Query Snapshot 全部提交后调用。
	 * 短暂保留初始 Transform，使 Client 先建立可见投影，再复制释放边界与 Movement。
	 */
	void SchedulePhysicsRelease();
	/** Dormant 物件接触唤醒时表现早已存在，可在状态提交后立即释放 Chaos。 */
	void ReleasePhysicsImmediately();

	bool IsPhysicsConfigured() const { return PhysicsConfiguration.IsValid(); }
	bool IsPhysicsReleased() const { return bPhysicsReleased; }
	uint32 GetActivationRevision() const { return PhysicsConfiguration.ActivationRevision; }
	bool HasClientPhysicsProjection() const { return bClientPhysicsProjectionActive; }
	/** Client 在 Actor/配置/Record 任意到达次序下，只为当前绑定且处于 Physics 的实体启用刚体。 */
	void RefreshClientPhysicsProjection();
	/** Client 新代理接管或收到较新 Dormant 后永久退出；迟到 Movement 不得重新打开旧碰撞。 */
	void RetireClientPhysicsProjection();
	float GetConfiguredMassKg() const { return PhysicsConfiguration.MassKg; }
	EWorldObjectPhysicsCollisionPolicy GetConfiguredCollisionPolicy() const
	{
		return PhysicsConfiguration.CollisionPolicy;
	}
	FVector GetConfiguredLocalCenter() const { return PhysicsConfiguration.LocalCenter; }
	FVector GetConfiguredLocalExtent() const { return PhysicsConfiguration.LocalExtent; }
	FTransform GetWorldObjectTransform() const;
	static FTransform MakeActorTransform(
		const FTransform& WorldObjectTransform,
		const FBox& LocalBounds);

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category="Physics")
	TObjectPtr<UBoxComponent> PhysicsBox;

	UPROPERTY(VisibleAnywhere, Category="World Object")
	TObjectPtr<UWorldObjectProxyComponent> WorldObjectProxyComponent;

	UPROPERTY(ReplicatedUsing=OnRep_PhysicsConfiguration)
	FWorldObjectPhysicsBodyNetState PhysicsConfiguration;

	/** Authority 定时释放；复制该边界可避免客户端只看到已经落地的最终 Transform。 */
	UPROPERTY(ReplicatedUsing=OnRep_PhysicsReleased)
	bool bPhysicsReleased = false;

	UPROPERTY(Transient)
	TObjectPtr<UPhysicalMaterial> PhysicsMaterial;

	UFUNCTION()
	void OnRep_PhysicsConfiguration();

	UFUNCTION()
	void OnRep_PhysicsReleased();

	void ApplyPhysicsConfiguration();
	void ApplyPhysicsReleaseState();
	void SetClientPhysicsProjectionActive(bool bActive);

	FTimerHandle PhysicsReleaseTimer;
	FVector PendingInitialLinearVelocity = FVector::ZeroVector;
	FVector PendingInitialAngularVelocityDegrees = FVector::ZeroVector;
	bool bHasDeferredReplicatedMovement = false;
	bool bClientPhysicsProjectionActive = false;
	bool bClientPhysicsProjectionRetired = false;
};
