#pragma once

#include "Collision/BuildCollisionTypes.h"
#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Entity/WorldEntityId.h"
#include "Shape/BuildShapeTypes.h"

/** Render Part 到独立宿主 Shape 的注册结果。 */
enum class EBuildRenderPartShapeAuditStatus : uint8
{
	Registered,
	RuntimeUnavailable,
	InvalidEntity,
	MissingDefinition,
	MissingRenderPart,
	MissingRenderMesh,
	InvalidShapeGeometry
};

/**
 * Building Render Part 的中性 Shape 审计。成功记录和失败记录都保留可获得的稳定身份，
 * 便于从视线命中的 Part 一直追到宿主 Journal，而不把 Render、Element 与 Chaos 混为一体。
 */
struct ELEMENTSANDBOXBUILDING_API FBuildRenderPartShapeAudit final
{
	FName Domain = TEXT("Building");
	FWorldEntityId WorldEntityId;
	FBuildEntityHandle Entity;
	FName DefinitionId = NAME_None;
	int32 MeshPartId = INDEX_NONE;
	FBuildShapeRef ShapeRef;
	FName SurfaceProfileId = NAME_None;
	EBuildRenderPartShapeAuditStatus Status =
		EBuildRenderPartShapeAuditStatus::RuntimeUnavailable;
	FString FailureReason;
};

/** 一个 Collision Part 从内容配置到局部 Chaos Body 的当前结果。 */
enum class EBuildCollisionPartAuditStatus : uint8
{
	ActiveBody,
	CachedBody,
	RuntimeUnavailable,
	InvalidEntity,
	MissingDefinition,
	MissingRenderPart,
	NoCollisionDefinition,
	NoMatchingCollisionPart,
	NotRequired,
	AwaitingSelection,
	PendingBudget,
	AwaitingProjection,
	ProjectionFailure,
	HostUnavailable,
	HostApplyFailure,
	InvalidInstance
};

struct ELEMENTSANDBOXBUILDING_API FBuildCollisionPartAudit final
{
	int32 CollisionPartId = INDEX_NONE;
	bool bRequired = false;
	bool bRetained = false;
	bool bPendingBudget = false;
	FBuildCollisionClusterKey ClusterKey;
	FBuildCollisionInstanceHandle Instance;
	EBuildCollisionProjectionFailure ProjectionFailure =
		EBuildCollisionProjectionFailure::None;
	EBuildCollisionPartAuditStatus Status =
		EBuildCollisionPartAuditStatus::RuntimeUnavailable;
	FString FailureReason;
};

/** Render Part 到 Collision Part、Required 集合及 Host Instance 的完整审计。 */
struct ELEMENTSANDBOXBUILDING_API FBuildRenderPartCollisionAudit final
{
	FName Domain = TEXT("Building");
	FWorldEntityId WorldEntityId;
	FBuildEntityHandle Entity;
	FName DefinitionId = NAME_None;
	int32 MeshPartId = INDEX_NONE;
	FName SurfaceProfileId = NAME_None;
	TArray<FBuildCollisionPartAudit> CollisionParts;
};

ELEMENTSANDBOXBUILDING_API const TCHAR* LexToString(
	EBuildRenderPartShapeAuditStatus Status);
ELEMENTSANDBOXBUILDING_API const TCHAR* LexToString(
	EBuildCollisionPartAuditStatus Status);
ELEMENTSANDBOXBUILDING_API const TCHAR* LexToString(
	EBuildCollisionProjectionFailure Failure);

/** 生成可直接写日志的 Render→Shape 与 Render→Collision 完整诊断。 */
ELEMENTSANDBOXBUILDING_API FString FormatBuildRenderPartRegistrationAudit(
	const FBuildRenderPartShapeAudit& ShapeAudit,
	const FBuildRenderPartCollisionAudit& CollisionAudit);
