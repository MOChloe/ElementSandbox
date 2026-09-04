#pragma once

#include "CoreMinimal.h"
#include "Storage/WorldStorageDomainAdapter.h"

class FWorldObjectEntityRegistry;
class UWorldStorageSubsystem;
struct FWorldObjectEntityHandle;

/**
 * 上层模块为 WorldObject 增加 Persistent Fragment 时，通过命名 Section 接入存档。
 * Section 只允许稳定值；UObject 强引用、Actor、空间树 Handle 和任务状态不得写入。
 */
class ELEMENTSANDBOXWORLDOBJECTS_API IWorldObjectPersistenceExtension
{
public:
	virtual ~IWorldObjectPersistenceExtension() = default;

	virtual FName GetSectionId() const = 0;
	virtual uint16 GetSectionVersion() const = 0;
	virtual bool RegisterFragmentPersistence(UWorldStorageSubsystem& WorldStorage) const = 0;
	virtual bool Capture(
		const FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity,
		TArray<uint8>& OutPayload,
		FString& OutError) const = 0;
	virtual bool Restore(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity,
		uint16 SectionVersion,
		TConstArrayView<uint8> Payload,
		FString& OutError) const = 0;
};
