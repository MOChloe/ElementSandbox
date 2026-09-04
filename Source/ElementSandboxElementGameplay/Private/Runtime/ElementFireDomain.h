#pragma once

#include "ElementGameplayWorldSubsystem.h"
#include "Templates/UniquePtr.h"

class FElementFireDomainData;
struct FFireRuleSnapshot;
struct FElementFirePersistentRecord;
struct FWorldChunkCoord;
enum class EElementPersistentRemovalSemantic : uint8;

/**
 * Fire 的项目装配边界。查询、队列、数值状态和 Barrier 均由
 * Simulation 的 FElementAuthorityExecution 拥有；本类型只接宿主快照、应用命令并投影结果。
 */
class FElementFireDomain final
{
public:
	explicit FElementFireDomain(UElementGameplayWorldSubsystem& InOwner);
	~FElementFireDomain();

	bool Initialize();
	void Shutdown();

	FElementRuntimeFireSourceHandle CreateFireballSource(const FVector& WorldLocation);
	bool RemoveRuntimeFireSource(FElementRuntimeFireSourceHandle Handle);
	bool SetStickFireInteractionState(FWorldEntityId WorldEntityId, bool bActive);
#if WITH_DEV_AUTOMATION_TESTS
	bool IsStickFireInteractionActiveForTesting(FWorldEntityId WorldEntityId) const;
	int32 GetBuildingHostCountForTesting() const;
#endif

	const FFireRuleSnapshot& GetRules() const;

	bool CapturePersistentState(
		TConstArrayView<FWorldEntityId> EntityIds,
		TArray<FElementFirePersistentRecord>& OutRecords,
		FString& OutError) const;
	bool RestorePersistentState(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FElementFirePersistentRecord> Records,
		FString& OutError);
	bool RemovePersistentState(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldEntityId> EntityIds,
		EElementPersistentRemovalSemantic Semantic,
		FString& OutError);
	bool CanRuntimeEvictPersistentState(FWorldEntityId EntityId) const;

private:
	TUniquePtr<FElementFireDomainData> Data;
};
