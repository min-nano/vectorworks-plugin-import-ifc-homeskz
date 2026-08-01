//
//	parse/IfcAttr.h
//
//	IFC エンティティの属性インデックス（宣言順の 0 始まり番号）と、そこから値を
//	取り出す極小のヘルパー。自前 STEP リーダ（parse/Step）はスキーマを持たず属性を
//	番号でしか引けないため、この番号が実質のスキーマになる。
//
//	**番号は 1 か所にだけ書く。** 以前は同じ番号（IfcRoot.Name=2 / IfcProduct
//	.ObjectPlacement=5 …）が parse/Floor・parse/Rafter・parse/Roof・parse/Story・
//	parse/IfcGeometry の無名名前空間に個別の定数として散っており、片方だけ直せば静かに
//	ズレる形になっていた。参照する要素型が増えるたびに、ここへ 1 行足す。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//	ここは parse/Step の Value / Entity しか知らないヘッダオンリーの定義。
//

#pragma once

#include "parse/Step.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::parse
{
	// IFC の属性インデックス。名前は「エンティティ名 + 属性名」で、IFC スキーマの
	// 宣言順（0 始まり）に対応する。継承した属性も宣言順に並ぶので、サブタイプでも
	// 上位型の番号がそのまま使える（例: IfcSlab は IfcRoot / IfcProduct の派生なので
	// Name=2・ObjectPlacement=5）。
	namespace attr
	{
		// IfcRoot(GlobalId, OwnerHistory, Name=2, Description)。全 IFC 要素で共通。
		inline constexpr std::size_t kRootName = 2;

		// IfcProduct(… ObjectType=4, ObjectPlacement=5, Representation=6)。
		// IfcProduct の全サブタイプ（IfcSlab / IfcBeam / IfcColumn / IfcBuildingStorey …）で共通。
		inline constexpr std::size_t kProductObjectPlacement = 5;
		inline constexpr std::size_t kProductRepresentation = 6;

		// IfcProductDefinitionShape(Name, Description, Representations=2)。
		inline constexpr std::size_t kProductDefinitionShapeRepresentations = 2;

		// IfcShapeRepresentation(ContextOfItems, RepresentationIdentifier,
		// RepresentationType, Items=3)。
		inline constexpr std::size_t kShapeRepresentationItems = 3;

		// IfcLocalPlacement(PlacementRelTo, RelativePlacement=1)。
		inline constexpr std::size_t kLocalPlacementRelativePlacement = 1;

		// IfcAxis2Placement3D / …2D(Location=0, Axis=1, RefDirection=2)。
		inline constexpr std::size_t kAxis2PlacementLocation = 0;
		inline constexpr std::size_t kAxis2PlacementAxis = 1;
		inline constexpr std::size_t kAxis2PlacementRefDirection = 2;

		// IfcCartesianPoint(Coordinates=0) / IfcDirection(DirectionRatios=0) /
		// IfcPolyline(Points=0)。いずれも唯一の属性が先頭に来る。
		inline constexpr std::size_t kCartesianPointCoordinates = 0;
		inline constexpr std::size_t kDirectionRatios = 0;
		inline constexpr std::size_t kPolylinePoints = 0;

		// IfcBuildingStorey(… CompositionType=8, Elevation=9)。
		inline constexpr std::size_t kBuildingStoreyElevation = 9;

		// IfcRelContainedInSpatialStructure(… RelatedElements=4, RelatingStructure=5)。
		inline constexpr std::size_t kRelContainedRelatedElements = 4;
		inline constexpr std::size_t kRelContainedRelatingStructure = 5;

		// IfcGridAxis(AxisTag=0, AxisCurve=1, SameSense=2)。
		inline constexpr std::size_t kGridAxisTag = 0;
		inline constexpr std::size_t kGridAxisCurve = 1;

		// IfcParameterizedProfileDef(ProfileType, ProfileName, Position=2) と、その派生の寸法。
		// IfcRectangleProfileDef(…, Position=2, XDim=3, YDim=4)。
		inline constexpr std::size_t kProfilePosition = 2;
		inline constexpr std::size_t kRectangleProfileXDim = 3;
		inline constexpr std::size_t kRectangleProfileYDim = 4;

		// IfcArbitraryClosedProfileDef(ProfileType, ProfileName, OuterCurve=2)。番号は
		// 上の Position と同じだが**指す属性が違う**ので、別名を与えて呼び出し側の意図を
		// 取り違えないようにする（kProfilePosition で外形を引くと "Position" を読んでいる
		// ように見えてしまう）。派生の IfcArbitraryProfileDefWithVoids も同じ位置。
		inline constexpr std::size_t kArbitraryProfileOuterCurve = 2;

		// IfcExtrudedAreaSolid(SweptArea=0, Position=1, ExtrudedDirection=2, Depth=3)。
		inline constexpr std::size_t kExtrudedAreaSolidSweptArea = 0;
		inline constexpr std::size_t kExtrudedAreaSolidPosition = 1;
		inline constexpr std::size_t kExtrudedAreaSolidDirection = 2;
		inline constexpr std::size_t kExtrudedAreaSolidDepth = 3;

		// IfcBooleanResult(Operator, FirstOperand=1, SecondOperand)。
		inline constexpr std::size_t kBooleanResultFirstOperand = 1;
	} // namespace attr

	// エンティティの指定属性を文字列で返す（未設定・非文字列なら空文字）。Value::text は
	// パーサが decodeStepString を通しているので常に UTF-8（parse/Step.h 参照）。
	inline std::string entityString(const Entity& entity, std::size_t index)
	{
		const Value& value = entity.attribute(index);
		return (value.type == ValueType::String) ? value.text : std::string();
	}

	// IfcRoot.Name を文字列で返す（未設定なら空文字）。要素種別の判定はこの名前に
	// ホームズ君固有の命名規約（"床版" / "屋根版:…" / "木梁:{種別}:{連番}"）が乗る。
	inline std::string entityName(const Entity& entity)
	{
		return entityString(entity, attr::kRootName);
	}
} // namespace HomeskzIfcImport::parse
