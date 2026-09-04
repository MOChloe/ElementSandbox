// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMeshActor.h"
#include "ElementSandboxDemoFallbackGround.generated.h"

/**
 * 测试地图缺少正式地形时由 Authority 创建的临时落脚面。
 *
 * StaticMesh 与碰撞必须在构造阶段配置。运行中的 Static 组件会拒绝
 * SetStaticMesh，因此不能在 GameMode::BeginPlay 中临时给空 Actor 装网格。
 */
UCLASS(NotBlueprintable, Transient)
class AElementSandboxDemoFallbackGround final : public AStaticMeshActor
{
	GENERATED_BODY()

public:
	AElementSandboxDemoFallbackGround();
};
