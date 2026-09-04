// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CharacterSnapshotHandle.h"
#include "GameFramework/Character.h"
#include "ElementSandboxCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputAction;
class UEquipmentComponent;
class UEquipmentAbilityBridgeComponent;
class UWorldObjectEquipmentBridgeComponent;
class UCharacterBurningPresentationComponent;
class UAnimSequenceBase;
struct FInputActionValue;

/**
 * 可直接由 C++ GameMode 创建的第三人称角色。
 * 这里只保留移动、视角和跳跃这组项目级基础能力。
 */
UCLASS()
class AElementSandboxCharacter : public ACharacter
{
	GENERATED_BODY()

	/** 将摄像机保持在角色身后，并负责镜头碰撞。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	/** 第三人称跟随摄像机。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FollowCamera;

	/** 服务器管理当前道具的手持 Actor 投影。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEquipmentComponent> EquipmentComponent;

	/** 只在装配层连接 Equipment 事件与 PlayerState ASC。 */
	UPROPERTY(VisibleAnywhere, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEquipmentAbilityBridgeComponent> EquipmentAbilityBridgeComponent;

	/** 把 Items 装备投影接入独立 WorldObject ECS。 */
	UPROPERTY(VisibleAnywhere, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UWorldObjectEquipmentBridgeComponent> WorldObjectEquipmentBridgeComponent;

	/** 根据 ASC 的 State.Burning Tag 显示占位火焰，不复制第二份燃烧状态。 */
	UPROPERTY(VisibleAnywhere, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCharacterBurningPresentationComponent> BurningPresentationComponent;

	/** 角色到候选命中点的最大聚焦距离；默认 3 米，可在派生 Blueprint 默认值中配置。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Focus",
		meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	double FocusDistance = 300.0;

protected:

	/** 跳跃输入。 */
	UPROPERTY()
	TObjectPtr<UInputAction> JumpAction;

	/** 移动输入。 */
	UPROPERTY()
	TObjectPtr<UInputAction> MoveAction;

	/** 手柄视角输入。 */
	UPROPERTY()
	TObjectPtr<UInputAction> LookAction;

	/** 鼠标视角输入。 */
	UPROPERTY()
	TObjectPtr<UInputAction> MouseLookAction;

public:

	explicit AElementSandboxCharacter(
		const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:

	/** 绑定角色输入。 */
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void UnPossessed() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 将二维输入转换为相对控制器朝向的世界移动。 */
	void Move(const FInputActionValue& Value);

	/** 将二维输入写入控制器的水平和俯仰视角。 */
	void Look(const FInputActionValue& Value);

	/** 鼠标使用独立横纵比例；不影响手柄视角曲线。 */
	void MouseLook(const FInputActionValue& Value);

public:

	/** 返回角色的镜头臂。 */
	FORCEINLINE USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	/** 返回角色的跟随摄像机。 */
	FORCEINLINE UCameraComponent* GetFollowCamera() const { return FollowCamera; }

	FORCEINLINE UEquipmentComponent* GetEquipmentComponent() const { return EquipmentComponent; }
	FORCEINLINE UWorldObjectEquipmentBridgeComponent* GetWorldObjectEquipmentBridgeComponent() const
	{
		return WorldObjectEquipmentBridgeComponent;
	}

	UFUNCTION(BlueprintPure, Category="Focus")
	double GetFocusDistance() const { return FMath::Max(0.0, FocusDistance); }

	/**
	 * LocalPredicted Ability 先在所属客户端播放；服务器再广播给非所属客户端。
	 * 动画资源和行为语义由 Ability 提供，角色层不判断木棍、弓箭或攻击类型。
	 */
	bool PlayPredictedUpperBodyAnimation(
		UAnimSequenceBase* Animation,
		float BlendInTime = 0.1f,
		float BlendOutTime = 0.15f,
		float PlayRate = 1.0f);

	/** 预测被服务器拒绝或装备被收起时，回滚当前上半身动作。 */
	void StopPredictedUpperBodyAnimation(float BlendOutTime = 0.1f);

private:
	/** 当前 World 中的本地 Character 查询快照投影；不复制、不跨 World。 */
	FCharacterSnapshotHandle CharacterSnapshotHandle;

	/** 短生命周期的表现事件允许丢包；真正的命中和元素后果不会依赖这个 RPC。 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayUpperBodyAnimation(
		UAnimSequenceBase* Animation,
		float BlendInTime,
		float BlendOutTime,
		float PlayRate);

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastStopUpperBodyAnimation(float BlendOutTime);

		void InitializeAbilityActorInfo();
		void EnsureBurningPresentation();
		void ClearAbilityActorInfo();
	bool PlayUpperBodyAnimationLocally(
		UAnimSequenceBase* Animation,
		float BlendInTime,
		float BlendOutTime,
		float PlayRate) const;
	void StopUpperBodyAnimationLocally(float BlendOutTime) const;
};
