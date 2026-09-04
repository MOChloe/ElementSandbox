#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildingDefinition.h"
#include "Entity/BuildFragment.h"

#include "BuildEntityTestTypes.generated.h"

USTRUCT(meta=(WorldStorageTestFragment))
struct FBuildTestValueFragment : public FBuildFragment
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Value = 0;

	UPROPERTY()
	FString Label;
};

USTRUCT(meta=(WorldStorageTestFragment))
struct FBuildTestTransformFragment : public FBuildFragment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;
};

USTRUCT(meta=(WorldStorageTestFragment))
struct FBuildTestObjectReferenceFragment : public FBuildFragment
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UObject> Object = nullptr;
};

UCLASS()
class UBuildTestDefinition final : public UBuildingDefinition
{
	GENERATED_BODY()

public:
	UBuildTestDefinition();
	int32 InitialValue = 0;
	bool bAllowConfiguration = true;

protected:
	virtual bool ConfigureEntity(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity) const override;
};
