#pragma once

#include "CoreMinimal.h"
#include "Shape/BuildShapeTypes.h"

#include "BuildMeshPartDefinition.generated.h"

class UStaticMesh;
class UMaterialInterface;

/** Mesh Part 的长期表现策略；运行时冷热状态不写回 Definition。 */
UENUM()
enum class EBuildMeshPartPresentationPolicy : uint8
{
	/** 永久进入稳定 HISM，不参与邻近激活。 */
	Static,

	/** 远处进入 Cold HISM，邻近或运动时迁移到 Hot ISM。 */
	ProximityPromotable
};

/** 同类建筑共享的一个 Mesh Part 配置，不代表独立 Building Entity。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildMeshPartDefinition
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Building|Rendering")
	TObjectPtr<UStaticMesh> Mesh = nullptr;

	/** 可选的第 0 材质槽覆盖；为空时使用 Static Mesh 自带材质。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Rendering")
	TObjectPtr<UMaterialInterface> MaterialOverride = nullptr;

	UPROPERTY(EditDefaultsOnly, Category="Building|Rendering")
	FTransform LocalTransform = FTransform::Identity;

	/**
	 * 中性表面内容标签。它只描述内容来源，不代表可燃、可碰撞或任何 Element 资格。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Shape")
	FName SurfaceProfileId = NAME_None;

	/** 独立于 Render Mesh 与 Chaos Collision 的稳定宿主 Shape。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Shape")
	FBuildPartShapeDefinition Shape;

	UPROPERTY(EditDefaultsOnly, Category="Building|Rendering")
	EBuildMeshPartPresentationPolicy PresentationPolicy =
		EBuildMeshPartPresentationPolicy::Static;

	/** Cluster 的 PerInstance Custom Data float 数；同一 Cluster 必须完全一致。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Rendering", meta=(ClampMin="0", ClampMax="8"))
	int32 CustomDataFloatCount = 0;
};
