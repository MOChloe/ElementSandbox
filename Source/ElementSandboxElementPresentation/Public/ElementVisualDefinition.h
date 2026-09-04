#pragma once

#include "CoreMinimal.h"
#include "ElementPresentationTypes.h"

class UStaticMesh;
class UMaterialInterface;

/** Visual Catalog 的资源描述；只允许在 Game Thread 注册和读取。 */
struct ELEMENTSANDBOXELEMENTPRESENTATION_API FElementVisualDefinition final
{
	FName DefinitionId = NAME_None;
	TObjectPtr<UStaticMesh> StaticMesh = nullptr;
	TObjectPtr<UMaterialInterface> MaterialOverride = nullptr;
	EElementVisualInstanceBackend Backend = EElementVisualInstanceBackend::Hierarchical;
	int32 CustomDataFloatCount = 0;

	bool IsValid() const;
};
