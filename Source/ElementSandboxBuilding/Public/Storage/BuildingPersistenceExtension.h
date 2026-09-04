#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"

class FBuildEntityRegistry;
class UWorldStorageSubsystem;

/**
 * Building Core 不依赖具体 Catalog Fragment。Catalog 以稳定的具名 Section 扩展
 * Building Chunk Payload；Section 内容必须是显式值编码，禁止序列化 Fragment Pool。
 */
class ELEMENTSANDBOXBUILDING_API IBuildingPersistenceExtension
{
public:
	virtual ~IBuildingPersistenceExtension() = default;

	virtual FName GetSectionId() const = 0;
	virtual uint16 GetSectionVersion() const = 0;
	virtual bool Capture(
		const FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity,
		TArray<uint8>& OutPayload,
		FString& OutError) const = 0;
	/**
	 * 批量移除的只读预检。默认复用 Capture 保持扩展校验语义；高频扩展可覆盖此函数，
	 * 在不编码 Payload 的前提下验证同一组不变量。实现不得修改 Registry 或外部状态。
	 */
	virtual bool ValidateRemovalState(
		const FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity,
		FString& OutError) const
	{
		TArray<uint8> IgnoredPayload;
		return Capture(Registry, Entity, IgnoredPayload, OutError);
	}
	virtual bool Restore(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity,
		uint16 SectionVersion,
		TConstArrayView<uint8> Payload,
		FString& OutError) const = 0;
	virtual bool RegisterFragmentPersistence(UWorldStorageSubsystem& WorldStorage) const = 0;
};
