#pragma once

#include "CoreMinimal.h"

#include "WorldDestructionDefinition.generated.h"

class UWorldObjectDefinition;

/**
 * Building 与 WorldObject 共用的纯破坏配置。
 *
 * 当前伤害属于运行期临时状态；这里只描述满耐久以及破坏成功后应创建的产品。
 * MaxDurability <= 0 或 ProductClass 为空表示该 Definition 不可破坏。
 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldDestructionDefinition final
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction", meta=(ClampMin="0.0"))
	float MaxDurability = 100.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction")
	TSubclassOf<UWorldObjectDefinition> ProductClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction", meta=(ClampMin="1"))
	int32 MinimumProductCount = 3;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction", meta=(ClampMin="1"))
	int32 MaximumProductCount = 6;

	/** 每个产品的独立均匀缩放范围。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction")
	FVector2D UniformScaleRange = FVector2D(0.85, 1.15);

	/** 相对源包围盒中心的随机生成偏移半径。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction")
	FVector SpawnOffsetExtent = FVector(80.0, 80.0, 50.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction")
	FVector2D HorizontalSpeedRange = FVector2D(40.0, 120.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction")
	FVector2D UpwardSpeedRange = FVector2D(80.0, 180.0);

	/** 每轴角速度的绝对值范围，单位为 degrees/second。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction")
	FVector2D AngularSpeedRange = FVector2D(30.0, 120.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Destruction", meta=(ClampMin="0.01"))
	float ProductMassKg = 3.0f;

	bool IsEnabled() const;

	/** 禁用状态本身合法；启用后所有范围必须有限且有序。 */
	bool IsValid() const;
};
