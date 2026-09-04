#pragma once

#include "CoreMinimal.h"
#include "Visual/ElementVisualTypes.h"

enum class EElementVisualApplyCommandKind : uint8
{
	Upsert,
	Remove
};

struct FElementVisualApplyCommand final
{
	EElementVisualApplyCommandKind Kind = EElementVisualApplyCommandKind::Upsert;
	FElementVisualShardKey Shard;
	FElementVisualKey Key;
	FElementVisualDescriptor Descriptor;
	uint64 TargetRevision = 0;
	uint64 CoverageToken = 0;
	uint64 CatalogGeneration = 0;
	int32 RetryCount = 0;
};

struct FElementVisualBuildRequest final
{
	FElementVisualShardKey Shard;
	uint64 TargetRevision = 0;
	uint64 CoverageToken = 0;
	uint64 CatalogGeneration = 0;
	TSharedPtr<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> Target;
	TSharedPtr<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> Applied;
};

struct FElementVisualBuildResult final
{
	FElementVisualShardKey Shard;
	uint64 TargetRevision = 0;
	uint64 CoverageToken = 0;
	uint64 CatalogGeneration = 0;
	TArray<FElementVisualApplyCommand> Commands;
	bool bSucceeded = false;
};

FElementVisualBuildResult BuildElementVisualDelta(const FElementVisualBuildRequest& Request);
