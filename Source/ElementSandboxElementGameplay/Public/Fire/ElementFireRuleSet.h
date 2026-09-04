#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Fire/FireCombustionTypes.h"

#include "ElementFireRuleSet.generated.h"

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXELEMENTGAMEPLAY_API FElementFireSourceRule final
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.0"))
	double Intensity = 1.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire", meta=(ClampMin="0.0"))
	double RangeCentimeters = 100.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire")
	EFirePropagationPolicy Policy = EFirePropagationPolicy::All;
};

/** 加载 DataAsset 后冻结的 Worker 纯值；运行期不再读取 UObject。 */
struct ELEMENTSANDBOXELEMENTGAMEPLAY_API FFireRuleSnapshot final
{
	uint64 Revision = 0;
	FFireCombustionProfile Structure;
	FFireCombustionProfile Stick;
	FFireCombustionProfile Character;
	FElementFireSourceRule FirePile;
	FElementFireSourceRule MountedTorch;
	FElementFireSourceRule Fireball;
	FVector FirePileCapsuleCenter = FVector::ZeroVector;
	double FirePileCapsuleRadius = 0.0;
	double FirePileCapsuleSegmentHalfLength = 0.0;
	FVector MountedTorchSphereCenter = FVector::ZeroVector;
	double MountedTorchSphereRadius = 0.0;
	double FireballSphereRadius = 0.0;
	int64 FireballLifetimeMilliseconds = 0;
	double StackTwoEnterIntensity = 0.0;
	double StackTwoExitIntensity = 0.0;
	double StackThreeEnterIntensity = 0.0;
	double StackThreeExitIntensity = 0.0;
	double BaseDamagePerPeriod = 0.0;
	double DamagePeriodSeconds = 0.0;

	bool IsValid(FString* OutError = nullptr) const;
};

/** 火焰数值唯一策划入口；所有字段加载时严格校验后冻结。 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXELEMENTGAMEPLAY_API UElementFireRuleSet final : public UDataAsset
{
	GENERATED_BODY()

public:
	UElementFireRuleSet();

	bool Freeze(FFireRuleSnapshot& OutSnapshot, FString& OutError) const;
	static const TCHAR* GetDefaultAssetPath()
	{
		return TEXT("/Game/Elements/DA_ElementFireRuleSet.DA_ElementFireRuleSet");
	}

	UPROPERTY(EditDefaultsOnly, Category="Fire|Targets")
	FFireCombustionProfile StructureProfile;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Targets")
	FFireCombustionProfile StickProfile;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Targets")
	FFireCombustionProfile CharacterProfile;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources")
	FElementFireSourceRule FirePileSource;

	/** 挂墙 Building 形态的安全火源；固定排除整个 Building 域。 */
	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources")
	FElementFireSourceRule MountedTorchSource;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources")
	FElementFireSourceRule FireballSource;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources")
	FVector FirePileCapsuleCenter = FVector(0.0, 0.0, 82.0);

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources", meta=(ClampMin="0.0"))
	double FirePileCapsuleRadius = 30.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources", meta=(ClampMin="0.0"))
	double FirePileCapsuleSegmentHalfLength = 35.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources")
	FVector MountedTorchSphereCenter = FVector(-24.0, 0.0, 92.0);

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources", meta=(ClampMin="0.0"))
	double MountedTorchSphereRadius = 18.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources", meta=(ClampMin="0.0"))
	double FireballSphereRadius = 18.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Sources", meta=(ClampMin="1"))
	int64 FireballLifetimeMilliseconds = 3000;

	UPROPERTY(EditDefaultsOnly, Category="Fire|GAS", meta=(ClampMin="0.0"))
	double StackTwoEnterIntensity = 0.75;

	UPROPERTY(EditDefaultsOnly, Category="Fire|GAS", meta=(ClampMin="0.0"))
	double StackTwoExitIntensity = 0.60;

	UPROPERTY(EditDefaultsOnly, Category="Fire|GAS", meta=(ClampMin="0.0"))
	double StackThreeEnterIntensity = 2.00;

	UPROPERTY(EditDefaultsOnly, Category="Fire|GAS", meta=(ClampMin="0.0"))
	double StackThreeExitIntensity = 1.50;

	UPROPERTY(EditDefaultsOnly, Category="Fire|GAS", meta=(ClampMin="0.0"))
	double BaseDamagePerPeriod = 1.0;

	UPROPERTY(EditDefaultsOnly, Category="Fire|GAS", meta=(ClampMin="0.0"))
	double DamagePeriodSeconds = 0.5;

	UPROPERTY(EditDefaultsOnly, Category="Fire|Version", meta=(ClampMin="1"))
	uint64 RuleRevision = 2;
};
