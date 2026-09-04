// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ElementSandboxGameMode.generated.h"

class APlayerController;
class UItemDefinition;

/**
 * 项目默认 GameMode，直接使用 C++ 角色和控制器。
 */
UCLASS()
class AElementSandboxGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	static constexpr int32 DefaultFireballQuickbarIndex = 4;
	static constexpr int32 DefaultFireballQuantity = 999;
	static constexpr int32 DefaultDemolitionToolQuickbarIndex = 5;
	static constexpr int32 DefaultAxeQuickbarIndex = 6;
	static constexpr int32 DefaultMeteorQuickbarIndex = 7;
	static constexpr int32 DefaultMeteorQuantity = 1;

	AElementSandboxGameMode();
	virtual void BeginPlay() override;

	/** 登录完成后由服务器幂等地发放木棍、建造物品、火焰球、拆除锤、斧头与唯一陨石核心。 */
	virtual void PostLogin(APlayerController* NewPlayer) override;

private:
	UPROPERTY()
	TObjectPtr<UItemDefinition> DefaultStickDefinition;

	UPROPERTY()
	TObjectPtr<UItemDefinition> DefaultFireballDefinition;

	UPROPERTY()
	TObjectPtr<UItemDefinition> DefaultDemolitionToolDefinition;

	UPROPERTY()
	TObjectPtr<UItemDefinition> DefaultAxeDefinition;

	UPROPERTY()
	TObjectPtr<UItemDefinition> DefaultMeteorDefinition;

	/** 顺序固定映射到快捷栏 2--4：木墙、木地板、木柱。 */
	UPROPERTY()
	TArray<TObjectPtr<UItemDefinition>> DefaultBuildingItemDefinitions;

};
