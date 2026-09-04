#pragma once

#include "ElementPresentationTypes.h"
#include "Subsystems/WorldSubsystem.h"

#include "ElementFirePresentationWorldSubsystem.generated.h"

class UMaterialInterface;
class UStaticMesh;

/** 将 Simulation 的只读 Visual Journal 接到客户端实例池，并注册首轮 Fire 表现资源。 */
UCLASS()
class UElementFirePresentationWorldSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	FElementVisualSourceHandle VisualSourceHandle;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> FlameMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FlameMaterial = nullptr;
};
