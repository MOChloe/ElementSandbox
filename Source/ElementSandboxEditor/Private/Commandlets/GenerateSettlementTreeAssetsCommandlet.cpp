#include "Commandlets/GenerateSettlementTreeAssetsCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Tree/SettlementTreeTypes.h"
#include "WorldObjects/WoodBlockMeshFactory.h"

DEFINE_LOG_CATEGORY_STATIC(LogGenerateSettlementTreeAssets, Log, All);

namespace
{
	constexpr const TCHAR* TreeMaterialPackageName = TEXT("/Game/WorldObjects/Trees/M_SettlementTree");
	constexpr const TCHAR* TreeMaterialObjectName = TEXT("M_SettlementTree");
	constexpr const TCHAR* WoodBlockPackageName = TEXT("/Game/WorldObjects/WoodBlock/SM_WoodBlock");
	constexpr const TCHAR* WoodBlockObjectName = TEXT("SM_WoodBlock");

	template <typename TExpression>
	TExpression* AddExpression(UMaterial& Material, const int32 X, const int32 Y)
	{
		TExpression* Expression = NewObject<TExpression>(&Material, NAME_None, RF_Transactional);
		Expression->MaterialExpressionEditorX = X;
		Expression->MaterialExpressionEditorY = Y;
		Material.GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}
}

UGenerateSettlementTreeAssetsCommandlet::UGenerateSettlementTreeAssetsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UGenerateSettlementTreeAssetsCommandlet::Main(const FString& Params)
{
	(void)Params;
	UPackage* Package = CreatePackage(TreeMaterialPackageName);
	if (!Package)
	{
		UE_LOG(LogGenerateSettlementTreeAssets, Error, TEXT("无法创建树材质 Package。"));
		return 1;
	}
	Package->FullyLoad();
	UMaterial* Material = FindObject<UMaterial>(Package, TreeMaterialObjectName);
	const bool bCreatedMaterial = Material == nullptr;
	if (!Material)
	{
		Material = NewObject<UMaterial>(
			Package,
			TreeMaterialObjectName,
			RF_Public | RF_Standalone | RF_Transactional);
	}
	if (!Material || !Material->GetEditorOnlyData())
	{
		UE_LOG(LogGenerateSettlementTreeAssets, Error, TEXT("无法创建树材质对象。"));
		return 1;
	}
	Material->Modify();
	Material->GetExpressionCollection().Empty();
	Material->BlendMode = BLEND_Opaque;
	Material->MaterialDomain = MD_Surface;
	Material->SetShadingModel(MSM_DefaultLit);
	Material->TwoSided = true;
	Material->bUsedWithInstancedStaticMeshes = true;

	UMaterialExpressionVertexColor* VertexColor =
		AddExpression<UMaterialExpressionVertexColor>(*Material, -600, -80);
	UMaterialExpressionPerInstanceCustomData* InstanceVariation =
		AddExpression<UMaterialExpressionPerInstanceCustomData>(*Material, -600, 100);
		InstanceVariation->DataIndex = SettlementTreeColorVariationCustomDataIndex;
	InstanceVariation->ConstDefaultValue = 0.5f;
	UMaterialExpressionMultiply* VariationRange =
		AddExpression<UMaterialExpressionMultiply>(*Material, -400, 100);
	VariationRange->A.Expression = InstanceVariation;
	VariationRange->ConstB = 0.36f;
	UMaterialExpressionAdd* VariationBias =
		AddExpression<UMaterialExpressionAdd>(*Material, -200, 100);
	VariationBias->A.Expression = VariationRange;
	VariationBias->ConstB = 0.82f;
	UMaterialExpressionMultiply* TintedVertexColor =
		AddExpression<UMaterialExpressionMultiply>(*Material, 0, -40);
		TintedVertexColor->A.Expression = VertexColor;
		TintedVertexColor->B.Expression = VariationBias;
		UMaterialExpressionPerInstanceCustomData* BurnAmount =
			AddExpression<UMaterialExpressionPerInstanceCustomData>(*Material, -200, 280);
		BurnAmount->DataIndex = SettlementTreeBurnAmountCustomDataIndex;
		BurnAmount->ConstDefaultValue = 0.0f;
		UMaterialExpressionOneMinus* RemainingColor =
			AddExpression<UMaterialExpressionOneMinus>(*Material, 0, 280);
		RemainingColor->Input.Expression = BurnAmount;
		UMaterialExpressionMultiply* BurnedVertexColor =
			AddExpression<UMaterialExpressionMultiply>(*Material, 220, -40);
		BurnedVertexColor->A.Expression = TintedVertexColor;
		BurnedVertexColor->B.Expression = RemainingColor;
		Material->GetEditorOnlyData()->BaseColor.Expression = BurnedVertexColor;

	UMaterialExpressionConstant* Roughness =
		AddExpression<UMaterialExpressionConstant>(*Material, 0, 180);
	Roughness->R = 0.86f;
	Material->GetEditorOnlyData()->Roughness.Expression = Roughness;
	Material->PostEditChange();
	Material->MarkPackageDirty();
	if (bCreatedMaterial)
	{
		FAssetRegistryModule::AssetCreated(Material);
	}

	const FString Filename = FPackageName::LongPackageNameToFilename(
		TreeMaterialPackageName,
		FPackageName::GetAssetPackageExtension());
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
	{
		UE_LOG(LogGenerateSettlementTreeAssets, Error,
			TEXT("无法创建树材质目录：%s"), *FPaths::GetPath(Filename));
		return 1;
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GError;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Material, *Filename, SaveArgs))
	{
		UE_LOG(LogGenerateSettlementTreeAssets, Error,
			TEXT("保存树材质失败：%s"), *Filename);
		return 1;
	}
	UE_LOG(LogGenerateSettlementTreeAssets, Display,
			TEXT("树材质生成完成：%s；Opaque/TwoSided/DefaultLit/VertexColor/PerInstanceCustomData[0..1]。"),
		*Filename);

	UPackage* WoodPackage = CreatePackage(WoodBlockPackageName);
	if (!WoodPackage)
	{
		UE_LOG(LogGenerateSettlementTreeAssets, Error, TEXT("无法创建木块 Mesh Package。"));
		return 1;
	}
	WoodPackage->FullyLoad();
	UStaticMesh* WoodBlock = FindObject<UStaticMesh>(WoodPackage, WoodBlockObjectName);
	if (WoodBlock)
	{
		WoodBlock->ClearFlags(RF_Public | RF_Standalone);
		WoodBlock->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
	}
	WoodBlock = FWoodBlockMeshFactory::Create(
		*WoodPackage,
		WoodBlockObjectName,
		RF_Public | RF_Standalone | RF_Transactional);
	if (!WoodBlock)
	{
		UE_LOG(LogGenerateSettlementTreeAssets, Error, TEXT("无法生成固定木块 Mesh。"));
		return 1;
	}
	WoodBlock->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(WoodBlock);
	const FString WoodFilename = FPackageName::LongPackageNameToFilename(
		WoodBlockPackageName,
		FPackageName::GetAssetPackageExtension());
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(WoodFilename), true)
		|| !UPackage::SavePackage(WoodPackage, WoodBlock, *WoodFilename, SaveArgs))
	{
		UE_LOG(LogGenerateSettlementTreeAssets, Error,
			TEXT("保存木块 Mesh 失败：%s"), *WoodFilename);
		return 1;
	}
	UE_LOG(LogGenerateSettlementTreeAssets, Display,
		TEXT("固定木块 Mesh 生成完成：%s；140x48x40cm、Simple Box Collision。"),
		*WoodFilename);
	return 0;
}
