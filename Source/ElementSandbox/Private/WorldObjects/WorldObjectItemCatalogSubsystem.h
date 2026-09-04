#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "WorldObjectItemCatalogSubsystem.generated.h"

class UItemDefinition;
class UWorldObjectDefinition;

/**
 * 装配层唯一的 WorldObject ↔ Item 对照表。
 * Items 与 WorldObjects 两个底层模块都不知道对方存在。
 */
UCLASS()
class UWorldObjectItemCatalogSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	UWorldObjectDefinition* FindWorldObjectDefinition(const UObject* ItemDefinition) const;
	UItemDefinition* FindItemDefinition(const UWorldObjectDefinition* WorldObjectDefinition) const;
	bool IsReady() const;

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UItemDefinition> StickItemDefinition;

	UPROPERTY(Transient)
	TObjectPtr<UWorldObjectDefinition> StickWorldObjectDefinition;

	UPROPERTY(Transient)
	TObjectPtr<UItemDefinition> WoodBlockItemDefinition;

	UPROPERTY(Transient)
	TObjectPtr<UWorldObjectDefinition> WoodBlockWorldObjectDefinition;

	UPROPERTY(Transient)
	TObjectPtr<UItemDefinition> CharcoalItemDefinition;

	UPROPERTY(Transient)
	TObjectPtr<UWorldObjectDefinition> CharcoalWorldObjectDefinition;

	bool bDefinitionsRegistered = false;
};
