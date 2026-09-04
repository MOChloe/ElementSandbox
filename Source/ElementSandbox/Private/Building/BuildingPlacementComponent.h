#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Placement/BuildPlacementTypes.h"

#include "BuildingPlacementComponent.generated.h"

class ABuildPlacementPreviewActor;
class AElementSandboxPlayerController;
class UBuildingDefinition;
class UBuildingWorldSubsystem;
class UInputAction;
class UInputComponent;
class UInputMappingContext;
class UInteractionPromptWidget;
class UInventoryComponent;
class UItemInstance;
struct FInputActionValue;

/**
 * 本地 PlayerController 拥有的建造意图与预览。最终 Building 从不在这里创建；
	 * 左键只把槽位、有限精度候选和旋转提交给服务器。
 */
UCLASS(ClassGroup=(Building), meta=(BlueprintSpawnableComponent))
class UBuildingPlacementComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildingPlacementComponent();

	void BindInput(UInputComponent& InputComponent);
	bool IsPlacementActive() const { return bPlacementActive; }
	/** R 键的可靠离散入口；返回 false 时 PlayerController 可以继续处理复活。 */
	bool TryRotateClockwise();

	/** 对应请求完成后解除防连点；物品复制回来后会自动刷新建造模式。 */
	void HandlePlacementResult(uint16 RequestId, EBuildPlacementFailure Failure);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void TryBindInventory();
	void UnbindInventory();
	void HandleInventoryChanged();
	void RefreshPlacementMode();
	void EnterPlacementMode(
		UItemInstance& Item,
		int32 QuickbarIndex,
		UBuildingDefinition& Definition,
		const FTransform& PlacementShapeTransform);
	void ExitPlacementMode();
	void RefreshPreview();
	void SetPlacementInputEnabled(bool bEnabled);
	void SuspendFocus(bool bSuspend);
	void ApplyRotationStep(int32 Direction);
	bool EnsurePlacementPromptWidget();
	void RefreshPlacementPrompt();
	void HidePlacementPrompt();
	void HandleConfirm();
	void HandleCancel();
	void HandleRotate(const FInputActionValue& Value);
	bool TryGetSelectedBuildItem(
		UItemInstance*& OutItem,
		int32& OutQuickbarIndex,
		UBuildingDefinition*& OutDefinition,
		FTransform& OutPlacementShapeTransform) const;
	bool TryFindViewSurface(
		UBuildingWorldSubsystem& BuildingSubsystem,
		FVector& OutExpectedLocation) const;

	UPROPERTY(Transient)
	TObjectPtr<ABuildPlacementPreviewActor> PreviewActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> PlacementMappingContext = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ConfirmAction = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> CancelAction = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RotateAction = nullptr;

	/** 固定在快捷栏上方，只投影 Placement Component 提交的文字。 */
	UPROPERTY(Transient)
	TObjectPtr<UInteractionPromptWidget> PlacementPromptWidget = nullptr;

	TWeakObjectPtr<UInventoryComponent> BoundInventory;
	TWeakObjectPtr<UItemInstance> ActiveItem;
	TWeakObjectPtr<UItemInstance> CancelledItem;
	TWeakObjectPtr<UBuildingDefinition> ActiveDefinition;
	FDelegateHandle InventoryChangedHandle;
	FBuildPlacementEvaluation CurrentEvaluation;
	FVector CurrentExpectedLocation = FVector::ZeroVector;
	FTransform ActivePlacementShapeTransform = FTransform::Identity;
	FBuildPlacementEvaluation CachedEvaluation;
	FTransform CachedCandidateTransform = FTransform::Identity;
	TWeakObjectPtr<UBuildingDefinition> CachedDefinition;
	uint64 CachedSpatialRevision = 0;
	double CachedEvaluationTime = -DBL_MAX;
	int32 ActiveQuickbarIndex = INDEX_NONE;
	uint8 YawQuarterTurns = 0;
	uint8 CachedYawQuarterTurns = 0;
	uint16 NextRequestId = 1;
	uint16 PendingRequestId = 0;
	/** 服务器拒绝会短暂覆盖本地预测提示，避免失败 RPC 被下一帧预览吞掉。 */
	EBuildPlacementFailure LastAuthorityFailure = EBuildPlacementFailure::None;
	double LastAuthorityFailureExpiryTime = -DBL_MAX;
	bool bPlacementActive = false;
	bool bPlacementInputEnabled = false;
	bool bHasCachedEvaluation = false;
	bool bHasCurrentExpectedLocation = false;
	bool bPlacementPromptSnapshotValid = false;
	uint8 PromptedYawQuarterTurns = 0;
	EBuildPlacementFailure PromptedFailure = EBuildPlacementFailure::InvalidTransform;
	TWeakObjectPtr<UItemInstance> PromptedItem;
};
