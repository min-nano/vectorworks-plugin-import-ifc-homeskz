//
//	ParseMemberTests.cpp
//
//	横架材解析（src/parse/Member）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:core/ parse/ は無 SDK
//	で単体テスト）。**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M7）: 配置と断面の抽出・構造材 ID・材種名・**天端中央線への
//	基準点補正**（水平材と軸直交切りの傾斜材）・梁ごとのローカル Z からの高さ・レイヤ基準
//	高さへのフォールバック・ストーリレベルへのバインド（横架材天端／軒高／母屋／登り梁）・
//	母屋と登り梁の専用レイヤ分離・クラス割り当て・鉛直軸の材のスキップ・**登り梁の任意断面
//	抽出と直切りの幾何**・食い込み調整（T 字／L 字・勝ち負け・対称・傾斜材の除外）・
//	センタリング・決定性。実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "StepText.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Loader.h"
#include "parse/Member.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::StoryBoundCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildMemberCommands;
using HomeskzIfcImport::parse::CLASS_DODAI;
using HomeskzIfcImport::parse::CLASS_DOUSASHI;
using HomeskzIfcImport::parse::CLASS_MOYA;
using HomeskzIfcImport::parse::CLASS_MUNAGI;
using HomeskzIfcImport::parse::CLASS_NOBORIBARI;
using HomeskzIfcImport::parse::Entity;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::makeMemberId;
using HomeskzIfcImport::parse::memberMaterialName;
using HomeskzIfcImport::parse::MemberPlacement;
using HomeskzIfcImport::parse::memberPlacement3D;
using HomeskzIfcImport::parse::MemberProfile;
using HomeskzIfcImport::parse::memberProfileDims;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::resolveMemberInterferences;
using HomeskzIfcImport::parse::slopedMemberGeometry;
using HomeskzIfcImport::parse::SlopedMemberGeometry;
using HomeskzIfcTests::contain;
using HomeskzIfcTests::direction;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::forEachFixture;
using HomeskzIfcTests::makeStorey;
using HomeskzIfcTests::near;
using HomeskzIfcTests::num;
using HomeskzIfcTests::point2;
using HomeskzIfcTests::point3;
using HomeskzIfcTests::ref;
using HomeskzIfcTests::StepText;

namespace
{
	// -----------------------------------------------------------------------
	// 合成 IFC（STEP テキスト）の組み立て（ストーリ・梁・傾斜梁・通り芯の最小構成）。
	// 器と汎用ヘルパー（StepText / num / ref / point* / direction / makeStorey /
	// contain）は tests/StepText.h の共有定義を使う。ここに残るのは梁の要素固有の
	// 組み立てだけ。
	// -----------------------------------------------------------------------

	// 矩形断面の横架材。
	struct BeamSpec
	{
		double ox = 0.0;
		double oy = 0.0;
		double oz = 0.0;
		bool hasZ = true; // false なら Location を 2 座標にして「Z 無し」を作る
		double dx = 1.0;
		double dy = 0.0;
		double dz = 0.0;
		double width = 120.0;
		double height = 180.0;
		double length = 3000.0;
		std::string material;
		std::string name; // 空文字なら Name を $（未設定）にする
	};

	int makeBeam(StepText& step, int storey, const BeamSpec& spec)
	{
		const int location =
			spec.hasZ ? point3(step, spec.ox, spec.oy, spec.oz) : point2(step, spec.ox, spec.oy);
		const int axis = direction(step, spec.dx, spec.dy, spec.dz);
		const int placement =
			step.add("IFCAXIS2PLACEMENT3D(" + ref(location) + "," + ref(axis) + ",$)");
		const int localPlacement = step.add("IFCLOCALPLACEMENT($," + ref(placement) + ")");

		const int profile = step.add("IFCRECTANGLEPROFILEDEF(.AREA.,$,$," + num(spec.width) + "," +
									 num(spec.height) + ")");
		const int extrudeDir = direction(step, 1.0, 0.0, 0.0);
		const int solid = step.add("IFCEXTRUDEDAREASOLID(" + ref(profile) + ",$," +
								   ref(extrudeDir) + "," + num(spec.length) + ")");
		const int shape =
			step.add("IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(" + ref(solid) + "))");
		const int shapeDef = step.add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + ref(shape) + "))");

		const std::string name = spec.name.empty() ? std::string("$") : "'" + spec.name + "'";
		const int beam = step.add("IFCBEAM('b',$," + name + ",$,$," + ref(localPlacement) + "," +
								  ref(shapeDef) + ",$)");

		if (!spec.material.empty())
		{
			const int material = step.add("IFCMATERIAL('" + spec.material + "')");
			step.add("IFCRELASSOCIATESMATERIAL('m',$,$,$,(" + ref(beam) + ")," + ref(material) +
					 ")");
		}
		contain(step, storey, beam);
		return beam;
	}

	// 登り梁（任意断面＝平行四辺形の側面を厚み方向へ押し出した材）。
	struct SlopedBeamSpec
	{
		double ox = 0.0;
		double oy = 0.0;
		double oz = 0.0;
		double theta = 0.0; // 中心軸の勾配（rad）
		double length = 1000.0;
		double height = 120.0;
		double width = 90.0;
		double shear = 30.0; // 端部直切りによるプロファイルのせん断量
		std::string name;
		int pointCount = 4; // 4=平行四辺形（登り梁）/ 6=登り梁でない形状（筋かい等）
		bool verticalElementAxis = false; // 要素 Axis を鉛直（火打）にする
		bool transpose = false;			  // プロファイルの (u, v) を入れ替える
		int rotate = 0; // プロファイル頂点列を先頭から rotate 個ずらす
		// 非空ならこの頂点列をそのままプロファイルに使う（平行四辺形でない 4 頂点＝
		// 自己交差した断面などを作るため。length / height / shear / pointCount は無視される）。
		std::vector<Vec2> customRing;
	};

	int makeSlopedBeam(StepText& step, int storey, const SlopedBeamSpec& spec)
	{
		const double c = std::cos(spec.theta);
		const double s = std::sin(spec.theta);
		const int location = point3(step, spec.ox, spec.oy, spec.oz);
		std::string axisRef = "$";
		if (spec.verticalElementAxis)
			axisRef = ref(direction(step, 0.0, 0.0, 1.0));
		const int placement =
			step.add("IFCAXIS2PLACEMENT3D(" + ref(location) + "," + axisRef + ",$)");
		const int localPlacement = step.add("IFCLOCALPLACEMENT($," + ref(placement) + ")");

		const double halfHeight = spec.height / 2.0;
		std::vector<Vec2> ring;
		if (!spec.customRing.empty())
		{
			ring = spec.customRing;
		}
		else if (spec.pointCount == 4)
		{
			ring = {Vec2{0.0, -halfHeight}, Vec2{spec.length, -halfHeight},
					Vec2{spec.length + spec.shear, halfHeight}, Vec2{spec.shear, halfHeight}};
		}
		else
		{
			ring = {Vec2{0.0, -halfHeight},		  Vec2{spec.length, -halfHeight},
					Vec2{spec.length, 0.0},		  Vec2{spec.length + spec.shear, halfHeight},
					Vec2{spec.shear, halfHeight}, Vec2{0.0, 0.0}};
		}
		if (spec.transpose)
		{
			for (Vec2& p : ring)
				p = Vec2{p.y, p.x};
		}
		if (spec.rotate != 0)
		{
			const auto shift = static_cast<std::size_t>(spec.rotate) % ring.size();
			std::rotate(ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(shift),
						ring.end());
		}

		std::string points;
		for (const Vec2& p : ring)
			points += (points.empty() ? "" : ",") + ref(point2(step, p.x, p.y));
		points += "," + ref(point2(step, ring.front().x, ring.front().y)); // 閉じる

		const int polyline = step.add("IFCPOLYLINE((" + points + "))");
		const int profile =
			step.add("IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$," + ref(polyline) + ")");

		// ソリッド配置: 局所 X（プロファイル u = 長さ方向）を勾配方向、局所 Z（押し出し方向
		// ＝厚み）をワールド Y に向ける。
		const int refDirection = direction(step, c, 0.0, s);
		const int zAxis = direction(step, 0.0, 1.0, 0.0);
		const int solidOrigin = point3(step, 0.0, 0.0, 0.0);
		const int solidPosition = step.add("IFCAXIS2PLACEMENT3D(" + ref(solidOrigin) + "," +
										   ref(zAxis) + "," + ref(refDirection) + ")");
		const int extrudeDir = direction(step, 0.0, 0.0, 1.0);
		const int solid =
			step.add("IFCEXTRUDEDAREASOLID(" + ref(profile) + "," + ref(solidPosition) + "," +
					 ref(extrudeDir) + "," + num(spec.width) + ")");
		const int shape =
			step.add("IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(" + ref(solid) + "))");
		const int shapeDef = step.add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + ref(shape) + "))");

		const std::string name = spec.name.empty() ? std::string("$") : "'" + spec.name + "'";
		const int beam = step.add("IFCBEAM('b',$," + name + ",$,$," + ref(localPlacement) + "," +
								  ref(shapeDef) + ",$)");
		contain(step, storey, beam);
		return beam;
	}

	// 要素へ材種を関連付ける（RelatingMaterial は IfcMaterial 以外の形も渡せる）。
	void associateMaterial(StepText& step, int element, int material)
	{
		step.add("IFCRELASSOCIATESMATERIAL('m',$,$,$,(" + ref(element) + ")," + ref(material) +
				 ")");
	}

	// 通り芯（センタリング中心の算出に使う）。
	void makeGridAxis(StepText& step, const std::string& tag, double x1, double y1, double x2,
					  double y2)
	{
		const int a = point2(step, x1, y1);
		const int b = point2(step, x2, y2);
		const int polyline = step.add("IFCPOLYLINE((" + ref(a) + "," + ref(b) + "))");
		step.add("IFCGRIDAXIS('" + tag + "'," + ref(polyline) + ",.T.)");
	}

	// -----------------------------------------------------------------------
	// 命令レベルのテスト用ヘルパー
	// -----------------------------------------------------------------------
	struct MemberSpec
	{
		Vec2 start;
		Vec2 end;
		double width = 120.0;
		double height = 180.0;
		double elevation = 473.0;
		double endElevation = 473.0;
		std::string layer = "1-横架材天端";
		std::string memberId = "m";
	};

	MemberCommand member(const MemberSpec& spec)
	{
		MemberCommand command;
		command.layer = spec.layer;
		command.memberId = spec.memberId;
		command.drawClass = CLASS_DODAI;
		command.start = spec.start;
		command.end = spec.end;
		command.width = spec.width;
		command.height = spec.height;
		command.elevation = spec.elevation;
		command.endElevation = spec.endElevation;
		command.startBound = StoryBoundCommand{0, "横架材天端", 0.0};
		command.endBound = StoryBoundCommand{0, "横架材天端", 0.0};
		return command;
	}

	// memberId で命令を探す（無ければ nullptr）。
	const MemberCommand* findById(const std::vector<MemberCommand>& members,
								  const std::string& memberId)
	{
		for (const MemberCommand& m : members)
		{
			if (m.memberId == memberId)
				return &m;
		}
		return nullptr;
	}

	// memberId が suffix で終わる命令を探す（材種名で甲／乙を区別するため）。
	const MemberCommand* findBySuffix(const std::vector<MemberCommand>& members,
									  const std::string& suffix)
	{
		for (const MemberCommand& m : members)
		{
			if (m.memberId.size() >= suffix.size() &&
				m.memberId.compare(m.memberId.size() - suffix.size(), suffix.size(), suffix) == 0)
				return &m;
		}
		return nullptr;
	}
} // namespace

// --------------------------------------------------------------------------
// - makeMemberId
// ---------------------------------------------------------------------------

TEST(make_member_id_joins_section_and_material)
{
	CHECK_EQ(makeMemberId(120.0, 180.0, "杉対称異等級集成材E105-F355"),
			 "120×180 - 杉対称異等級集成材E105-F355");
}

TEST(make_member_id_without_material_is_section_only)
{
	CHECK_EQ(makeMemberId(105.0, 105.0, ""), "105×105");
}

TEST(make_member_id_rounds_dimensions)
{
	CHECK_EQ(makeMemberId(119.6, 180.4, ""), "120×180");
}

// --------------------------------------------------------------------------
// - memberPlacement3D / memberProfileDims / memberMaterialName
// ---------------------------------------------------------------------------

TEST(member_placement_reads_origin_and_axis)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	BeamSpec spec;
	spec.ox = 100.0;
	spec.oy = 200.0;
	spec.oz = -50.0;
	spec.dx = 0.6;
	spec.dz = 0.8;
	const int beam = makeBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	CHECK(element != nullptr);
	MemberPlacement placement;
	if (element != nullptr && memberPlacement3D(model, *element, placement))
	{
		CHECK(near(placement.x, 100.0));
		CHECK(near(placement.y, 200.0));
		CHECK(placement.hasZ);
		CHECK(near(placement.z, -50.0));
		CHECK(near(placement.axis.x, 0.6));
		CHECK(near(placement.axis.z, 0.8));
	}
	else
	{
		CHECK(false);
	}
}

TEST(member_placement_without_z_reports_missing)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	BeamSpec spec;
	spec.hasZ = false;
	const int beam = makeBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	MemberPlacement placement;
	CHECK(element != nullptr && memberPlacement3D(model, *element, placement));
	CHECK(!placement.hasZ);
}

TEST(member_placement_false_without_placement)
{
	Model const model = loadIfcFromText("#1=IFCBEAM('b',$,$,$,$,$,$,$);\n");
	const Entity* element = model.entity(1);
	MemberPlacement placement;
	CHECK(element != nullptr && !memberPlacement3D(model, *element, placement));
}

TEST(member_profile_dims_reads_rectangle)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	BeamSpec spec;
	spec.width = 105.0;
	spec.height = 240.0;
	spec.length = 2730.0;
	const int beam = makeBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	MemberProfile profile;
	CHECK(element != nullptr && memberProfileDims(model, *element, profile));
	CHECK(near(profile.width, 105.0));
	CHECK(near(profile.height, 240.0));
	CHECK(near(profile.length, 2730.0));
}

TEST(member_profile_dims_ignores_non_body_representation)
{
	// Body 以外の表現（例: Axis）に押し出しがあっても矩形断面としては拾わない。
	StepText step;
	const int profile = step.add("IFCRECTANGLEPROFILEDEF(.AREA.,$,$,120.,180.)");
	const int extrudeDir = step.add("IFCDIRECTION((1.,0.,0.))");
	const int solid =
		step.add("IFCEXTRUDEDAREASOLID(" + ref(profile) + ",$," + ref(extrudeDir) + ",3000.)");
	const int shape = step.add("IFCSHAPEREPRESENTATION($,'Axis','Curve3D',(" + ref(solid) + "))");
	const int shapeDef = step.add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + ref(shape) + "))");
	const int beam = step.add("IFCBEAM('b',$,$,$,$,$," + ref(shapeDef) + ",$)");

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	MemberProfile dims;
	CHECK(element != nullptr && !memberProfileDims(model, *element, dims));
}

TEST(member_material_name_reads_associated_material)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	BeamSpec spec;
	spec.material = "杉";
	const int beam = makeBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	CHECK(element != nullptr);
	if (element != nullptr)
		CHECK_EQ(memberMaterialName(model, *element), "杉");
}

TEST(member_material_name_empty_without_association)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	const int beam = makeBeam(step, storey, BeamSpec{});

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	CHECK(element != nullptr);
	if (element != nullptr)
		CHECK_EQ(memberMaterialName(model, *element), "");
}

TEST(member_material_name_reads_material_list)
{
	// IfcMaterialList は先頭の材種名を採る。
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	const int beam = makeBeam(step, storey, BeamSpec{});
	const int first = step.add("IFCMATERIAL('杉')");
	const int second = step.add("IFCMATERIAL('檜')");
	const int list = step.add("IFCMATERIALLIST((" + ref(first) + "," + ref(second) + "))");
	associateMaterial(step, beam, list);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	CHECK(element != nullptr);
	if (element != nullptr)
		CHECK_EQ(memberMaterialName(model, *element), "杉");
}

TEST(member_material_name_empty_for_empty_material_list)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	const int beam = makeBeam(step, storey, BeamSpec{});
	associateMaterial(step, beam, step.add("IFCMATERIALLIST(())"));

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	CHECK(element != nullptr);
	if (element != nullptr)
		CHECK_EQ(memberMaterialName(model, *element), "");
}

TEST(member_material_name_reads_material_layer_set_usage)
{
	// IfcMaterialLayerSetUsage → ForLayerSet → 先頭 MaterialLayer → Material の名前。
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	const int beam = makeBeam(step, storey, BeamSpec{});
	const int material = step.add("IFCMATERIAL('杉集成材')");
	const int layer = step.add("IFCMATERIALLAYER(" + ref(material) + ",120.,$)");
	const int layerSet = step.add("IFCMATERIALLAYERSET((" + ref(layer) + "),'set')");
	const int usage =
		step.add("IFCMATERIALLAYERSETUSAGE(" + ref(layerSet) + ",.AXIS2.,.POSITIVE.,0.)");
	associateMaterial(step, beam, usage);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	CHECK(element != nullptr);
	if (element != nullptr)
		CHECK_EQ(memberMaterialName(model, *element), "杉集成材");
}

TEST(member_material_name_empty_for_incomplete_layer_set_usage)
{
	// ForLayerSet が解決できない／構成層が空 のいずれも空文字（1 材の欠損で止めない）。
	StepText missingSet;
	const int storeyA = makeStorey(missingSet, "1FL", 0.0);
	const int beamA = makeBeam(missingSet, storeyA, BeamSpec{});
	associateMaterial(missingSet, beamA,
					  missingSet.add("IFCMATERIALLAYERSETUSAGE($,.AXIS2.,.POSITIVE.,0.)"));
	Model const withoutSet = missingSet.build();
	const Entity* elementA = withoutSet.entity(beamA);
	CHECK(elementA != nullptr);
	if (elementA != nullptr)
		CHECK_EQ(memberMaterialName(withoutSet, *elementA), "");

	StepText emptyLayers;
	const int storeyB = makeStorey(emptyLayers, "1FL", 0.0);
	const int beamB = makeBeam(emptyLayers, storeyB, BeamSpec{});
	const int layerSet = emptyLayers.add("IFCMATERIALLAYERSET((),'set')");
	associateMaterial(
		emptyLayers, beamB,
		emptyLayers.add("IFCMATERIALLAYERSETUSAGE(" + ref(layerSet) + ",.AXIS2.,.POSITIVE.,0.)"));
	Model const withoutLayers = emptyLayers.build();
	const Entity* elementB = withoutLayers.entity(beamB);
	CHECK(elementB != nullptr);
	if (elementB != nullptr)
		CHECK_EQ(memberMaterialName(withoutLayers, *elementB), "");
}

TEST(member_material_name_empty_for_unresolvable_layer_chain)
{
	// MaterialLayer / その Material が解決できない鎖も空文字（1 材の欠損で止めない）。
	StepText danglingLayer;
	const int storeyA = makeStorey(danglingLayer, "1FL", 0.0);
	const int beamA = makeBeam(danglingLayer, storeyA, BeamSpec{});
	const int setA = danglingLayer.add("IFCMATERIALLAYERSET((#999),'set')");
	associateMaterial(
		danglingLayer, beamA,
		danglingLayer.add("IFCMATERIALLAYERSETUSAGE(" + ref(setA) + ",.AXIS2.,.POSITIVE.,0.)"));
	Model const withDanglingLayer = danglingLayer.build();
	const Entity* elementA = withDanglingLayer.entity(beamA);
	CHECK(elementA != nullptr);
	if (elementA != nullptr)
		CHECK_EQ(memberMaterialName(withDanglingLayer, *elementA), "");

	StepText noLayerMaterial;
	const int storeyB = makeStorey(noLayerMaterial, "1FL", 0.0);
	const int beamB = makeBeam(noLayerMaterial, storeyB, BeamSpec{});
	const int layer = noLayerMaterial.add("IFCMATERIALLAYER($,120.,$)");
	const int setB = noLayerMaterial.add("IFCMATERIALLAYERSET((" + ref(layer) + "),'set')");
	associateMaterial(
		noLayerMaterial, beamB,
		noLayerMaterial.add("IFCMATERIALLAYERSETUSAGE(" + ref(setB) + ",.AXIS2.,.POSITIVE.,0.)"));
	Model const withoutLayerMaterial = noLayerMaterial.build();
	const Entity* elementB = withoutLayerMaterial.entity(beamB);
	CHECK(elementB != nullptr);
	if (elementB != nullptr)
		CHECK_EQ(memberMaterialName(withoutLayerMaterial, *elementB), "");
}

TEST(member_material_name_empty_for_unsupported_material_type)
{
	// 対応しない型（IfcMaterialLayer を直接関連付ける等）と、RelatingMaterial 未設定は空文字。
	StepText unsupported;
	const int storeyA = makeStorey(unsupported, "1FL", 0.0);
	const int beamA = makeBeam(unsupported, storeyA, BeamSpec{});
	associateMaterial(unsupported, beamA, unsupported.add("IFCMATERIALLAYER($,120.,$)"));
	Model const withUnsupported = unsupported.build();
	const Entity* elementA = withUnsupported.entity(beamA);
	CHECK(elementA != nullptr);
	if (elementA != nullptr)
		CHECK_EQ(memberMaterialName(withUnsupported, *elementA), "");

	StepText missing;
	const int storeyB = makeStorey(missing, "1FL", 0.0);
	const int beamB = makeBeam(missing, storeyB, BeamSpec{});
	missing.add("IFCRELASSOCIATESMATERIAL('m',$,$,$,(" + ref(beamB) + "),$)");
	Model const withoutMaterial = missing.build();
	const Entity* elementB = withoutMaterial.entity(beamB);
	CHECK(elementB != nullptr);
	if (elementB != nullptr)
		CHECK_EQ(memberMaterialName(withoutMaterial, *elementB), "");
}

TEST(member_material_name_ignores_association_for_other_elements)
{
	// 自分を RelatedObjects に含まない関連付け（別の梁のもの）は無視する。
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	const int mine = makeBeam(step, storey, BeamSpec{});
	BeamSpec otherSpec;
	otherSpec.oy = 2000.0;
	const int other = makeBeam(step, storey, otherSpec);
	associateMaterial(step, other, step.add("IFCMATERIAL('檜')"));

	Model const model = step.build();
	const Entity* element = model.entity(mine);
	CHECK(element != nullptr);
	if (element != nullptr)
		CHECK_EQ(memberMaterialName(model, *element), "");
}

TEST(member_material_name_ignores_association_not_listing_this_element)
{
	// 自分を**参照はする**が RelatedObjects には含めない関連付けも無視する
	// （逆参照は「どこかで参照している」だけなので、RelatedObjects 側を必ず確認する。
	// parse/Story の collectStoryElements が RelatingStructure を確かめるのと同じ理由）。
	// 併せて RelatedObjects がリストでない（$）関連付けも素通しすることを確かめる。
	StepText step;
	const int storey = makeStorey(step, "1FL", 0.0);
	const int mine = makeBeam(step, storey, BeamSpec{});
	BeamSpec otherSpec;
	otherSpec.oy = 2000.0;
	const int other = makeBeam(step, storey, otherSpec);
	// 自分を RelatingMaterial 側に置いた（＝RelatedObjects には居ない）関連付け。
	step.add("IFCRELASSOCIATESMATERIAL('m',$,$,$,(" + ref(other) + ")," + ref(mine) + ")");
	// RelatedObjects がリストでない関連付け。
	step.add("IFCRELASSOCIATESMATERIAL('n',$,$,$,$," + ref(mine) + ")");

	Model const model = step.build();
	const Entity* element = model.entity(mine);
	CHECK(element != nullptr);
	if (element != nullptr)
		CHECK_EQ(memberMaterialName(model, *element), "");
}

TEST(member_placement_false_for_malformed_placements)
{
	// RelativePlacement が 3D でない／Location が無い／座標が 1 つしかない、のいずれも false。
	const std::string notAxis3D = "#1=IFCAXIS2PLACEMENT2D($,$);\n"
								  "#2=IFCLOCALPLACEMENT($,#1);\n"
								  "#3=IFCBEAM('b',$,$,$,$,#2,$,$);\n";
	const std::string noLocation = "#1=IFCAXIS2PLACEMENT3D($,$,$);\n"
								   "#2=IFCLOCALPLACEMENT($,#1);\n"
								   "#3=IFCBEAM('b',$,$,$,$,#2,$,$);\n";
	const std::string oneCoordinate = "#1=IFCCARTESIANPOINT((0.));\n"
									  "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
									  "#3=IFCLOCALPLACEMENT($,#2);\n"
									  "#4=IFCBEAM('b',$,$,$,$,#3,$,$);\n";
	for (const auto& [text, beamId] :
		 {std::pair{notAxis3D, 3}, std::pair{noLocation, 3}, std::pair{oneCoordinate, 4}})
	{
		Model const model = loadIfcFromText(text);
		const Entity* element = model.entity(beamId);
		MemberPlacement placement;
		CHECK(element != nullptr && !memberPlacement3D(model, *element, placement));
	}
}

TEST(member_profile_dims_false_for_malformed_representations)
{
	// 表現が無い／Representations がリストでない／参照先が解決できない／Items がリストでない／
	// Body の中身が押し出しでない、のいずれも false。
	const std::string noShape = "#1=IFCBEAM('b',$,$,$,$,$,$,$);\n";
	const std::string noRepresentations = "#1=IFCPRODUCTDEFINITIONSHAPE($,$,$);\n"
										  "#2=IFCBEAM('b',$,$,$,$,$,#1,$);\n";
	const std::string danglingRepresentation = "#1=IFCPRODUCTDEFINITIONSHAPE($,$,(#99));\n"
											   "#2=IFCBEAM('b',$,$,$,$,$,#1,$);\n";
	const std::string itemsNotList = "#1=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',$);\n"
									 "#2=IFCPRODUCTDEFINITIONSHAPE($,$,(#1));\n"
									 "#3=IFCBEAM('b',$,$,$,$,$,#2,$);\n";
	const std::string nonExtrudedItem = "#1=IFCCARTESIANPOINT((0.,0.));\n"
										"#2=IFCPOLYLINE((#1));\n"
										"#3=IFCSHAPEREPRESENTATION($,'Body','Curve2D',(#2));\n"
										"#4=IFCPRODUCTDEFINITIONSHAPE($,$,(#3));\n"
										"#5=IFCBEAM('b',$,$,$,$,$,#4,$);\n";
	for (const auto& [text, beamId] : {std::pair{noShape, 1}, std::pair{noRepresentations, 2},
									   std::pair{danglingRepresentation, 2},
									   std::pair{itemsNotList, 3}, std::pair{nonExtrudedItem, 5}})
	{
		Model const model = loadIfcFromText(text);
		const Entity* element = model.entity(beamId);
		MemberProfile profile;
		CHECK(element != nullptr && !memberProfileDims(model, *element, profile));
	}
}

TEST(sloped_geometry_false_without_solid)
{
	// 形状表現の無い材からは中心軸を導出できない。
	Model const model = loadIfcFromText("#1=IFCBEAM('b',$,$,$,$,$,$,$);\n");
	const Entity* element = model.entity(1);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && !slopedMemberGeometry(model, *element, geometry));
}

TEST(sloped_geometry_false_when_end_edge_midpoints_coincide)
{
	// 4 頂点でも**平行四辺形でない**（自己交差した「たすき掛け」の）断面は、両端辺の中点が
	// 一致して中心軸の長さが 0 になる。長さ 0 の軸からは方向が定まらないのでスキップする。
	// 頂点 (0,0)(L,0)(0,H)(L,H) は u/v の広がりが正なのに端辺の中点がどちらも (L/2, H/2)。
	StepText step;
	const int storey = makeStorey(step, "2FL", 0.0);
	SlopedBeamSpec spec;
	spec.customRing = {Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, Vec2{0.0, 120.0}, Vec2{1000.0, 120.0}};
	const int beam = makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && !slopedMemberGeometry(model, *element, geometry));
}

TEST(sloped_geometry_false_for_degenerate_profile)
{
	// 4 頂点でも広がりが 0（一直線に潰れた断面）なら中心軸を導出できない。
	StepText step;
	const int storey = makeStorey(step, "2FL", 0.0);
	SlopedBeamSpec spec;
	spec.height = 0.0; // せいが 0 → プロファイルの v 方向の span が 0
	spec.shear = 0.0;
	const int beam = makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && !slopedMemberGeometry(model, *element, geometry));
}

// --------------------------------------------------------------------------
// - buildMemberCommands: レイヤ・高さ・バインド
// ---------------------------------------------------------------------------

TEST(build_member_commands_empty_model_is_empty)
{
	Model const model = loadIfcFromText("");
	CHECK(buildMemberCommands(model).empty());
}

TEST(build_member_commands_one_per_beam)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	BeamSpec first;
	makeBeam(step, storey, first);
	BeamSpec second;
	second.oy = 1000.0;
	makeBeam(step, storey, second);
	makeStorey(step, "RFL", 5973.0);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(2));
	for (const MemberCommand& m : members)
		CHECK_EQ(m.layer, "1-横架材天端");
}

TEST(top_story_member_uses_eaves_layer_and_top_centre_elevation)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.name = "木梁:小屋梁:1_1";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].layer, "R-軒高");
		// 天端 = ストーリ高さ + ローカル Z(0) + 背/2（断面中心 → 天端補正）
		CHECK(near(members[0].elevation, 5973.0 + 90.0));
		CHECK(near(members[0].endElevation, 5973.0 + 90.0));
	}
}

TEST(top_story_moya_uses_moya_layer)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.name = "木梁:母屋:1_2";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].layer, "R-母屋");
		CHECK_EQ(members[0].drawClass, CLASS_MOYA);
	}
}

TEST(top_story_munagi_uses_moya_layer)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.name = "木梁:棟木:1_1";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].layer, "R-母屋");
		CHECK_EQ(members[0].drawClass, CLASS_MUNAGI);
	}
}

TEST(moya_binds_to_moya_level)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.oz = 300.0;
	spec.name = "木梁:母屋:1_2";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		// 天端 = 5973 + 300 + 90 = 6363、レベル絶対 Z = 5973 → offset = 390
		CHECK_EQ(members[0].startBound.level, "母屋");
		CHECK_EQ(members[0].startBound.storyOffset, 0);
		CHECK(near(members[0].startBound.offset, 390.0));
		CHECK_EQ(members[0].endBound.level, "母屋");
	}
}

TEST(assigns_layer_per_story)
{
	StepText step;
	const int first = makeStorey(step, "1FL", 473.0);
	const int second = makeStorey(step, "2FL", 3273.0);
	makeStorey(step, "RFL", 5973.0);
	makeBeam(step, first, BeamSpec{});
	makeBeam(step, second, BeamSpec{});

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(2));
	const bool hasFirst = std::ranges::any_of(members, [](const MemberCommand& m)
											  { return m.layer == "1-横架材天端"; });
	const bool hasSecond = std::ranges::any_of(members, [](const MemberCommand& m)
											   { return m.layer == "2-横架材天端"; });
	CHECK(hasFirst);
	CHECK(hasSecond);
}

TEST(applies_grid_center_offset)
{
	StepText step;
	// 通り芯 X=0〜2000・Y=0〜2000 → 中心 (1000, 1000)
	makeGridAxis(step, "X1", 0.0, 0.0, 2000.0, 0.0);
	makeGridAxis(step, "Y1", 0.0, 0.0, 0.0, 2000.0);
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.ox = 1500.0;
	spec.oy = 1500.0;
	spec.length = 600.0;
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK(near(members[0].start.x, 500.0));
		CHECK(near(members[0].start.y, 500.0));
		CHECK(near(members[0].end.x, 1100.0));
		CHECK(near(members[0].end.y, 500.0));
		// 天端 = ストーリ高さ 473 + ローカル Z(0) + 背 180 の半分
		CHECK(near(members[0].elevation, 563.0));
	}
}

TEST(uses_beam_local_z_for_elevation)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.oz = -250.0;
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
		CHECK(near(members[0].elevation, 313.0)); // 473 - 250 + 90
}

TEST(elevation_is_section_top_not_centre)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	// 実データと同じ関係: 背 105 の梁が天端 −5 にある場合、中心 Z = −57.5
	BeamSpec spec;
	spec.height = 105.0;
	spec.oz = -57.5;
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK(near(members[0].elevation, 468.0)); // 473 + (-57.5) + 105/2
		CHECK(near(members[0].endElevation, 468.0));
	}
}

TEST(beams_at_different_heights_get_distinct_elevations)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec high;
	high.oz = -48.0;
	makeBeam(step, storey, high);
	BeamSpec low;
	low.oy = 1000.0;
	low.oz = -300.0;
	makeBeam(step, storey, low);

	std::vector<double> elevations;
	for (const MemberCommand& m : buildMemberCommands(step.build()))
		elevations.push_back(m.elevation);
	std::ranges::sort(elevations);
	CHECK_EQ(elevations.size(), std::size_t(2));
	if (elevations.size() == 2)
	{
		CHECK(near(elevations[0], 263.0)); // 473 - 300 + 90
		CHECK(near(elevations[1], 515.0)); // 473 -  48 + 90
	}
}

TEST(sloped_beam_keeps_slope_and_plan_projection)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	// 軸 (0.6, 0, 0.8)・全長 1000・背 180（矩形断面なので軸直交持ち上げ）
	BeamSpec spec;
	spec.dx = 0.6;
	spec.dz = 0.8;
	spec.length = 1000.0;
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		// 軸直交・上向きの単位ベクトル n = (−0.8, 0, 0.6)、背/2 = 90
		// → 断面中心線から (−72, 0, +54) ずらした天端中央線
		CHECK(near(members[0].start.x, -72.0));
		CHECK(near(members[0].start.y, 0.0));
		// 平面投影長 = 0.6 × 1000 = 600
		CHECK(near(members[0].end.x, 528.0));
		CHECK(near(members[0].elevation, 527.0));	  // 473 + 0 + 54
		CHECK(near(members[0].endElevation, 1327.0)); // + 0.8 × 1000
	}
}

TEST(binds_flat_beam_to_beam_top_level_with_zero_offset)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	// 床版（Z=−90）でレイヤ基準高さ（横架材天端）を 473−90=383 にする。
	const int slabPoint = point3(step, 0.0, 0.0, -90.0);
	const int slabPlacement = step.add("IFCAXIS2PLACEMENT3D(" + ref(slabPoint) + ",$,$)");
	const int slabLocal = step.add("IFCLOCALPLACEMENT($," + ref(slabPlacement) + ")");
	const int slab = step.add("IFCSLAB('s',$,$,$,$," + ref(slabLocal) + ",$,$,$)");
	contain(step, storey, slab);
	// 背 180・中心 Z=−180 の梁は天端 = 473−180+90 = 383 でレベルに一致する。
	BeamSpec spec;
	spec.oz = -180.0;
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].startBound.level, "横架材天端");
		CHECK(near(members[0].startBound.offset, 0.0));
		CHECK_EQ(members[0].endBound.level, "横架材天端");
		CHECK(near(members[0].endBound.offset, 0.0));
	}
}

TEST(binds_offset_beam_to_level_distance)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.oz = -250.0;
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		// 横架材天端オフセット無し → レベル絶対 Z = 473。天端 = 473−250+90 = 313。
		CHECK_EQ(members[0].startBound.level, "横架材天端");
		CHECK(near(members[0].startBound.offset, 313.0 - 473.0));
		CHECK(near(members[0].endBound.offset, 313.0 - 473.0));
	}
}

TEST(binds_top_story_beam_to_eaves_level)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.name = "木梁:小屋梁:1_1";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].startBound.level, "軒高");
		CHECK_EQ(members[0].startBound.storyOffset, 0);
		CHECK(near(members[0].startBound.offset, 90.0));
	}
}

TEST(binds_sloped_beam_with_distinct_offsets)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.dx = 0.6;
	spec.dz = 0.8;
	spec.length = 1000.0;
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK(near(members[0].startBound.offset, 527.0 - 473.0));
		CHECK(near(members[0].endBound.offset, 1327.0 - 473.0));
	}
}

TEST(skips_vertical_axis_member)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.dx = 0.0;
	spec.dz = 1.0;
	makeBeam(step, storey, spec);

	CHECK(buildMemberCommands(step.build()).empty());
}

TEST(falls_back_to_layer_elevation_when_local_z_unavailable)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	// 床版（Z=−50）でレイヤ基準高さを 473−50=423 にする（ストーリ高さ 473 ではない）。
	const int slabPoint = point3(step, 0.0, 0.0, -50.0);
	const int slabPlacement = step.add("IFCAXIS2PLACEMENT3D(" + ref(slabPoint) + ",$,$)");
	const int slabLocal = step.add("IFCLOCALPLACEMENT($," + ref(slabPlacement) + ")");
	const int slab = step.add("IFCSLAB('s',$,$,$,$," + ref(slabLocal) + ",$,$,$)");
	contain(step, storey, slab);
	BeamSpec spec;
	spec.hasZ = false; // Location が 2 座標＝ローカル Z を取れない
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		// レイヤ基準高さは既に天端なので背/2 の補正は掛からない。
		CHECK(near(members[0].elevation, 423.0));
		CHECK(near(members[0].endElevation, 423.0));
	}
}

TEST(sets_member_id_and_dimensions)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.material = "杉対称異等級集成材E105-F355";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].memberId, "120×180 - 杉対称異等級集成材E105-F355");
		CHECK(near(members[0].width, 120.0));
		CHECK(near(members[0].height, 180.0));
	}
}

TEST(assigns_class_from_ifc_name)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.name = "木梁:胴差:1_2";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
		CHECK_EQ(members[0].drawClass, CLASS_DOUSASHI);
}

TEST(unnamed_lowest_story_beam_falls_back_to_dodai)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "2FL", 3273.0);
	makeStorey(step, "RFL", 5973.0);
	BeamSpec spec;
	spec.name = "火打:0_1";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
		CHECK_EQ(members[0].drawClass, CLASS_DODAI);
}

TEST(unnamed_top_story_high_beam_falls_back_to_moya_layer)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 5973.0);
	// 軒高 5973 を大きく超える傾斜材（隅木相当・種別トークンでは判別できない）
	BeamSpec spec;
	spec.dx = 0.6;
	spec.dz = 0.8;
	spec.length = 1000.0;
	spec.name = "木梁:隅木・谷木:1_2";
	makeBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].drawClass, CLASS_MOYA);
		// 名前で判別できなくても母屋と推定された材は R-母屋 レイヤに分ける
		CHECK_EQ(members[0].layer, "R-母屋");
	}
}

TEST(skips_beam_without_placement)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	const int beam = step.add("IFCBEAM('b',$,$,$,$,$,$,$)");
	contain(step, storey, beam);

	CHECK(buildMemberCommands(step.build()).empty());
}

TEST(build_trims_interfering_beam_end)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	// 甲梁: Y 方向の通し材（x=0, y=−1000〜1000）・幅 120 → 面は x=±60
	BeamSpec primary;
	primary.oy = -1000.0;
	primary.dx = 0.0;
	primary.dy = 1.0;
	primary.length = 2000.0;
	primary.material = "甲";
	makeBeam(step, storey, primary);
	// 乙梁: X 方向・+x 側から甲の中心線（x=0）まで食い込む
	BeamSpec butting;
	butting.ox = 600.0;
	butting.oy = 500.0;
	butting.dx = -1.0;
	butting.width = 105.0;
	butting.length = 600.0;
	butting.material = "乙";
	makeBeam(step, storey, butting);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	const MemberCommand* otsu = findBySuffix(members, "乙");
	const MemberCommand* kou = findBySuffix(members, "甲");
	CHECK(otsu != nullptr && kou != nullptr);
	if (otsu != nullptr && kou != nullptr)
	{
		// 乙の端点は甲の**芯線**（x=0）に乗り、甲の +x 面（x=60）までの戻りが端部オフセット
		// （−60）に入る（core/Document.h「端部オフセット」）。実際に描かれる範囲は
		// x=60〜600 で、端点を面まで詰めていたころと同じ。
		CHECK(near(otsu->end.x, 0.0));
		CHECK(near(otsu->end.y, 500.0));
		CHECK(near(otsu->endOffset, -60.0));
		CHECK(near(otsu->start.x, 600.0));
		CHECK(near(otsu->startOffset, 0.0)); // 自由端は動かさない
		// 甲（通し材）は変更されない。
		CHECK(near(kou->start.y, -1000.0));
		CHECK(near(kou->end.y, 1000.0));
		CHECK(near(kou->startOffset, 0.0));
		CHECK(near(kou->endOffset, 0.0));
	}
}

// --------------------------------------------------------------------------
// - resolveMemberInterferences
// ---------------------------------------------------------------------------

TEST(t_joint_end_snaps_to_winner_centreline)
{
	const MemberCommand primary =
		member(MemberSpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}, 120.0, 180.0, 473.0, 473.0,
						  "1-横架材天端", "primary"});
	const MemberCommand butting =
		member(MemberSpec{Vec2{600.0, 500.0}, Vec2{0.0, 500.0}, 105.0, 180.0, 473.0, 473.0,
						  "1-横架材天端", "butting"});

	const std::vector<MemberCommand> result = resolveMemberInterferences({primary, butting});
	CHECK_EQ(result.size(), std::size_t(2));
	if (result.size() == 2)
	{
		// 端点は通し材の芯線（x=0）、面（x=60）までの戻りは端部オフセットへ。
		CHECK(near(result[1].end.x, 0.0));
		CHECK(near(result[1].end.y, 500.0));
		CHECK(near(result[1].endOffset, -60.0));
		CHECK(near(result[1].start.x, 600.0));
		// 通し材は不変
		CHECK(near(result[0].start.y, -1000.0));
		CHECK(near(result[0].end.y, 1000.0));
		CHECK(near(result[0].startOffset, 0.0));
		CHECK(near(result[0].endOffset, 0.0));
	}
}

TEST(interference_result_is_order_independent)
{
	const MemberCommand primary = member(MemberSpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}});
	MemberSpec buttingSpec{Vec2{600.0, 500.0}, Vec2{0.0, 500.0}};
	buttingSpec.width = 105.0;
	const MemberCommand butting = member(buttingSpec);

	const std::vector<MemberCommand> a = resolveMemberInterferences({primary, butting});
	const std::vector<MemberCommand> b = resolveMemberInterferences({butting, primary});
	CHECK(near(a[1].end.x, b[0].end.x));
	CHECK(near(a[1].end.y, b[0].end.y));
}

TEST(trims_both_ends_between_two_primaries)
{
	const MemberCommand left = member(MemberSpec{Vec2{-300.0, -1000.0}, Vec2{-300.0, 1000.0}, 120.0,
												 180.0, 473.0, 473.0, "1-横架材天端", "L"});
	const MemberCommand right = member(MemberSpec{Vec2{300.0, -1000.0}, Vec2{300.0, 1000.0}, 120.0,
												  180.0, 473.0, 473.0, "1-横架材天端", "R"});
	const MemberCommand mid = member(MemberSpec{Vec2{-300.0, 0.0}, Vec2{300.0, 0.0}, 105.0, 180.0,
												473.0, 473.0, "1-横架材天端", "mid"});

	const std::vector<MemberCommand> result = resolveMemberInterferences({left, right, mid});
	const MemberCommand* found = findById(result, "mid");
	CHECK(found != nullptr);
	if (found != nullptr)
	{
		// 両端とも相手の芯線（∓300）に乗り、面（∓240）までの戻りが端部オフセットに入る。
		CHECK(near(found->start.x, -300.0));
		CHECK(near(found->end.x, 300.0));
		CHECK(near(found->startOffset, -60.0));
		CHECK(near(found->endOffset, -60.0));
	}
}

TEST(parallel_beams_not_trimmed)
{
	const MemberCommand a = member(MemberSpec{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}});
	const MemberCommand b = member(MemberSpec{Vec2{1000.0, 0.0}, Vec2{2000.0, 0.0}});
	const std::vector<MemberCommand> result = resolveMemberInterferences({a, b});
	CHECK(near(result[0].end.x, 1000.0));
	CHECK(near(result[1].start.x, 1000.0));
}

TEST(symmetric_l_corner_not_trimmed)
{
	// 同寸の材が出隅で相互に食い込む対称な角（勝ち負けが付かない）は触らない。
	const MemberCommand a = member(MemberSpec{Vec2{0.0, 0.0}, Vec2{0.0, 1000.0}});
	const MemberCommand b = member(MemberSpec{Vec2{1000.0, 0.0}, Vec2{0.0, 0.0}});
	const std::vector<MemberCommand> result = resolveMemberInterferences({a, b});
	CHECK(near(result[0].start.x, 0.0));
	CHECK(near(result[0].start.y, 0.0));
	CHECK(near(result[1].end.x, 0.0));
	CHECK(near(result[1].end.y, 0.0));
}

TEST(asymmetric_l_corner_trims_loser)
{
	MemberSpec winnerSpec{Vec2{0.0, 0.0}, Vec2{0.0, 2000.0}};
	winnerSpec.memberId = "win";
	MemberSpec loserSpec{Vec2{1000.0, 0.0}, Vec2{0.0, 0.0}};
	loserSpec.width = 105.0;
	loserSpec.memberId = "lose";

	const std::vector<MemberCommand> result =
		resolveMemberInterferences({member(winnerSpec), member(loserSpec)});
	const MemberCommand* winner = findById(result, "win");
	const MemberCommand* loser = findById(result, "lose");
	CHECK(winner != nullptr && loser != nullptr);
	if (winner != nullptr && loser != nullptr)
	{
		// 出隅（相手の端部での取り合い）は相互の食い込み量で勝ち負けを決める。負け側の端点は
		// 既に勝ち側の芯線（x=0）に乗っているので動かず、面（x=60）までの戻りだけが入る。
		CHECK(near(loser->end.x, 0.0));
		CHECK(near(loser->endOffset, -60.0));
		CHECK(near(loser->start.x, 1000.0));
		CHECK(near(winner->start.y, 0.0));
		CHECK(near(winner->end.y, 2000.0));
		CHECK(near(winner->startOffset, 0.0));
		CHECK(near(winner->endOffset, 0.0));
	}
}

TEST(diagonal_brace_corner_not_trimmed)
{
	// 同寸・同長の斜材が一点で交わる対称な角（火打等）は触らない。
	MemberSpec aSpec{Vec2{0.0, 0.0}, Vec2{1000.0, 1000.0}};
	aSpec.width = 105.0;
	MemberSpec bSpec{Vec2{0.0, 0.0}, Vec2{1000.0, -1000.0}};
	bSpec.width = 105.0;
	const std::vector<MemberCommand> result =
		resolveMemberInterferences({member(aSpec), member(bSpec)});
	CHECK(near(result[0].start.x, 0.0));
	CHECK(near(result[1].start.x, 0.0));
}

TEST(sloped_member_not_trimmed)
{
	// 傾斜梁（両端の天端 Z が異なる材）は詰める側にも相手側にもしない。
	const MemberCommand primary = member(MemberSpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}});
	MemberSpec slopedSpec{Vec2{600.0, 500.0}, Vec2{0.0, 500.0}};
	slopedSpec.width = 105.0;
	slopedSpec.endElevation = 973.0;
	slopedSpec.memberId = "sloped";
	const std::vector<MemberCommand> result =
		resolveMemberInterferences({primary, member(slopedSpec)});
	const MemberCommand* sloped = findById(result, "sloped");
	CHECK(sloped != nullptr);
	if (sloped != nullptr)
	{
		CHECK(near(sloped->end.x, 0.0));
		CHECK(near(sloped->endElevation, 973.0));
	}
}

TEST(member_butting_sloped_member_not_trimmed)
{
	MemberSpec slopedSpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}};
	slopedSpec.endElevation = 1473.0;
	MemberSpec buttingSpec{Vec2{600.0, 500.0}, Vec2{0.0, 500.0}};
	buttingSpec.width = 105.0;
	buttingSpec.memberId = "butting";
	const std::vector<MemberCommand> result =
		resolveMemberInterferences({member(slopedSpec), member(buttingSpec)});
	const MemberCommand* butting = findById(result, "butting");
	CHECK(butting != nullptr);
	if (butting != nullptr)
		CHECK(near(butting->end.x, 0.0));
}

TEST(non_overlapping_z_not_trimmed)
{
	const MemberCommand primary = member(MemberSpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}});
	MemberSpec buttingSpec{Vec2{600.0, 500.0}, Vec2{0.0, 500.0}};
	buttingSpec.elevation = 0.0;
	buttingSpec.endElevation = 0.0;
	const std::vector<MemberCommand> result =
		resolveMemberInterferences({primary, member(buttingSpec)});
	CHECK(near(result[1].end.x, 0.0));
}

TEST(different_layers_not_trimmed)
{
	MemberSpec primarySpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}};
	MemberSpec buttingSpec{Vec2{600.0, 500.0}, Vec2{0.0, 500.0}};
	buttingSpec.layer = "2-横架材天端";
	const std::vector<MemberCommand> result =
		resolveMemberInterferences({member(primarySpec), member(buttingSpec)});
	CHECK(near(result[1].end.x, 0.0));
}

TEST(degenerate_member_is_not_trimmed_and_passes_through)
{
	// 平面投影長が 0 の命令（点に潰れた材）は調整対象にせず、そのまま返す。
	const MemberCommand primary = member(MemberSpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}});
	MemberSpec pointSpec{Vec2{0.0, 500.0}, Vec2{0.0, 500.0}};
	pointSpec.memberId = "point";
	const std::vector<MemberCommand> result =
		resolveMemberInterferences({primary, member(pointSpec)});
	const MemberCommand* point = findById(result, "point");
	CHECK(point != nullptr);
	if (point != nullptr)
	{
		CHECK(near(point->start.x, 0.0));
		CHECK(near(point->start.y, 500.0));
		CHECK(near(point->end.y, 500.0));
	}
}

TEST(non_interfering_beam_unchanged)
{
	// 相手の幅内に達していない（食い込んでいない）端部は不変。
	const MemberCommand primary = member(MemberSpec{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}});
	const MemberCommand far = member(MemberSpec{Vec2{600.0, 500.0}, Vec2{200.0, 500.0}});
	const std::vector<MemberCommand> result = resolveMemberInterferences({primary, far});
	CHECK(near(result[1].end.x, 200.0));
}

// --------------------------------------------------------------------------
// - slopedMemberGeometry（登り梁の任意断面）
// ---------------------------------------------------------------------------

TEST(sloped_geometry_derives_section_and_centre_axis)
{
	StepText step;
	const int storey = makeStorey(step, "2FL", 3361.0);
	SlopedBeamSpec spec;
	spec.ox = 100.0;
	spec.oy = 200.0;
	spec.oz = 10.0;
	spec.theta = std::atan2(0.8, 0.6); // cosθ=0.6・sinθ=0.8
	const int beam = makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && slopedMemberGeometry(model, *element, geometry));
	CHECK(near(geometry.width, 90.0));
	CHECK(near(geometry.height, 120.0));
	CHECK(near(geometry.length, 1000.0, 1e-6));
	// 中心軸は勾配方向 (±0.6, 0, ±0.8) の単位ベクトル
	CHECK(near(std::hypot(geometry.axis.x, geometry.axis.y), 0.6));
	CHECK(near(std::abs(geometry.axis.z), 0.8));
}

TEST(sloped_geometry_when_length_axis_is_second_profile_coord)
{
	StepText step;
	const int storey = makeStorey(step, "2FL", 3361.0);
	SlopedBeamSpec spec;
	spec.theta = std::atan2(0.8, 0.6);
	spec.transpose = true;
	const int beam = makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && slopedMemberGeometry(model, *element, geometry));
	CHECK(near(geometry.width, 90.0));
	CHECK(near(geometry.height, 120.0));
	CHECK(near(geometry.length, 1000.0, 1e-6));
}

TEST(sloped_geometry_when_vertex_order_starts_at_end_edge)
{
	StepText step;
	const int storey = makeStorey(step, "2FL", 3361.0);
	SlopedBeamSpec spec;
	spec.theta = std::atan2(0.8, 0.6);
	spec.rotate = 1;
	const int beam = makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && slopedMemberGeometry(model, *element, geometry));
	CHECK(near(geometry.width, 90.0));
	CHECK(near(geometry.height, 120.0));
	CHECK(near(geometry.length, 1000.0, 1e-6));
}

TEST(sloped_geometry_false_for_rectangle_profile)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	const int beam = makeBeam(step, storey, BeamSpec{});

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && !slopedMemberGeometry(model, *element, geometry));
}

TEST(sloped_geometry_false_for_six_point_profile)
{
	StepText step;
	const int storey = makeStorey(step, "2FL", 3361.0);
	SlopedBeamSpec spec;
	spec.theta = std::atan2(0.8, 0.6);
	spec.pointCount = 6;
	const int beam = makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && !slopedMemberGeometry(model, *element, geometry));
}

// ---------------------------------------------------------------------------
// 登り梁（buildMemberCommands の任意断面経路）
// ---------------------------------------------------------------------------

TEST(noboribari_goes_to_dedicated_layer_with_slope)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	SlopedBeamSpec spec;
	spec.ox = 100.0;
	spec.oy = 200.0;
	spec.oz = 10.0;
	spec.theta = std::atan2(0.8, 0.6);
	spec.name = "木梁:登り梁:1_1";
	makeSlopedBeam(step, storey, spec);

	const std::vector<MemberCommand> members = buildMemberCommands(step.build());
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		CHECK_EQ(members[0].drawClass, CLASS_NOBORIBARI);
		CHECK_EQ(members[0].layer, "1-登り梁");
		CHECK_EQ(members[0].memberId, "90×120");
		// 傾斜: 天端 Z の差 = sinθ × 全長 = 0.8 × 1000
		CHECK(near(std::abs(members[0].elevation - members[0].endElevation), 800.0, 1e-6));
		// 平面投影長 = cosθ × 全長 = 0.6 × 1000
		CHECK(near(std::hypot(members[0].end.x - members[0].start.x,
							  members[0].end.y - members[0].start.y),
				   600.0, 1e-6));
		CHECK_EQ(members[0].startBound.level, "登り梁");
		CHECK_EQ(members[0].endBound.level, "登り梁");
	}
}

TEST(noboribari_uses_vertical_cut_top_without_xy_shift)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	SlopedBeamSpec spec;
	spec.ox = 100.0;
	spec.oy = 200.0;
	spec.oz = 10.0;
	spec.theta = std::atan2(0.8, 0.6);
	spec.name = "木梁:登り梁:1_1";
	const int beam = makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	const Entity* element = model.entity(beam);
	SlopedMemberGeometry geometry;
	CHECK(element != nullptr && slopedMemberGeometry(model, *element, geometry));
	const double horiz = std::hypot(geometry.axis.x, geometry.axis.y);

	const std::vector<MemberCommand> members = buildMemberCommands(model);
	CHECK_EQ(members.size(), std::size_t(1));
	if (members.size() == 1)
	{
		// XY ずらし無し: 端点は断面中心軸の平面投影そのもの（センタリングは無い）。
		CHECK(near(members[0].start.x, geometry.origin.x, 1e-6));
		CHECK(near(members[0].start.y, geometry.origin.y, 1e-6));
		CHECK(
			near(members[0].end.x, geometry.origin.x + (geometry.axis.x * geometry.length), 1e-6));
		// 高さ = 断面中心 + せい/(2·cosθ)（鉛直な端面の上端）
		const double expected = 473.0 + geometry.origin.z + (geometry.height / (2.0 * horiz));
		CHECK(near(members[0].elevation, expected, 1e-6));
		CHECK(near(members[0].endElevation,
				   members[0].elevation + (geometry.axis.z * geometry.length), 1e-6));
		// 矩形前提の軸直交持ち上げ（473 + oz + cosθ·せい/2）より (せい/2)(secθ − cosθ) 高い。
		const double perpendicularTop = 473.0 + geometry.origin.z + (horiz * geometry.height / 2.0);
		CHECK(members[0].elevation > perpendicularTop);
		CHECK(near(members[0].elevation - perpendicularTop,
				   (geometry.height / 2.0) * ((1.0 / horiz) - horiz), 1e-6));
	}
}

TEST(noboribari_level_added_to_its_story)
{
	// 登り梁の命令がある階にだけ "登り梁" レベル（＝レイヤ "1-登り梁"）ができる（parse/Story
	// が命令の配置先レイヤで判定する。Story.cpp の「レベルを足す条件」）。
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	SlopedBeamSpec spec;
	spec.theta = std::atan2(0.8, 0.6);
	spec.name = "木梁:登り梁:1_1";
	makeSlopedBeam(step, storey, spec);

	const std::vector<core::StoryCommand> stories = parse::buildStoryCommands(step.build());
	CHECK_EQ(stories.size(), std::size_t(2));
	if (stories.size() == 2)
	{
		std::vector<std::string> types;
		for (const core::LevelCommand& level : stories[0].levels)
			types.push_back(level.type);
		CHECK(types == (std::vector<std::string>{"FL", "登り梁", "横架材天端"}));
		CHECK_EQ(stories[0].levels[1].layer, "1-登り梁");
		// 最上階には登り梁の命令が無いので "登り梁" レベルを作らない（空レイヤを作らない）。
		for (const core::LevelCommand& level : stories[1].levels)
			CHECK(level.type != "登り梁");
	}
}

TEST(vertical_axis_sloped_beam_skipped)
{
	// 押し出し軸（要素 Axis）が鉛直な材（火打等）は横架材から除外する。
	StepText step;
	const int storey = makeStorey(step, "RFL", 5973.0);
	SlopedBeamSpec spec;
	spec.theta = std::atan2(0.8, 0.6);
	spec.name = "火打:1_1";
	spec.verticalElementAxis = true;
	makeSlopedBeam(step, storey, spec);

	CHECK(buildMemberCommands(step.build()).empty());
}

TEST(sloped_beam_with_vertical_centre_axis_skipped)
{
	// 要素 Axis は水平でも、**任意断面から導いた中心軸**が鉛直な材（束・方杖のように
	// 立った材が登り梁と同じ断面表現で出力された場合）は横架材でないのでスキップする。
	// 鉛直軸の関門は要素 Axis（断面種別より先）と導出軸（登り梁経路の後）の 2 か所にある。
	StepText step;
	const int storey = makeStorey(step, "1FL", 473.0);
	makeStorey(step, "RFL", 5973.0);
	SlopedBeamSpec spec;
	spec.theta = std::numbers::pi / 2.0; // 断面の長さ軸をワールド Z へ向ける
	spec.name = "木梁:登り梁:1_1";
	makeSlopedBeam(step, storey, spec);

	Model const model = step.build();
	// 中心軸そのものは導出できる（長さは正）——弾かれるのは水平成分が無いから。
	const std::vector<core::MemberCommand> members = buildMemberCommands(model);
	CHECK(members.empty());
}

TEST(six_point_profile_beam_skipped)
{
	// 平行四辺形でない任意断面（筋かい等）は取り込まない。
	StepText step;
	const int storey = makeStorey(step, "2FL", 3361.0);
	makeStorey(step, "RFL", 5973.0);
	SlopedBeamSpec spec;
	spec.theta = std::atan2(0.8, 0.6);
	spec.name = "筋かい:1_1";
	spec.pointCount = 6;
	makeSlopedBeam(step, storey, spec);

	CHECK(buildMemberCommands(step.build()).empty());
}

// ---------------------------------------------------------------------------
// 実フィクスチャ（ホームズ君 EX の実 IFC）
// ---------------------------------------------------------------------------

TEST(fixtures_produce_members_with_valid_fields)
{
	forEachFixture(failures,
				   [&](const std::string&, const Model& model)
				   {
					   const std::vector<MemberCommand> members = buildMemberCommands(model);
					   CHECK(!members.empty());
					   for (const MemberCommand& m : members)
					   {
						   CHECK(!m.layer.empty());
						   CHECK(!m.drawClass.empty());
						   CHECK(!m.memberId.empty());
						   CHECK(m.width > 0.0);
						   CHECK(m.height > 0.0);
						   CHECK(!m.startBound.level.empty());
						   CHECK(!m.endBound.level.empty());
						   // 天端中央線が縮退していない（描けない命令を出さない）。
						   CHECK(std::hypot(m.end.x - m.start.x, m.end.y - m.start.y) > 0.0);
					   }
				   });
}

TEST(fixture_member_end_offsets_land_on_the_winner_centreline)
{
	// 実データでも取り合いの端点が芯線へ移り、戻りが端部オフセットに入る。
	//   * 端部オフセットは 0 以下（材を短くする向き）で、
	//   * 実際に描かれる長さ（パス長 ＋ 両端のオフセット）は正のまま、
	//   * オフセットの入った端が**必ず出る**（0 件ならこの調整は何も効いていない）。
	std::size_t adjusted = 0;
	forEachFixture(failures,
				   [&](const std::string&, const Model& model)
				   {
					   for (const MemberCommand& m : buildMemberCommands(model))
					   {
						   CHECK(m.startOffset <= 0.0);
						   CHECK(m.endOffset <= 0.0);
						   const double drawn =
							   std::hypot(m.end.x - m.start.x, m.end.y - m.start.y) +
							   m.startOffset + m.endOffset;
						   CHECK(drawn > 0.0);
						   if (m.startOffset < 0.0 || m.endOffset < 0.0)
							   ++adjusted;
					   }
				   });
	CHECK(adjusted > 0);
}

TEST(fixture_members_are_deterministic)
{
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	if (!ok)
		return;

	const std::vector<MemberCommand> first = buildMemberCommands(model);
	const std::vector<MemberCommand> second = buildMemberCommands(model);
	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size() && i < second.size(); ++i)
	{
		CHECK_EQ(first[i].layer, second[i].layer);
		CHECK_EQ(first[i].memberId, second[i].memberId);
		CHECK(near(first[i].start.x, second[i].start.x));
		CHECK(near(first[i].elevation, second[i].elevation));
	}
}

TEST_MAIN();
