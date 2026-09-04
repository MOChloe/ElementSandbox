#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PresentationViewSource.h"

#include "PresentationViewSourceComponent.generated.h"

/** 本地相机观察数据提交器；不知道 Building、Character 或任何驻留策略。 */
UCLASS(NotBlueprintable, ClassGroup=(Presentation))
class UPresentationViewSourceComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UPresentationViewSourceComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool TryBuildViewSource(FPresentationViewSource& OutSource);
	void RememberPublishedView(const FPresentationViewSource& Source);
	FPresentationSourceHandle SourceHandle;
	uint64 NextSourceRevision = 1;
	FVector LastViewLocation = FVector::ZeroVector;
	FVector LastSubjectLocation = FVector::ZeroVector;
	FVector LastForward = FVector::ForwardVector;
	FVector LastRight = FVector::RightVector;
	FVector LastUp = FVector::UpVector;
	float LastHorizontalFOVDegrees = 90.0f;
	float LastAspectRatio = 16.0f / 9.0f;
	FIntPoint LastViewportSize = FIntPoint::ZeroValue;
	bool bHasPublishedView = false;
};
