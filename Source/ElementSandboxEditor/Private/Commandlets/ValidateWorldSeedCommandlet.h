#pragma once

#include "Commandlets/Commandlet.h"

#include "ValidateWorldSeedCommandlet.generated.h"

/** 逐 Chunk 校验已经生成的稀疏世界种子，不把整个世界一次性载入内存。 */
UCLASS()
class UValidateWorldSeedCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UValidateWorldSeedCommandlet();
	virtual int32 Main(const FString& Params) override;
};
