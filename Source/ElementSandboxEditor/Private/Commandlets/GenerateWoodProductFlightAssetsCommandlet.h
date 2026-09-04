#pragma once
#include "Commandlets/Commandlet.h"
#include "GenerateWoodProductFlightAssetsCommandlet.generated.h"

/** 克隆规范木质表面，生成 HISM 飞行材质及预编译 WPO 位移档位。 */
UCLASS()
class UGenerateWoodProductFlightAssetsCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UGenerateWoodProductFlightAssetsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
