#include "PresentationViewSource.h"

bool FPresentationViewSource::IsValid() const
{
	return !ViewLocation.ContainsNaN()
		&& !SubjectLocation.ContainsNaN()
			&& !Forward.ContainsNaN() && !Forward.IsNearlyZero()
		&& !Right.ContainsNaN() && !Right.IsNearlyZero()
		&& !Up.ContainsNaN() && !Up.IsNearlyZero()
		&& FMath::IsFinite(HorizontalFOVDegrees)
		&& HorizontalFOVDegrees > 1.0f && HorizontalFOVDegrees < 179.0f
		&& FMath::IsFinite(AspectRatio) && AspectRatio > UE_SMALL_NUMBER
		&& ViewportSize.X > 0 && ViewportSize.Y > 0
		&& Revision != 0;
}

float FPresentationViewSource::GetVerticalFOVDegrees() const
{
	const double HalfHorizontalRadians = FMath::DegreesToRadians(HorizontalFOVDegrees * 0.5);
	return static_cast<float>(FMath::RadiansToDegrees(
		2.0 * FMath::Atan(FMath::Tan(HalfHorizontalRadians) / AspectRatio)));
}
