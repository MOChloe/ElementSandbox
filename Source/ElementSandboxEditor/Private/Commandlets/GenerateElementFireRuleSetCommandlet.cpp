#include "Commandlets/GenerateElementFireRuleSetCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Fire/ElementFireRuleSet.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogGenerateElementFireRuleSet, Log, All);

namespace
{
	constexpr const TCHAR* FireRulePackageName = TEXT("/Game/Elements/DA_ElementFireRuleSet");
	constexpr const TCHAR* FireRuleObjectName = TEXT("DA_ElementFireRuleSet");
}

UGenerateElementFireRuleSetCommandlet::UGenerateElementFireRuleSetCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UGenerateElementFireRuleSetCommandlet::Main(const FString& Params)
{
	(void)Params;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		FireRulePackageName, FPackageName::GetAssetPackageExtension());
	if (IFileManager::Get().FileExists(*Filename))
	{
		UE_LOG(LogGenerateElementFireRuleSet, Error,
			TEXT("Fire RuleSet 已存在，拒绝覆盖策划资产：%s"), *Filename);
		return 1;
	}

	UPackage* Package = CreatePackage(FireRulePackageName);
	UElementFireRuleSet* RuleSet = Package
		? NewObject<UElementFireRuleSet>(Package, FireRuleObjectName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!RuleSet)
	{
		UE_LOG(LogGenerateElementFireRuleSet, Error, TEXT("无法创建 Fire RuleSet。"));
		return 1;
	}
	FFireRuleSnapshot Snapshot;
	FString Error;
	if (!RuleSet->Freeze(Snapshot, Error))
	{
		UE_LOG(LogGenerateElementFireRuleSet, Error,
			TEXT("默认 Fire RuleSet 未通过严格校验：%s"), *Error);
		return 1;
	}

	RuleSet->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(RuleSet);
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
	{
		UE_LOG(LogGenerateElementFireRuleSet, Error,
			TEXT("无法创建 Fire RuleSet 目录：%s"), *FPaths::GetPath(Filename));
		return 1;
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GError;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, RuleSet, *Filename, SaveArgs))
	{
		UE_LOG(LogGenerateElementFireRuleSet, Error,
			TEXT("保存 Fire RuleSet 失败：%s"), *Filename);
		return 1;
	}
	UE_LOG(LogGenerateElementFireRuleSet, Display,
		TEXT("新版 Fire Gameplay 唯一规则资产已生成：%s"), *Filename);
	return 0;
}
