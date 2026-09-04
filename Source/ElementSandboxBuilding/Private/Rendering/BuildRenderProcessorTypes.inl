// Build Render 的不可变请求键、Source 状态与延迟释放批次。
// 只描述数据，不执行 MeshPool 或 ECS 写入。

namespace
{
using FRenderedPart = FBuildPresentationAppliedPart;

struct FRenderedEntity final
{
	TArray<FRenderedPart, TInlineAllocator<8>> Parts;
	int32 MeshPartCost = 0;
	int32 NextPartId = 0;
	double LastRequiredTimeSeconds = 0.0;
	bool bComplete = false;
};

struct FRetiringRenderedEntity final
{
	TArray<FMeshPoolInstanceHandle, TInlineAllocator<8>> Instances;
};

struct FLocalResidencyCache final
{
	/** 该 Source 的 Local 内容版本；0 表示当前缓存无效。 */
	uint64 SourceRevision = 0;
	uint64 IndexChangeSerial = 0;
	FVector SubjectLocation = FVector::ZeroVector;
	TSet<FBuildEntityHandle> RequiredEntities;
	/** Far 请求只共享该不可变副本；只在 Local 真正换代时重建。删除项由 Index tombstone 屏蔽。 */
	TSharedPtr<const TSet<FBuildEntityHandle>, ESPMode::ThreadSafe> RequiredEntitiesSnapshot;
	TArray<FBuildEntityHandle> OrderedEntities;
	TSet<FBuildEntityHandle> HotPinnedEntities;
	/** 该缓存当前拥有的 Hot 引用；保持数组形态，Source 移除时可 O(1) 转交分帧释放。 */
	TArray<FBuildEntityHandle> OwnedHotPinnedEntities;
	/** TargetLocal 独占的新引用；与旧 Local 重合的 Entity 不重复加引用。 */
	TArray<FBuildEntityHandle> AcquiredEntities;
	/** TargetLocal 相对旧 Local 新取得的 Hot 引用；取消 Target 时必须分帧归还。 */
	TArray<FBuildEntityHandle> AcquiredHotPinnedEntities;
	/** 已取得引用、但仍在等待 MeshPool Add 的 TargetLocal Entity。 */
	TSet<FBuildEntityHandle> PendingRenderEntities;
	int32 PreparationReadCursor = 0;
	int32 PreparationWriteCursor = 0;
	int32 PreviousLocalReleaseCursor = 0;
	bool bPreparationComplete = false;
	int32 MeshPartCost = 0;
	double Boundary = 0.0;

	void Reset()
	{
		SourceRevision = 0;
		IndexChangeSerial = 0;
		SubjectLocation = FVector::ZeroVector;
		RequiredEntities.Reset();
		RequiredEntitiesSnapshot.Reset();
		OrderedEntities.Reset();
		HotPinnedEntities.Reset();
		OwnedHotPinnedEntities.Reset();
		AcquiredEntities.Reset();
		AcquiredHotPinnedEntities.Reset();
		PendingRenderEntities.Reset();
		PreparationReadCursor = 0;
		PreparationWriteCursor = 0;
		PreviousLocalReleaseCursor = 0;
		bPreparationComplete = false;
		MeshPartCost = 0;
		Boundary = 0.0;
	}

	void RefreshRequiredEntitiesSnapshot()
	{
		RequiredEntitiesSnapshot = MakeShared<TSet<FBuildEntityHandle>, ESPMode::ThreadSafe>(RequiredEntities);
	}

	SIZE_T GetEstimatedAllocatedSize() const
	{
		return RequiredEntities.GetAllocatedSize() + OrderedEntities.GetAllocatedSize() +
			   HotPinnedEntities.GetAllocatedSize() + OwnedHotPinnedEntities.GetAllocatedSize() +
			   AcquiredEntities.GetAllocatedSize() + AcquiredHotPinnedEntities.GetAllocatedSize() +
			   PendingRenderEntities.GetAllocatedSize();
	}
};

struct FPresentationSourceKey final
{
	FPresentationSourceHandle Handle;
	int32 FallbackIndex = INDEX_NONE;

	friend bool operator==(const FPresentationSourceKey& Left, const FPresentationSourceKey& Right)
	{
		if (Left.Handle.IsSet() || Right.Handle.IsSet())
		{
			return Left.Handle == Right.Handle;
		}
		return Left.FallbackIndex == Right.FallbackIndex;
	}
	friend uint32 GetTypeHash(const FPresentationSourceKey& Key)
	{
		return Key.Handle.IsSet() ? GetTypeHash(Key.Handle)
								  : HashCombineFast(0x96a7d3e1u, GetTypeHash(Key.FallbackIndex));
	}
};

struct FSourceResidencyState final
{
	uint64 Token = 0;
	FPresentationViewSource LatestView;
	FLocalResidencyCache Local;
	FLocalResidencyCache TargetLocal;
	/** 只在该 Source 的 Local Cell 订阅命中时推进。 */
	uint64 LocalContentRevision = 1;
	uint64 LatestLocalRequestId = 0;
	uint64 InFlightLocalRequestId = 0;
	bool bLocalSelectionInFlight = false;
	bool bNeedsLocalSelection = true;
	bool bRefreshLocalAfterProvisionalResult = false;
	/** Tombstone 命中过 Local/Target 快照后，下一份选择结果必须整体换代，不能走纯追加快路。 */
	bool bRequiresLocalSnapshotReplacement = false;
	TSet<FBuildEntityHandle> ActiveFarSet;
	/** 上次晋升结果的 Worker 侧纯值快照；可能保留已删除项，但绝不会漏掉当前 Active。 */
	TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe> ActiveFarEntriesSnapshot;
	int32 ActiveFarMeshPartCost = 0;
	int32 ActiveFarRequestedMeshPartCost = 0;
	double ActiveFarBoundaryScore = 0.0;
	FVector2D ActiveDirection = FVector2D(1.0, 0.0);
	FVector ActiveViewLocation = FVector::ZeroVector;
	FVector ActiveSubjectLocation = FVector::ZeroVector;
	uint64 ActiveFarSourceRevision = 0;
	bool bHasActiveDirection = false;

	TSet<FBuildEntityHandle> TransitionFarSet;
	TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe> TransitionFarEntriesSnapshot;
	int32 TransitionFarMeshPartCost = 0;
	int32 OverlappingFarMeshPartCost = 0;
	int32 TransitionFarTargetMeshPartCost = 0;
	int32 TransitionFarRequestedMeshPartCost = 0;
	double TransitionFarBoundaryScore = 0.0;
	FVector2D TransitionDirection = FVector2D(1.0, 0.0);
	FVector TransitionViewLocation = FVector::ZeroVector;
	FVector TransitionSubjectLocation = FVector::ZeroVector;
	uint64 TransitionFarSourceRevision = 0;
	TArray<FBuildPresentationSelectorEntry> TargetEntries;
	TSet<FBuildEntityHandle> TargetSet;
	TArray<FBuildEntityHandle> ReclaimOrder;
	/** Worker 发布的旧 Transition 差集；按 Residency mutation deadline 跨帧回收。 */
	TArray<FBuildEntityHandle> SupersededTransitionEntities;
	int32 SupersededTransitionCursor = 0;
	int32 TargetCursor = 0;
	int32 VisibleCoreEntryCount = 0;
	int32 VisibleCoreRemainingMeshPartCost = 0;
	int32 ReclaimCursor = 0;
	bool bTargetRevisionCurrent = false;
	bool bRefreshAfterProvisionalResult = false;

	EBuildPresentationTransitionPhase Phase = EBuildPresentationTransitionPhase::Stable;
	uint64 LatestRequestId = 0;
	uint64 InFlightRequestId = 0;
	FVector2D InFlightDirection = FVector2D(1.0, 0.0);
	FVector InFlightViewLocation = FVector::ZeroVector;
	FVector InFlightSubjectLocation = FVector::ZeroVector;
	/** 由命中该 Source Far 订阅或 Local 排除集合变化推进。 */
	uint64 FarContentRevision = 1;
	double LastFarContentChangeTimeSeconds = -1.0;
	bool bSelectionInFlight = false;
	double LastObservationTimeSeconds = -1.0;
	double SettledSinceSeconds = -1.0;
	double TargetAcceptedTimeSeconds = -1.0;
	double PromotionLockedUntilSeconds = -1.0;
	double LastTurnTimeSeconds = -1.0;
	int32 LastTurnSign = 0;
	TArray<double, TInlineAllocator<4>> ReversalTimes;
	bool bRapidRotation = false;
	bool bNeedsFarSelection = true;

	SIZE_T GetEstimatedAllocatedSize() const
	{
		return Local.GetEstimatedAllocatedSize() + TargetLocal.GetEstimatedAllocatedSize() +
			   ActiveFarSet.GetAllocatedSize() + TransitionFarSet.GetAllocatedSize() +
			   TargetEntries.GetAllocatedSize() + TargetSet.GetAllocatedSize() + ReclaimOrder.GetAllocatedSize() +
			   SupersededTransitionEntities.GetAllocatedSize() +
			   ReversalTimes.GetAllocatedSize();
	}
};

class FBuildPresentationSelectionAsyncState final
	: public TSharedFromThis<FBuildPresentationSelectionAsyncState, ESPMode::ThreadSafe>
{
public:
	TAtomic<bool> bActive{true};
	TAtomic<int32> InFlightCount{0};
	TAtomic<int32> LocalInFlightCount{0};
	TQueue<TUniquePtr<FBuildLocalSelectionResult>, EQueueMode::Mpsc> CompletedLocalResults;
	TQueue<TUniquePtr<FBuildFarSelectionResult>, EQueueMode::Mpsc> CompletedFarResults;
};

struct FDeferredResidencyReleaseBatch final
{
	TArray<FBuildEntityHandle> Entities;
	bool bQueueDirectionalCleanup = false;
};

struct FDeferredHotPinReleaseBatch final
{
	TArray<FBuildEntityHandle> Entities;
};

bool TryResolveRenderEntity(const FBuildEntityRegistry& Registry, const FBuildEntityHandle Entity,
							const FBuildTransformFragment*& OutTransform, const UBuildingDefinition*& OutDefinition,
							const FBuildPartTransformFragment*& OutPartTransforms)
{
	OutTransform = Registry.FindFragment<FBuildTransformFragment>(Entity);
	const FBuildDefinitionFragment* DefinitionFragment = Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	OutDefinition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	OutPartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Entity);
	return OutTransform && OutDefinition &&
		   (!OutPartTransforms || OutPartTransforms->LocalTransforms.Num() == OutDefinition->MeshParts.Num());
}

} // namespace
