#include "Torch/TorchDefinition.h"

namespace
{
	const FName MountedTorchBuildingDefinitionId(TEXT("Torch.Mounted.Building"));
}

UTorchDefinition::UTorchDefinition()
{
	ContentId = TEXT("Torch");
	FTorchFormBinding& Mounted = Forms.AddDefaulted_GetRef();
	Mounted.Form = ETorchForm::MountedBuilding;
	Mounted.DefinitionId = MountedTorchBuildingDefinitionId;
}

bool UTorchDefinition::IsValid(FString* OutError) const
{
	if (ContentId.IsNone() || Forms.IsEmpty())
	{
		if (OutError) *OutError = TEXT("火把内容必须配置 ContentId 和至少一种生产形态。");
		return false;
	}

	TSet<ETorchForm> SeenForms;
	TSet<FName> SeenDefinitionIds;
	for (const FTorchFormBinding& Binding : Forms)
	{
		if (Binding.DefinitionId.IsNone()
			|| SeenForms.Contains(Binding.Form)
			|| SeenDefinitionIds.Contains(Binding.DefinitionId))
		{
			if (OutError)
			{
				*OutError = TEXT("火把形态必须使用非空且不重复的 Form/DefinitionId。");
			}
			return false;
		}
		SeenForms.Add(Binding.Form);
		SeenDefinitionIds.Add(Binding.DefinitionId);
	}
	return true;
}

bool UTorchDefinition::TryResolveForm(
	const ETorchForm Form,
	FName& OutDefinitionId) const
{
	OutDefinitionId = NAME_None;
	for (const FTorchFormBinding& Binding : Forms)
	{
		if (Binding.Form == Form && !Binding.DefinitionId.IsNone())
		{
			OutDefinitionId = Binding.DefinitionId;
			return true;
		}
	}
	return false;
}

FName GetMountedTorchBuildingDefinitionId()
{
	FName DefinitionId;
	const UTorchDefinition* Torch = GetDefault<UTorchDefinition>();
	return Torch && Torch->TryResolveForm(ETorchForm::MountedBuilding, DefinitionId)
		? DefinitionId
		: NAME_None;
}
