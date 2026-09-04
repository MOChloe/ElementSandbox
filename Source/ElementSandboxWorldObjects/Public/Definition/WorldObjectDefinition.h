#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldObjectTypes.h"
#include "Definition/WorldDestructionDefinition.h"
#include "Shape/WorldObjectShapeTypes.h"

#include "WorldObjectDefinition.generated.h"

class FWorldObjectEntityRegistry;
class UWorldObjectWorldSubsystem;

/** 每种场景物件共享一份的空间定义；不保存实例状态或表现 Actor。 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXWORLDOBJECTS_API UWorldObjectDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 跨服务器/客户端解析共享定义的稳定标识；运行时不得修改。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Object")
	FName DefinitionId = NAME_None;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Object")
	EWorldObjectSpatialClass SpatialClass = EWorldObjectSpatialClass::Portable;

	/** 空间查询与拾取使用的包络；允许比实际几何更宽。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Object")
	FBox InteractionLocalBounds = FBox(FVector(-25.0), FVector(25.0));

	/** 中性内容标签；宿主不据此推断任何 Element/Fire 资格。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Object|Shape")
	FName SurfaceProfileId = NAME_None;

	/** 独立于 Interaction Bounds、渲染、Actor 与 Chaos 的纯值几何。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Object|Shape")
	FWorldObjectShapeDefinition ShapeGeometry;

	/** 未配置时不可被斧头破坏；当前伤害不属于这份共享配置。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Object|Destruction")
	FWorldDestructionDefinition Destruction;

	bool IsDefinitionValid() const
	{
		return !DefinitionId.IsNone()
			&& InteractionLocalBounds.IsValid != 0
			&& !InteractionLocalBounds.ContainsNaN()
			&& ShapeGeometry.IsValid()
			&& Destruction.IsValid();
	}

protected:
	/**
	 * 核心 Fragment 写入后、空间和复制身份发布前调用。派生 Definition 只能在这里
	 * 写入自己的初始 Fragment；返回 false 会让整次 WorldObject 创建回滚。
	 */
	virtual bool ConfigureEntity(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity) const
	{
		return true;
	}

	friend UWorldObjectWorldSubsystem;
};
