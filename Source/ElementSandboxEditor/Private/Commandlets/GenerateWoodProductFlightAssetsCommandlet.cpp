#include "Commandlets/GenerateWoodProductFlightAssetsCommandlet.h"
#include "Materials/WoodProductFlightShader.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "UObject/SavePackage.h"
#include "WorldObjects/WoodProductFlight.h"
#include "WorldObjects/WoodProductFlightMaterialSet.h"
#include "MeteorStrikeRuleSet.h"

namespace
{
	const FString AssetRoot = TEXT("/Game/WorldObjects/WoodBlock/");
	template<class T> T* Node(UMaterial& Material)
	{
		T* Value = NewObject<T>(&Material, NAME_None, RF_Transactional);
		Material.GetExpressionCollection().AddExpression(Value);
		return Value;
	}
	void Input(UMaterialExpressionCustom& Custom, FName Name, UMaterialExpression* Expression, int32 Output = 0)
	{
		auto& Item = Custom.Inputs.AddDefaulted_GetRef();
		Item.InputName = Name; Item.Input.Expression = Expression; Item.Input.OutputIndex = Output;
	}
	bool Save(UObject& Asset)
	{
		Asset.MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(&Asset);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Asset.GetOutermost(), &Asset,
			*FPackageName::LongPackageNameToFilename(Asset.GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}
	UMaterialExpression* Data4(UMaterial& Material, int32 First)
	{
		UMaterialExpression* Scalars[4];
		for (int32 I = 0; I < 4; ++I)
		{
			auto* Data = Node<UMaterialExpressionPerInstanceCustomData>(Material);
			Data->DataIndex = First + I; Data->ConstDefaultValue = 0;
			Scalars[I] = Data;
		}
		auto* AB = Node<UMaterialExpressionAppendVector>(Material);
		auto* CD = Node<UMaterialExpressionAppendVector>(Material);
		auto* Result = Node<UMaterialExpressionAppendVector>(Material);
		AB->A.Expression = Scalars[0]; AB->B.Expression = Scalars[1];
		CD->A.Expression = Scalars[2]; CD->B.Expression = Scalars[3];
		Result->A.Expression = AB; Result->B.Expression = CD;
		return Result;
	}
	UMaterial* MakeFlightMaterial(UMaterialInterface& Source, const FString& Name)
	{
		UPackage* Package = CreatePackage(*(AssetRoot + Name)); Package->FullyLoad();
		UMaterial* Material = DuplicateObject<UMaterial>(Source.GetMaterial(), Package, *Name);
		if (!Material) return nullptr;
		Material->SetFlags(RF_Public | RF_Standalone);
		// 普通木块没有烧蚀 Custom Data。原表面节点仍读取默认值，不能把飞行位置误当烧蚀量。
		for (UMaterialExpression* Expression : Material->GetExpressions())
			if (auto* Data = Cast<UMaterialExpressionPerInstanceCustomData>(Expression)) Data->DataIndex += FWoodProductFlight::CustomFloatCount;
		auto* Editor = Material->GetEditorOnlyData();
		FExpressionInput SurfaceNormal = Editor->Normal;
		const bool bWasTangent = Material->bTangentSpaceNormal;
		Material->bUsedWithInstancedStaticMeshes = true;
		Material->bTangentSpaceNormal = false;
		Material->MaxWorldPositionOffsetDisplacement = 3200.0f;
		auto* Zero = Node<UMaterialExpressionConstant3Vector>(*Material);
		Zero->Constant = FLinearColor::Black;
		auto* Pivot = Node<UMaterialExpressionTransformPosition>(*Material);
		Pivot->TransformSourceType = TRANSFORMPOSSOURCE_Instance;
		Pivot->TransformType = TRANSFORMPOSSOURCE_World;
		Pivot->Input.Expression = Zero;
		auto* WorldPosition = Node<UMaterialExpressionWorldPosition>(*Material);
		WorldPosition->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
		auto* Relative = Node<UMaterialExpressionSubtract>(*Material);
		Relative->A.Expression = WorldPosition; Relative->B.Expression = Pivot;
		auto* Time = Node<UMaterialExpressionTime>(*Material);
		Time->bIgnorePause = false; Time->bOverride_Period = false;
		auto* Position = Node<UMaterialExpressionCustom>(*Material);
		Position->Inputs.Reset(); Position->OutputType = CMOT_Float3;
		Position->Description = TEXT("Wood flight: fixed rest transform, analytic WPO");
		Position->Code = UE::ElementSandbox::WoodFlight::PositionCode;
		Input(*Position, TEXT("RestOffset"), Relative);
		Input(*Position, TEXT("Time"), Time);
		const FName Names[] = {TEXT("P0"),TEXT("V"),TEXT("A"),TEXT("W"),TEXT("Q0"),TEXT("QR"),TEXT("H")};
		for (int32 I = 0; I < 7; ++I) Input(*Position, Names[I], Data4(*Material, I * 4));
		auto& RotationOutput = Position->AdditionalOutputs.AddDefaulted_GetRef();
		RotationOutput.OutputName = TEXT("DeltaRotation"); RotationOutput.OutputType = CMOT_Float4;
		Position->RebuildOutputs();
		Editor->WorldPositionOffset.Expression = Position;
		Editor->WorldPositionOffset.OutputIndex = 0;
		auto* Rotation = Node<UMaterialExpressionVertexInterpolator>(*Material);
		Rotation->Input.Expression = Position; Rotation->Input.OutputIndex = 1;
		UMaterialExpression* BaseNormal = nullptr;
		if (SurfaceNormal.Expression)
		{
			if (bWasTangent)
			{
				auto* ToWorld = Node<UMaterialExpressionTransform>(*Material);
				ToWorld->TransformSourceType = TRANSFORMSOURCE_Tangent;
				ToWorld->TransformType = TRANSFORM_World;
				ToWorld->Input = SurfaceNormal; BaseNormal = ToWorld;
			}
			else BaseNormal = SurfaceNormal.Expression;
		}
		else
		{
			auto* NormalVS = Node<UMaterialExpressionVertexInterpolator>(*Material);
			NormalVS->Input.Expression = Node<UMaterialExpressionVertexNormalWS>(*Material); BaseNormal = NormalVS;
		}
		auto* Normal = Node<UMaterialExpressionCustom>(*Material);
		Normal->Inputs.Reset(); Normal->OutputType = CMOT_Float3;
		Normal->Code = UE::ElementSandbox::WoodFlight::NormalCode;
		Input(*Normal, TEXT("Rotation"), Rotation); Input(*Normal, TEXT("BaseNormal"), BaseNormal);
		Editor->Normal.Expression = Normal; Editor->Normal.OutputIndex = 0;
		Material->PostEditChange();
		return Save(*Material) ? Material : nullptr;
	}
}
UGenerateWoodProductFlightAssetsCommandlet::UGenerateWoodProductFlightAssetsCommandlet()
{
	IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true; ShowErrorCount = true;
}
int32 UGenerateWoodProductFlightAssetsCommandlet::Main(const FString& Params)
{
	auto* Wood = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Building/Materials/MI_FirePileWood.MI_FirePileWood"));
	auto* Charcoal = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
	auto* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/WorldObjects/WoodBlock/SM_WoodBlock.SM_WoodBlock"));
	if (!Wood || !Charcoal || !Mesh) return 1;
	UMaterial* Bases[] = {MakeFlightMaterial(*Wood, TEXT("M_WoodProductFlight")), MakeFlightMaterial(*Charcoal, TEXT("M_CharcoalFlight"))};
	if (!Bases[0] || !Bases[1]) return 2;
	auto* Package = CreatePackage(*(AssetRoot + TEXT("DA_WoodProductFlightMaterials")));
	Package->FullyLoad();
	auto* Set = NewObject<UWoodProductFlightMaterialSet>(Package, TEXT("DA_WoodProductFlightMaterials"), RF_Public | RF_Standalone);
	Set->Mesh = Mesh; Set->StaticWood = Wood; Set->StaticCharcoal = Charcoal;
	const auto Config = GetDefault<UMeteorStrikeRuleSet>()->Freeze();
	// 使用 RuleSet 的最高起点和最高速度包住弹道；运行时仍逐组选择实际包络的最小档。
	const double Speed = Config.DebrisSpeedRange.Y;
	const double Height = FMath::Max(100000.0, static_cast<double>(Config.MeteorHeight));
	const double Gravity = -Config.GravityZ;
	const double FlightTime = (Speed + FMath::Sqrt(Speed * Speed + 2.0 * Gravity * Height)) / Gravity;
	const double Envelope = FMath::Max(Speed * FlightTime, Height + Speed * Speed / (2.0 * Gravity)) + 10000.0;
	const int32 LastTier = UWoodProductFlightMaterialSet::ComputeTier(Envelope);
	for (int32 Tier = 0; Tier <= LastTier; ++Tier)
	{
		auto& Entry = Set->Tiers.AddDefaulted_GetRef();
		Entry.MaximumDisplacement = UWoodProductFlightMaterialSet::GetTierExtent(Tier);
		for (int32 Kind = 0; Kind < 2; ++Kind)
		{
			const FString Name = FString::Printf(TEXT("MI_%sFlight_%dm"), Kind ? TEXT("Charcoal") : TEXT("WoodProduct"), FMath::RoundToInt(Entry.MaximumDisplacement / 100.0f));
			auto* MIPackage = CreatePackage(*(AssetRoot + Name)); MIPackage->FullyLoad();
			auto* MI = NewObject<UMaterialInstanceConstant>(MIPackage, *Name, RF_Public | RF_Standalone);
			MI->SetParentEditorOnly(Bases[Kind], false);
			MI->CopyMaterialUniformParametersEditorOnly(Kind ? Charcoal : Wood);
			MI->BasePropertyOverrides.bOverride_MaxWorldPositionOffsetDisplacement = true;
			MI->BasePropertyOverrides.MaxWorldPositionOffsetDisplacement = Entry.MaximumDisplacement;
			MI->PostEditChange();
			if (!Save(*MI)) return 3;
			if (Kind) Entry.Charcoal = MI; else Entry.Wood = MI;
		}
	}
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	if (!Save(*Set)) return 4;
	UE_LOG(LogTemp, Display, TEXT("Wood flight assets generated: %d tiers, maximum %.0f cm, mesh=%s."), Set->Tiers.Num(), Set->Tiers.Last().MaximumDisplacement, *Mesh->GetPathName());
	return 0;
}
