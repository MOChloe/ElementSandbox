#pragma once

#include "CoreMinimal.h"
#include "Runtime/ElementAuthorityExecution.h"
#include "Subsystems/WorldSubsystem.h"

#include "ElementSimulationSubsystem.generated.h"

class FElementVisualJournal;

/**
 * Host-neutral Element infrastructure owned once per game world.
 * Gameplay rules、宿主 Adapter 与 Barrier 驱动属于 ElementGameplay；本 Subsystem 只拥有
 * 一份通用集中执行核和客户端可见的 Visual Journal。
 */
UCLASS()
class ELEMENTSANDBOXSIMULATION_API UElementSimulationSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UElementSimulationSubsystem();
	virtual ~UElementSimulationSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Authority 世界只能激活一份执行核；Client 永不创建查询或 Processor。 */
	bool ActivateAuthorityExecution(const FElementAuthorityExecutionConfig& Config = {});
	void DeactivateAuthorityExecution();
	FElementAuthorityExecution* GetAuthorityExecution() { return AuthorityExecution.Get(); }
	const FElementAuthorityExecution* GetAuthorityExecution() const { return AuthorityExecution.Get(); }

	/** Null only on Dedicated Server, where no presentation objects are allocated. */
	TSharedPtr<FElementVisualJournal, ESPMode::ThreadSafe> GetVisualJournal() const
	{
		return VisualJournal;
	}

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	TUniquePtr<FElementAuthorityExecution> AuthorityExecution;
	TSharedPtr<FElementVisualJournal, ESPMode::ThreadSafe> VisualJournal;
};
