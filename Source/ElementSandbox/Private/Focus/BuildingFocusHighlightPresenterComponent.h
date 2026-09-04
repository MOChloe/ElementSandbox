#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Entity/BuildEntityHandle.h"

#include "BuildingFocusHighlightPresenterComponent.generated.h"

class AFocusHighlightActor;
class UStaticMesh;

/**
 * 把当前本地 Building Focus 的精确命中 Mesh Part 投影为一层无碰撞高亮外壳。
 * 它不修改共享 HISM/ISM，也不保存或复制 Gameplay 状态。
 */
UCLASS(NotBlueprintable, ClassGroup=(Focus))
class UBuildingFocusHighlightPresenterComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildingFocusHighlightPresenterComponent();

	/** Focus Host 完成本帧查询后刷新高亮；公开入口也供确定性测试使用。 */
	void RefreshHighlight();
	bool IsHighlightVisible() const;
	const AFocusHighlightActor* GetHighlightActor() const { return HighlightActor; }
	FBuildEntityHandle GetHighlightedEntity() const { return HighlightedEntity; }
	int32 GetHighlightedPartId() const { return HighlightedPartId; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool EnsureHighlightActor();
	bool TryResolveFocusedPart(
		UStaticMesh*& OutMesh,
		FTransform& OutWorldTransform,
		FBuildEntityHandle& OutEntity,
		int32& OutPartId) const;
	void HideHighlight();

	/** 略微放大命中 Mesh，避免覆盖层与原实例发生 Z-fighting。 */
	UPROPERTY(EditDefaultsOnly, Category="Focus|Highlight",
		meta=(ClampMin="1.001", ClampMax="1.100", UIMin="1.001", UIMax="1.100"))
	float HighlightScale = 1.01f;

	UPROPERTY(Transient)
	TObjectPtr<AFocusHighlightActor> HighlightActor = nullptr;

	FBuildEntityHandle HighlightedEntity;
	int32 HighlightedPartId = INDEX_NONE;
};
