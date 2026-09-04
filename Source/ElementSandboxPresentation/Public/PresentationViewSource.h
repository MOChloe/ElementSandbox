#pragma once

#include "CoreMinimal.h"
#include "ConvexVolume.h"

class UPresentationWorldSubsystem;

/** 每 World 注册的本地观察源句柄；Generation 防止注销后的旧引用命中新来源。 */
struct ELEMENTSANDBOXPRESENTATION_API FPresentationSourceHandle final
{
public:
	FPresentationSourceHandle() = default;

	bool IsSet() const { return WorldId != 0 && Index != INDEX_NONE && Generation != 0; }
	int32 GetIndex() const { return Index; }
	uint32 GetGeneration() const { return Generation; }
	uint32 GetWorldId() const { return WorldId; }

	friend bool operator==(const FPresentationSourceHandle& Left, const FPresentationSourceHandle& Right)
	{
		return Left.WorldId == Right.WorldId
			&& Left.Index == Right.Index
			&& Left.Generation == Right.Generation;
	}
	friend bool operator!=(const FPresentationSourceHandle& Left, const FPresentationSourceHandle& Right)
	{
		return !(Left == Right);
	}
	friend uint32 GetTypeHash(const FPresentationSourceHandle& Handle)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(Handle.WorldId), GetTypeHash(Handle.Index)),
			GetTypeHash(Handle.Generation));
	}

private:
	FPresentationSourceHandle(uint32 InWorldId, int32 InIndex, uint32 InGeneration)
		: WorldId(InWorldId), Index(InIndex), Generation(InGeneration)
	{
	}

	uint32 WorldId = 0;
	int32 Index = INDEX_NONE;
	uint32 Generation = 0;

	friend UPresentationWorldSubsystem;
};

/** 提交器固化的纯观察数据；View 描述镜头，Subject 描述本地玩家主体。 */
struct ELEMENTSANDBOXPRESENTATION_API FPresentationViewSource final
{
	/** 注册表写入的稳定身份；提交器传入的值会被当前 World 的真实句柄覆盖。 */
	FPresentationSourceHandle SourceHandle;
	FVector ViewLocation = FVector::ZeroVector;
	FVector SubjectLocation = FVector::ZeroVector;
	FVector Forward = FVector::ForwardVector;
	FVector Right = FVector::RightVector;
	FVector Up = FVector::UpVector;
	FConvexVolume ViewFrustum;
	float HorizontalFOVDegrees = 90.0f;
	float AspectRatio = 16.0f / 9.0f;
	FIntPoint ViewportSize = FIntPoint(1920, 1080);
	int32 Priority = 0;
	uint64 Revision = 1;

	bool IsValid() const;
	float GetVerticalFOVDegrees() const;
};

/** 某个明确消费边界固化的多相机并集输入；本类型不规定客户端调度频率。 */
struct ELEMENTSANDBOXPRESENTATION_API FPresentationViewSnapshot final
{
	TArray<FPresentationViewSource> Sources;
	uint64 Revision = 0;
};
