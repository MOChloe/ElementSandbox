#include "Async/ElementVisualBuild.h"

FElementVisualBuildResult BuildElementVisualDelta(const FElementVisualBuildRequest& Request)
{
	FElementVisualBuildResult Result;
	Result.Shard = Request.Shard;
	Result.TargetRevision = Request.TargetRevision;
	Result.CoverageToken = Request.CoverageToken;
	Result.CatalogGeneration = Request.CatalogGeneration;
	if (!Request.Target.IsValid() || !Request.Applied.IsValid())
	{
		return Result;
	}

	TMap<FElementVisualKey, FElementVisualDescriptor> RemainingApplied;
	RemainingApplied.Reserve(Request.Applied->Num());
	for (const FElementVisualDescriptor& Descriptor : *Request.Applied)
	{
		RemainingApplied.Add(Descriptor.Key, Descriptor);
	}
	for (const FElementVisualDescriptor& Descriptor : *Request.Target)
	{
		const FElementVisualDescriptor* Existing = RemainingApplied.Find(Descriptor.Key);
		if (!Existing || !Descriptor.IsEquivalent(*Existing))
		{
			FElementVisualApplyCommand& Command = Result.Commands.AddDefaulted_GetRef();
			Command.Kind = EElementVisualApplyCommandKind::Upsert;
			Command.Shard = Request.Shard;
			Command.Key = Descriptor.Key;
			Command.Descriptor = Descriptor;
			Command.TargetRevision = Request.TargetRevision;
			Command.CoverageToken = Request.CoverageToken;
			Command.CatalogGeneration = Request.CatalogGeneration;
		}
		RemainingApplied.Remove(Descriptor.Key);
	}
	TArray<FElementVisualKey> RemovalKeys;
	RemainingApplied.GenerateKeyArray(RemovalKeys);
	RemovalKeys.Sort();
	for (const FElementVisualKey& Key : RemovalKeys)
	{
		FElementVisualApplyCommand& Command = Result.Commands.AddDefaulted_GetRef();
		Command.Kind = EElementVisualApplyCommandKind::Remove;
		Command.Shard = Request.Shard;
		Command.Key = Key;
		Command.TargetRevision = Request.TargetRevision;
		Command.CoverageToken = Request.CoverageToken;
		Command.CatalogGeneration = Request.CatalogGeneration;
	}
	Result.bSucceeded = true;
	return Result;
}
