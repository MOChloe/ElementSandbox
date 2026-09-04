#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FocusHighlightActor.generated.h"

class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * 本地 Focus 的纯 Mesh 表现载体。它不复制、不碰撞，也不拥有 Gameplay 状态；
 * 独立 Actor 避免高亮 Mesh 继承 PlayerController 的隐藏渲染状态。
 */
UCLASS(NotBlueprintable, NotPlaceable, Transient)
class AFocusHighlightActor final : public AActor
{
	GENERATED_BODY()

public:
	AFocusHighlightActor();

	bool SetHighlightedPart(
		UStaticMesh* Mesh,
		const FTransform& WorldTransform,
		float UniformScale);
	void SetHighlightVisible(bool bVisible);
	bool IsHighlightVisible() const;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> HighlightMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> HighlightMaterial = nullptr;
};
