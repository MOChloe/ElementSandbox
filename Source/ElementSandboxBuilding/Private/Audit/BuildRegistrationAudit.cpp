#include "Audit/BuildRegistrationAudit.h"

#include "Engine/StaticMesh.h"

const TCHAR* LexToString(const EBuildRenderPartShapeAuditStatus Status)
{
	switch (Status)
	{
	case EBuildRenderPartShapeAuditStatus::Registered:
		return TEXT("Registered");
	case EBuildRenderPartShapeAuditStatus::RuntimeUnavailable:
		return TEXT("RuntimeUnavailable");
	case EBuildRenderPartShapeAuditStatus::InvalidEntity:
		return TEXT("InvalidEntity");
	case EBuildRenderPartShapeAuditStatus::MissingDefinition:
		return TEXT("MissingDefinition");
	case EBuildRenderPartShapeAuditStatus::MissingRenderPart:
		return TEXT("MissingRenderPart");
	case EBuildRenderPartShapeAuditStatus::MissingRenderMesh:
		return TEXT("MissingRenderMesh");
	case EBuildRenderPartShapeAuditStatus::InvalidShapeGeometry:
		return TEXT("InvalidShapeGeometry");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* LexToString(const EBuildCollisionPartAuditStatus Status)
{
	switch (Status)
	{
	case EBuildCollisionPartAuditStatus::ActiveBody:
		return TEXT("ActiveBody");
	case EBuildCollisionPartAuditStatus::CachedBody:
		return TEXT("CachedBody");
	case EBuildCollisionPartAuditStatus::RuntimeUnavailable:
		return TEXT("RuntimeUnavailable");
	case EBuildCollisionPartAuditStatus::InvalidEntity:
		return TEXT("InvalidEntity");
	case EBuildCollisionPartAuditStatus::MissingDefinition:
		return TEXT("MissingDefinition");
	case EBuildCollisionPartAuditStatus::MissingRenderPart:
		return TEXT("MissingRenderPart");
	case EBuildCollisionPartAuditStatus::NoCollisionDefinition:
		return TEXT("NoCollisionDefinition");
	case EBuildCollisionPartAuditStatus::NoMatchingCollisionPart:
		return TEXT("NoMatchingCollisionPart");
	case EBuildCollisionPartAuditStatus::NotRequired:
		return TEXT("NotRequired");
	case EBuildCollisionPartAuditStatus::AwaitingSelection:
		return TEXT("AwaitingSelection");
	case EBuildCollisionPartAuditStatus::PendingBudget:
		return TEXT("PendingBudget");
	case EBuildCollisionPartAuditStatus::AwaitingProjection:
		return TEXT("AwaitingProjection");
	case EBuildCollisionPartAuditStatus::ProjectionFailure:
		return TEXT("ProjectionFailure");
	case EBuildCollisionPartAuditStatus::HostUnavailable:
		return TEXT("HostUnavailable");
	case EBuildCollisionPartAuditStatus::HostApplyFailure:
		return TEXT("HostApplyFailure");
	case EBuildCollisionPartAuditStatus::InvalidInstance:
		return TEXT("InvalidInstance");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* LexToString(const EBuildCollisionProjectionFailure Failure)
{
	switch (Failure)
	{
	case EBuildCollisionProjectionFailure::None:
		return TEXT("None");
	case EBuildCollisionProjectionFailure::MissingDefinitionOrTransform:
		return TEXT("MissingDefinitionOrTransform");
	case EBuildCollisionProjectionFailure::InvalidCollisionPart:
		return TEXT("InvalidCollisionPart");
	case EBuildCollisionProjectionFailure::InvalidClusterConfiguration:
		return TEXT("InvalidClusterConfiguration");
	case EBuildCollisionProjectionFailure::InvalidCollisionGeometry:
		return TEXT("InvalidCollisionGeometry");
	case EBuildCollisionProjectionFailure::HostRemoveFailed:
		return TEXT("HostRemoveFailed");
	case EBuildCollisionProjectionFailure::HostAddFailed:
		return TEXT("HostAddFailed");
	case EBuildCollisionProjectionFailure::HostUpdateFailed:
		return TEXT("HostUpdateFailed");
	case EBuildCollisionProjectionFailure::ProcessorStateInvalid:
		return TEXT("ProcessorStateInvalid");
	default:
		return TEXT("Unknown");
	}
}

FString FormatBuildRenderPartRegistrationAudit(
	const FBuildRenderPartShapeAudit& ShapeAudit,
	const FBuildRenderPartCollisionAudit& CollisionAudit)
{
	FString Report = FString::Printf(
		TEXT("Domain=%s WorldEntityId=%llu Entity={Registry=%u,Slot=%d,Generation=%u} ")
		TEXT("DefinitionId=%s MeshPartId=%d SurfaceProfileId=%s ")
		TEXT("Shape={Status=%s,PartId=%d,ShapeId=%u} ShapeFailure=\"%s\""),
		*ShapeAudit.Domain.ToString(),
		ShapeAudit.WorldEntityId.GetValue(),
		ShapeAudit.Entity.GetRegistryId(),
		ShapeAudit.Entity.GetIndex(),
		ShapeAudit.Entity.GetGeneration(),
		*ShapeAudit.DefinitionId.ToString(),
		ShapeAudit.MeshPartId,
		*ShapeAudit.SurfaceProfileId.ToString(),
		LexToString(ShapeAudit.Status),
		ShapeAudit.ShapeRef.PartId,
		ShapeAudit.ShapeRef.ShapeId,
		*ShapeAudit.FailureReason);

	for (const FBuildCollisionPartAudit& Part : CollisionAudit.CollisionParts)
	{
		const FString MeshName = Part.ClusterKey.Mesh
			? Part.ClusterKey.Mesh->GetPathName()
			: TEXT("None");
		Report += FString::Printf(
			TEXT(" Collision={CollisionPartId=%d,Required=%s,Retained=%s,PendingBudget=%s,")
			TEXT("Cluster={Mesh=%s,Mobility=%d,Profile=%s},")
			TEXT("Instance={Host=%u,Slot=%d,Generation=%u},Status=%s,ProjectionFailure=%s,")
			TEXT("Reason=\"%s\"}"),
			Part.CollisionPartId,
			Part.bRequired ? TEXT("true") : TEXT("false"),
			Part.bRetained ? TEXT("true") : TEXT("false"),
			Part.bPendingBudget ? TEXT("true") : TEXT("false"),
			*MeshName,
			static_cast<int32>(Part.ClusterKey.Mobility),
			*Part.ClusterKey.CollisionProfileName.ToString(),
			Part.Instance.GetHostId(),
			Part.Instance.GetIndex(),
			Part.Instance.GetGeneration(),
			LexToString(Part.Status),
			LexToString(Part.ProjectionFailure),
			*Part.FailureReason);
	}
	return Report;
}
