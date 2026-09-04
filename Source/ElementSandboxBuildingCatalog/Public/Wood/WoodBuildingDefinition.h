#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildingDefinition.h"

#include "WoodBuildingDefinition.generated.h"

class UMaterialInterface;
class UStaticMesh;

/** 由 Catalog 创建三份实例的基础木制建筑配置。 */
UCLASS(NotBlueprintable)
class ELEMENTSANDBOXBUILDINGCATALOG_API UWoodBuildingDefinition final
	: public UBuildingDefinition
{
	GENERATED_BODY()

public:
	UWoodBuildingDefinition();
	bool Initialize(FName InDefinitionId, const FVector& SizeCentimeters);

protected:
	virtual bool ConfigureEntity(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity) const override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> PrimitiveMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PrimitiveMaterial = nullptr;

	FVector ConfiguredSizeCentimeters = FVector::ZeroVector;
};
