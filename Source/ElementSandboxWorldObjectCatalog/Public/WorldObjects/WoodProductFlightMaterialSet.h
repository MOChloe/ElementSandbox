#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoodProductFlightMaterialSet.generated.h"

class UMaterialInterface;
class UStaticMesh;
USTRUCT()
struct FWoodProductDisplacementMaterials
{
	GENERATED_BODY()
	UPROPERTY() float MaximumDisplacement = 0.0f;
	UPROPERTY() TObjectPtr<UMaterialInterface> Wood;
	UPROPERTY() TObjectPtr<UMaterialInterface> Charcoal;
};

/** Editor Commandlet 生成的原生材质排列；运行时只选档，不修改编译属性。 */
UCLASS()
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API UWoodProductFlightMaterialSet final : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY() TObjectPtr<UStaticMesh> Mesh;
	UPROPERTY() TObjectPtr<UMaterialInterface> StaticWood;
	UPROPERTY() TObjectPtr<UMaterialInterface> StaticCharcoal;
	UPROPERTY() TArray<FWoodProductDisplacementMaterials> Tiers;
	int32 FindTier(float Extent) const;
	UMaterialInterface* GetMaterial(bool bCharcoal, int32 Tier) const;
	static int32 ComputeTier(float Extent);
	static float GetTierExtent(int32 Tier);
};
