#pragma once

#include "Commandlets/Commandlet.h"

#include "GenerateMillionSettlementWorldCommandlet.generated.h"

/** 将 Settlement 建筑与可选的一树一结构 WorldObject 离线展开为服务器种子世界。 */
UCLASS()
class UGenerateMillionSettlementWorldCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UGenerateMillionSettlementWorldCommandlet();
	virtual int32 Main(const FString& Params) override;
};
