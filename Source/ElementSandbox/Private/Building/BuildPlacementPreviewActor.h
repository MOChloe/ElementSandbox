#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BuildPlacementPreviewActor.generated.h"

class UBuildingDefinition;
class UMaterialInterface;
class USceneComponent;
class UStaticMeshComponent;

/**
 * 本地建造预览的纯表现 Actor。它不复制、不碰撞，也不创建 Building Entity；
 * Definition 变化时才重建组件，普通 Tick 只改变 Transform、颜色和可见性。
 */
UCLASS(NotBlueprintable, NotPlaceable, Transient)
class ABuildPlacementPreviewActor final : public AActor
{
	GENERATED_BODY()

public:
	ABuildPlacementPreviewActor();

	bool SetDefinition(const UBuildingDefinition* Definition);
	void SetPlacementState(const FTransform& WorldTransform, bool bAllowed);
	void SetPreviewVisible(bool bVisible);

private:
	void ApplyPreviewMaterial(bool bAllowed);
	void ClearMeshComponents();

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> SceneRoot = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> MeshComponents;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> AllowedMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BlockedMaterial = nullptr;

	TWeakObjectPtr<const UBuildingDefinition> CurrentDefinition;
	bool bCurrentAllowed = false;
};
