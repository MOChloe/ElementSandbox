#pragma once

#include "Commandlets/Commandlet.h"

#include "GenerateSettlementTreeAssetsCommandlet.generated.h"

/** 生成树 HISM 材质与固定木块 Static Mesh。 */
UCLASS()
class UGenerateSettlementTreeAssetsCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UGenerateSettlementTreeAssetsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
