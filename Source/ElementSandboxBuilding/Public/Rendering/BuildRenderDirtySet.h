#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"

/** Entity 的场景表现需要执行的最小同步范围，枚举顺序即覆盖优先级。 */
enum class EBuildRenderDirtyMode : uint8
{
	PartSet,
	AllParts,
	Rebuild
};

/** 一条按 Entity 合并后的表现脏记录。 */
struct ELEMENTSANDBOXBUILDING_API FBuildRenderDirtyEntry final
{
	FBuildEntityHandle Entity;
	EBuildRenderDirtyMode Mode = EBuildRenderDirtyMode::PartSet;
	TArray<int32, TInlineAllocator<8>> PartIds;
	/** 仅首次进入表现索引或 Mobility 改变时设置；空值保持当前分类。 */
	TOptional<bool> bPackedStatic;
};

/**
 * 世界装配边界持有的一帧表现脏集合。
 *
 * 本类型只记录需要同步的 Entity，不持有 Mesh、Transform 或场景对象。同一 Entity
 * 的重复标记保持首次出现顺序并合并；覆盖关系为 Rebuild > AllParts > PartSet。
 * 所有访问限定在 Game Thread，GetEntries 返回的 View 在下一次写操作后失效。
 */
class ELEMENTSANDBOXBUILDING_API FBuildRenderDirtySet final
{
public:
	/** 无效 Handle 被拒绝；重复标记属于成功的幂等操作。 */
	bool MarkPartsDirty(FBuildEntityHandle Entity, TConstArrayView<int32> PartIds);
	bool MarkAllPartsDirty(FBuildEntityHandle Entity);
	bool MarkRebuild(FBuildEntityHandle Entity);
	bool MarkRebuild(FBuildEntityHandle Entity, bool bPackedStatic);

	/**
	 * 请求消费者先清空全部已有表现。调用前积累的逐 Entity 标记随之作废；调用后
	 * 仍可继续记录本次 Clear-All 之后创建或修改的 Entity。
	 */
	void RequestClearAll();

	bool IsClearAllRequested() const;
	TConstArrayView<FBuildRenderDirtyEntry> GetEntries() const;
	int32 Num() const;
	bool IsEmpty() const;
	SIZE_T GetEstimatedAllocatedSize() const;

	/** 消费成功后清空本批次的 Clear-All 请求与逐 Entity 标记。 */
	void Clear();

private:
	FBuildRenderDirtyEntry* FindOrAddEntry(FBuildEntityHandle Entity);

	bool bClearAllRequested = false;
	TArray<FBuildRenderDirtyEntry> Entries;
	TMap<FBuildEntityHandle, int32> EntryIndexByEntity;
};
