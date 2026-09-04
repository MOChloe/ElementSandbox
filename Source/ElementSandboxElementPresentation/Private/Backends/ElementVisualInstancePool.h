#pragma once

#include "CoreMinimal.h"
#include "ElementPresentationTypes.h"
#include "ElementVisualDefinition.h"
#include "Visual/ElementVisualTypes.h"

class AActor;
class UInstancedStaticMeshComponent;
class UWorld;

/** ElementPresentation 独占的 HISM/ISM 页池；不注册通用 MeshPool Layer。 */
class FElementVisualInstancePool final
{
public:
	FElementVisualInstancePool(UWorld& InWorld, const FElementPresentationConfig& InConfig);
	~FElementVisualInstancePool();

	FElementVisualInstancePool(const FElementVisualInstancePool&) = delete;
	FElementVisualInstancePool& operator=(const FElementVisualInstancePool&) = delete;

	bool Upsert(
		const FElementVisualDescriptor& Descriptor,
		const FElementVisualDefinition& Definition);
	bool Remove(const FElementVisualKey& Key);
	bool Contains(const FElementVisualKey& Key) const;
	void Reset();

	void AppendStats(FElementPresentationStats& InOutStats) const;

private:
	struct FPage;
	struct FLocation final
	{
		int32 PageIndex = INDEX_NONE;
		int32 InstanceIndex = INDEX_NONE;
	};

	bool EnsureHost();
	int32 AcquirePage(const FElementVisualDefinition& Definition);
	int32 AllocatePage(const FElementVisualDefinition& Definition);
	bool ConfigurePage(FPage& Page, const FElementVisualDefinition& Definition);
	bool AddToPage(
		int32 PageIndex,
		const FElementVisualDescriptor& Descriptor,
		const FElementVisualDefinition& Definition);
	bool UpdateInPage(
		const FLocation& Location,
		const FElementVisualDescriptor& Descriptor,
		const FElementVisualDefinition& Definition);
	void RetireEmptyPage(int32 PageIndex);
	void DestroyPage(int32 PageIndex);
	int32 CountSparePages(EElementVisualInstanceBackend Backend) const;
	static bool ApplyCustomData(
		UInstancedStaticMeshComponent& Component,
		int32 InstanceIndex,
		const FElementVisualDescriptor& Descriptor,
		int32 CustomDataFloatCount);

	UWorld* World = nullptr;
	FElementPresentationConfig Config;
	TWeakObjectPtr<AActor> Host;
	TArray<TUniquePtr<FPage>> Pages;
	TMap<FElementVisualKey, FLocation> Locations;
	uint64 PageAllocateCount = 0;
	uint64 PageReuseCount = 0;
};
