#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldEntityId.h"

#include "WorldObjectProxyComponent.generated.h"

class UPrimitiveComponent;

/** 把一个少量存在的 Actor/Chaos 表现绑定到 WorldObject ECS 身份。 */
UCLASS(NotBlueprintable, ClassGroup=(WorldObject), meta=(BlueprintSpawnableComponent))
class ELEMENTSANDBOXWORLDOBJECTS_API UWorldObjectProxyComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldObjectProxyComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	FWorldEntityId GetWorldEntityId() const { return WorldEntityId; }
	FWorldObjectEntityHandle GetLocalEntity() const { return LocalEntity; }
	UPrimitiveComponent* GetPhysicsPrimitive() const;
	bool IsPhysicsProjectionActive() const { return bPhysicsProjectionActive; }

	/** 仅 Authority 在 Entity 原子创建成功后调用。 */
	bool AssignAuthorityWorldEntityId(FWorldEntityId InWorldEntityId);

	/**
	 * 自定义可见 Actor 的临时 Chaos 投影开关。自动 Physics Proxy 使用自身复制配置，
	 * 不走此入口；Authority 修改后会把启停边界复制给 Client。
	 */
	bool SetAuthorityPhysicsProjectionActive(bool bActive);

	/** Subsystem 销毁 Entity 时禁止 EndPlay 再反向提交一次销毁。 */
	void SuppressEntityDestroyOnEndPlay() { bDestroyEntityOnEndPlay = false; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(ReplicatedUsing=OnRep_WorldEntityId)
	FWorldEntityId WorldEntityId;

	UPROPERTY(ReplicatedUsing=OnRep_PhysicsProjectionActive)
	bool bPhysicsProjectionActive = false;

	FWorldObjectEntityHandle LocalEntity;
	bool bDestroyEntityOnEndPlay = true;

	UFUNCTION()
	void OnRep_WorldEntityId();

	UFUNCTION()
	void OnRep_PhysicsProjectionActive();

	UFUNCTION()
	void HandleComponentWake(UPrimitiveComponent* WakingComponent, FName BoneName);

	UFUNCTION()
	void HandleComponentSleep(UPrimitiveComponent* SleepingComponent, FName BoneName);

	void RegisterWithSubsystem();
	void ApplyPhysicsProjectionState();
	void BindPhysicsDelegates();
	void UnbindPhysicsDelegates();
	void SetLocalEntity(FWorldObjectEntityHandle Entity) { LocalEntity = Entity; }

	friend class UWorldObjectWorldSubsystem;
};
