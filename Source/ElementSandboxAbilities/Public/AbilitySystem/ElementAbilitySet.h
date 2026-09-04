#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ElementAbilitySet.generated.h"

class UGameplayAbility;

/** 一条服务器授予规则；InputTag 会随 AbilitySpec 复制到所属客户端。 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXABILITIES_API FElementAbilityGrant
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Ability")
	TSubclassOf<UGameplayAbility> Ability;

	/** 无输入的被动 Ability 可以留空；主动 Ability 使用此 Tag 接收 Enhanced Input。 */
	UPROPERTY(EditDefaultsOnly, Category="Ability", meta=(Categories="Input"))
	FGameplayTag InputTag;

	UPROPERTY(EditDefaultsOnly, Category="Ability", meta=(ClampMin="1"))
	int32 AbilityLevel = 1;
};

/** 可由装备等外部来源成组授予并按返回 Handle 精确回收的 Ability 配置。 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXABILITIES_API FElementAbilitySet
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Ability")
	TArray<FElementAbilityGrant> Abilities;
};
