#pragma once
#include "CoreMinimal.h"
#include "MeteorClientRuntime.h"
#include "Subsystems/WorldSubsystem.h"
#include "MeteorPresentationWorldSubsystem.generated.h"

/** ClientOnly 协议适配层。全部实例由 WoodProductPresentation 持有，不拥有渲染资源。 */
UCLASS()
class ELEMENTSANDBOXMETEORPRESENTATION_API UMeteorPresentationWorldSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
protected:
	virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
private:
	void HandleChanges(TConstArrayView<UE::ElementSandbox::Meteor::FMeteorClientPresentationLane> Changes);
	void HandleRetired(UE::ElementSandbox::Meteor::FMeteorBurstId Burst);
	FDelegateHandle ChangesHandle, RetiredHandle;
};
