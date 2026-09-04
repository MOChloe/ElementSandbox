#pragma once

#include "Commandlets/Commandlet.h"

#include "GenerateElementFireRuleSetCommandlet.generated.h"

/** 首次生成新版 Fire Gameplay 唯一规则资产；已存在时拒绝覆盖策划调参。 */
UCLASS()
class UGenerateElementFireRuleSetCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UGenerateElementFireRuleSetCommandlet();
	virtual int32 Main(const FString& Params) override;
};
