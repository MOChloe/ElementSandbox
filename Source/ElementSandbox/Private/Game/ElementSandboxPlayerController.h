// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "Entity/WorldEntityId.h"
#include "GameFramework/PlayerController.h"
#include "Placement/BuildPlacementTypes.h"
#include "WorldObjects/WorldObjectPickupFailure.h"
#include "ElementSandboxPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
class UElementAbilitySystemComponent;
class UInventoryComponent;
class UItemDefinition;
class UInventoryHUDWidget;
class UFocusHostComponent;
class UBuildingFocusQueryComponent;
class UBuildingFocusHighlightPresenterComponent;
class UFocusPromptPresenterComponent;
class UPresentationViewSourceComponent;
class UBuildingCollisionActivationComponent;
class USettlementTreeCollisionSourceComponent;
class UWorldObjectCollisionActivationComponent;
class UBuildingPlacementComponent;
class UWorldObjectFocusQueryComponent;
class UWorldObjectPickupComponent;
class UWorldObjectFocusHighlightPresenterComponent;
class UWorldChunkStreamingComponent;
class UWorldStreamingHUDPresenterComponent;
class UPlayerHealthCoordinatorComponent;
class UMeteorStreamingComponent;
namespace UE::ElementSandbox::NetBulk
{
	class FConnectionScheduler;
}

/**
 * 本地玩家的输入组合根与 Owner RPC 门面。
 *
 * 该类只编排输入、组件和跨网事务；HUD 采样、生命/死亡/重生等有独立生命周期的职责由组件持有。
 */
UCLASS()
class AElementSandboxPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AElementSandboxPlayerController();

	/** 鼠标 Delta 已由 InputSettings 提供基础灵敏度；这里只施加项目级横纵比例。 */
	void ApplyMouseLookInput(const FVector2D& LookInput);

	/** Focus Handler 的本地入口；跨网只发送服务器分配的 WorldEntityId。 */
	bool RequestPickupWorldObject(FWorldEntityId WorldEntityId);
	/** Door Focus 的本地入口；跨网只发送服务器分配的 Building WorldEntityId。 */
	bool RequestDoorInteraction(FWorldEntityId WorldEntityId);
	/** 拆除锤主要 Use 的本地入口；跨网只发送服务器分配的 Building WorldEntityId。 */
	bool RequestBuildingDismantle(FWorldEntityId WorldEntityId);
	bool IsDemolitionToolSelected() const;
	bool CanDismantleBuildingDefinition(FName BuildingDefinitionId) const;
	bool TryResolveSelectedDismantleReward(
		FName BuildingDefinitionId,
		UItemDefinition*& OutItemDefinition,
		int32& OutQuantity) const;
	/** 输入与提示共用的本地交互门禁，避免显示当前无法执行的操作。 */
	bool CanUseFocusInteraction() const;
	/** 本地预览门禁；服务器仍会独立重新校验相同条件。 */
	bool CanUseBuildingPlacement() const;
	/** Placement Component 的只读库存入口，不开放槽位写操作。 */
	UInventoryComponent* GetInventoryComponentForGameplay() const;
	/** 本地只提交槽位、支撑落点和预览原点；Definition/实例形态/碰撞结果从不跨网传入。 */
	void RequestBuildingPlacement(
		int32 QuickbarIndex,
		const FVector& SurfaceLocation,
		const FVector& ExpectedResolvedLocation,
		uint8 YawQuarterTurns,
		uint16 RequestId);

	/** 本地死亡输入入口；Authority 重新校验 Health 后才允许在 PlayerStart 重生。 */
	void RequestRespawn();

protected:
	/** 水平保持 InputSettings 的基础灵敏度，不额外放大。 */
	UPROPERTY(EditDefaultsOnly, Category = "Camera|Mouse Look", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MouseYawScale = 1.0f;

	/** 纵向屏幕空间更短，单独降速，避免轻微鼠标位移造成镜头俯冲。 */
	UPROPERTY(EditDefaultsOnly, Category = "Camera|Mouse Look", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MousePitchScale = 0.65f;

	/** 第三人称视角允许的最低俯角；避免镜头接近垂直朝下。 */
	UPROPERTY(EditDefaultsOnly, Category = "Camera|Mouse Look",
			  meta = (ClampMin = "-89.0", ClampMax = "0.0", UIMin = "-89.0", UIMax = "0.0", Units = "deg"))
	float ViewPitchMin = -55.0f;

	/** 第三人称视角允许的最高仰角；避免镜头接近垂直朝上。 */
	UPROPERTY(EditDefaultsOnly, Category = "Camera|Mouse Look",
			  meta = (ClampMin = "0.0", ClampMax = "89.0", UIMin = "0.0", UIMax = "89.0", Units = "deg"))
	float ViewPitchMax = 45.0f;

	UPROPERTY(EditDefaultsOnly, Category = "World Object|Throw", meta = (ClampMin = "0.0", Units = "cm/s"))
	double StickThrowForwardSpeed = 1200.0;

	UPROPERTY(EditDefaultsOnly, Category = "World Object|Throw", meta = (Units = "cm/s"))
	double StickThrowUpwardSpeed = 150.0;

	/** 本地玩家始终启用的输入映射。 */
	UPROPERTY()
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/** 1-9、0 对应的十个离散输入资产。 */
	UPROPERTY()
	TArray<TObjectPtr<UInputAction>> QuickbarActions;

	UPROPERTY()
	TObjectPtr<UInputAction> ToggleInventoryAction;

	/** 当前装备道具的主要 Use 输入；通过 InputTag 激活 ASC 中由装备授予的 Ability。 */
	UPROPERTY()
	TObjectPtr<UInputAction> UseEquippedItemAction;

	UPROPERTY()
	TObjectPtr<UInputMappingContext> ItemMappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInventoryHUDWidget> InventoryHUD;

	/** 汇总本地 Query，并按直接瞄准/附近辅助优先级转交交互。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFocusHostComponent> FocusHostComponent;

	/** 当前 demo 开局注册的 Building ECS 空间 Focus Query。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBuildingFocusQueryComponent> BuildingFocusQueryComponent;

	/** 当前 Building Focus 命中 Part 的本地无碰撞高亮外壳。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBuildingFocusHighlightPresenterComponent> BuildingFocusHighlightPresenterComponent;

	/** 独立 Portable 空间索引的附近拾取查询与目标保持。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWorldObjectFocusQueryComponent> WorldObjectFocusQueryComponent;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWorldObjectPickupComponent> WorldObjectPickupComponent;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWorldObjectFocusHighlightPresenterComponent> WorldObjectFocusHighlightPresenterComponent;

	/** 把当前 Handler 解析出的唯一交互提示固定投影到视口中心下方。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFocusPromptPresenterComponent> FocusPromptPresenterComponent;

	/** 只提交本地相机观察数据，不承载任何 ECS 表现策略或状态。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UPresentationViewSourceComponent> PresentationViewSourceComponent;

	/** 为本地预测 Pawn 或服务器权威 Pawn 提交近场碰撞 Source。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBuildingCollisionActivationComponent> BuildingCollisionActivationComponent;

	/** 树木模块独占的本地预测 / Authority 近场碰撞 Source。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USettlementTreeCollisionSourceComponent> SettlementTreeCollisionSourceComponent;

	/** 普通 Dormant WorldObject 的本地预测 / Authority 近场碰撞 Source。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWorldObjectCollisionActivationComponent> WorldObjectCollisionActivationComponent;

	/** 本地红蓝预览与建造输入；最终 Entity 只由服务器创建。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBuildingPlacementComponent> BuildingPlacementComponent;

	/** Owner-only 世界 Chunk 快照、缓存与增量协议端点。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWorldChunkStreamingComponent> WorldChunkStreamingComponent;

	/** 只负责低频采集诊断指标并投影 HUD，不参与 Chunk 协议或 Gameplay 输入。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWorldStreamingHUDPresenterComponent> WorldStreamingHUDPresenterComponent;

	/** 观察 PlayerState ASC，并独占死亡 UI、Fire 封口和 Authority 重生事务。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UPlayerHealthCoordinatorComponent> PlayerHealthCoordinatorComponent;

	/** Owner-only 陨石轨迹页、ACK、兴趣与结算映射端点。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UMeteorStreamingComponent> MeteorStreamingComponent;
	/** WorldStorage 与 Meteor 共用同一连接级令牌、在途窗口和公平队列。 */
	TSharedPtr<UE::ElementSandbox::NetBulk::FConnectionScheduler> BulkTransferScheduler;

	/** 将项目默认映射加入本地玩家的 Enhanced Input 子系统。 */
	virtual void SetupInputComponent() override;
	virtual void BeginPlay() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void OnRep_PlayerState() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void HandleQuickbarSelection(int32 QuickbarIndex);
	void PressUseEquippedItem();
	void ReleaseUseEquippedItem();
	void PressInteract();
	void ReleaseInteract();
	void PressThrowWorldObject();
	void HandleRotateOrRespawn();
	void ToggleInventory();
	void SetInventoryOpen(bool bOpen);
	/** 本地 UI/死亡只拥有一份成对的 Input Ignore token，并统一恢复 GameOnly 鼠标捕获。 */
	void RefreshLocalGameplayInputMode();
	void TryInitializeInventoryUI();
	UInventoryComponent* GetInventoryComponent() const;
	UElementAbilitySystemComponent* GetElementAbilitySystemComponent() const;
	bool IsHealthDepleted() const;
	bool TryThrowSelectedWorldObject();
	EWorldObjectPickupFailure TryPickupWorldObject(FWorldEntityId WorldEntityId);
	bool TryInteractWithDoor(FWorldEntityId WorldEntityId);
	bool TryDismantleBuilding(FWorldEntityId WorldEntityId);
	EBuildPlacementFailure TryPlaceBuilding(
		int32 QuickbarIndex,
		const FVector& SurfaceLocation,
		const FVector& ExpectedResolvedLocation,
		uint8 YawQuarterTurns);

	/** Owner Client 发起一次离散投掷；Authority 重新校验死亡状态、装备和库存后执行，因此使用 Reliable。 */
	UFUNCTION(Server, Reliable)
	void ServerThrowSelectedWorldObject();

	/** Owner Client 只提交稳定 ID；Authority 复验当前距离、遮挡、玩家状态与容量。离散事务使用 Reliable。 */
	UFUNCTION(Server, Reliable)
	void ServerPickupWorldObject(FWorldEntityId WorldEntityId);

	/** Authority 向 Owner 可靠确认本次拾取；只解除请求等待，实体删除仍由正常 Tombstone 同步。 */
	UFUNCTION(Client, Reliable)
	void ClientWorldObjectPickupResult(FWorldEntityId WorldEntityId, EWorldObjectPickupFailure Failure);

	/** 离散门交互使用 Reliable；服务器重新解析 WorldEntityId 并校验玩家状态、距离和 Door 状态。 */
	UFUNCTION(Server, Reliable)
	void ServerRequestDoorInteraction(FWorldEntityId WorldEntityId);

	/** Owner Client 只提交稳定 ID；Authority 重查拆除锤、目标、距离、容量并执行可回滚返还事务。 */
	UFUNCTION(Server, Reliable)
	void ServerRequestBuildingDismantle(FWorldEntityId WorldEntityId);

	/** Owner Client 发起死亡后的离散重生；Authority 重新校验死亡已封口且存在合法 PlayerStart，因此使用 Reliable。 */
	UFUNCTION(Server, Reliable)
	void ServerRequestRespawn();

	/** 离散摆放事务使用 Reliable；服务器不信任客户端的 Definition 或碰撞结果。 */
	UFUNCTION(Server, Reliable)
	void ServerRequestBuildingPlacement(
		int32 QuickbarIndex,
		FVector_NetQuantize10 SurfaceLocation,
		FVector_NetQuantize10 ExpectedResolvedLocation,
		uint8 YawQuarterTurns,
		uint16 RequestId);

	/** Authority 向 Owner Client 回传对应 RequestId 的最终结果；仅关闭本地预测，不改变权威世界状态。 */
	UFUNCTION(Client, Reliable)
	void ClientBuildingPlacementResult(uint16 RequestId, EBuildPlacementFailure Failure);

	/** 背包开关是离散本地 UI 状态；服务器保存门禁镜像，不据此相信任何物品数据。 */
	UFUNCTION(Server, Reliable)
	void ServerSetInventoryOpen(bool bOpen);

	bool bLocalUIOrDeathInputBlocked = false;
	bool bServerInventoryOpen = false;
	double LastBuildingPlacementRequestTime = -DBL_MAX;
	double LastBuildingDismantleRequestTime = -DBL_MAX;

	friend class UPlayerHealthCoordinatorComponent;
};
