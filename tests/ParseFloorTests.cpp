//
//	ParseFloorTests.cpp
//
//	床板解析（src/parse/Floor）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:
//	core/ parse/ は無 SDK で単体テスト）。Python 版 test_ifc_floor.py の全ケースを
//	1 対 1 で写している。
//
//	検証項目（ROADMAP.md M5）: 床版（IfcSlab "床版"）の抽出・FL レイヤ振り分け
//	（最上階＝屋根には置かない）・スラブ構成（上から 床仕上げ＝FL−横架材天端−24 と
//	床下地＝24）・クラス・**IFC の床位置を尊重した高さ**（elevation ＝ 床仕上げ上端 ＝
//	FL ＋ bound.offset の不変条件、スキップフロアの段差、横架材天端より上の床）・
//	グリッド中心オフセット済みの外形・決定性。実フィクスチャのパスは CMake が
//	HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Floor.h"
#include "parse/Loader.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::FloorCommand;
using HomeskzIfcImport::parse::buildFloorCommands;
using HomeskzIfcImport::parse::CLASS_FLOOR;
using HomeskzIfcImport::parse::collectStories;
using HomeskzIfcImport::parse::floorSlabStyleName;
using HomeskzIfcImport::parse::kSubfloorThickness;
using HomeskzIfcImport::parse::loadIfc;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::StoryInfo;

namespace
{
	// 2 つの実数が許容誤差内で等しいか。高さ・オフセット比較に使う（mm 単位なので
	// Python 版の round(…, 3) と同等の粒度で十分）。
	bool near(double a, double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	// フィクスチャを読む（読み込めなければテスト側で CHECK 失敗させる）。
	Model fixture(const std::string& filename, bool& ok)
	{
		return loadIfc(std::string(HOMESKZ_FIXTURES_DIR) + "/" + filename, &ok);
	}

	// 各 FL レイヤ名 → その階の FL 高さ（絶対 Z）。床仕上げ上端の基準になる。
	std::map<std::string, double> flByLayer(const Model& model)
	{
		std::map<std::string, double> result;
		const std::vector<StoryInfo> stories = collectStories(model);
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			if (stories[i].isTop)
				continue;
			result[std::to_string(i + 1) + "-FL"] = stories[i].elevation;
		}
		return result;
	}

	// 各 FL レイヤ名 → その階の横架材天端（絶対 Z）。スラブ構成（床仕上げ厚）の検証に使う。
	std::map<std::string, double> beamTopByLayer(const Model& model)
	{
		std::map<std::string, double> result;
		const std::vector<StoryInfo> stories = collectStories(model);
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			if (stories[i].isTop)
				continue;
			result[std::to_string(i + 1) + "-FL"] = stories[i].elevation + stories[i].beamOffset;
		}
		return result;
	}

	// 構成層の合計厚。
	double totalThickness(const FloorCommand& floor)
	{
		double total = 0.0;
		for (const HomeskzIfcImport::core::SlabComponentCommand& component : floor.components)
			total += component.thickness;
		return total;
	}

	// 指定レイヤの命令だけを取り出す。
	std::vector<FloorCommand> onLayer(const std::vector<FloorCommand>& floors,
									  const std::string& layer)
	{
		std::vector<FloorCommand> result;
		for (const FloorCommand& floor : floors)
		{
			if (floor.layer == layer)
				result.push_back(floor);
		}
		return result;
	}
} // namespace

// ---------------------------------------------------------------------------
// 合成モデル: 床版の抽出条件（Name / 型 / 最上階除外）
// ---------------------------------------------------------------------------

namespace
{
	// 1FL（Elevation 0）と 2FL（Elevation 3000）の 2 階を持ち、1FL に床版 1 枚
	// （ローカル配置 Z=-120・厚み 28・1000×2000 の鉛直押し出し）を含む最小モデル。
	// 横架材天端オフセットは同じ床版の Z=-120 から決まる（IfcSlab のローカル Z 負値）。
	// slabName を "床版" 以外にすると床板として拾われないことの確認にも使う。
	std::string minimalFloorText(const std::string& slabName)
	{
		return "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
			   "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
			   "#3=IFCLOCALPLACEMENT($,#2);\n"
			   "#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
			   "#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n"
			   // 床版の配置（ローカル Z = -120）
			   "#20=IFCCARTESIANPOINT((0.,0.,-120.));\n"
			   "#21=IFCAXIS2PLACEMENT3D(#20,$,$);\n"
			   "#22=IFCLOCALPLACEMENT(#3,#21);\n"
			   // 断面（原点中心の 1000×2000 矩形）と鉛直押し出し（厚み 28）
			   "#30=IFCCARTESIANPOINT((0.,0.));\n"
			   "#31=IFCAXIS2PLACEMENT2D(#30,$);\n"
			   "#32=IFCRECTANGLEPROFILEDEF(.AREA.,$,#31,1000.,2000.);\n"
			   "#33=IFCCARTESIANPOINT((0.,0.,0.));\n"
			   "#34=IFCAXIS2PLACEMENT3D(#33,$,$);\n"
			   "#35=IFCDIRECTION((0.,0.,1.));\n"
			   "#36=IFCEXTRUDEDAREASOLID(#32,#34,#35,28.);\n"
			   "#37=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#36));\n"
			   "#38=IFCPRODUCTDEFINITIONSHAPE($,$,(#37));\n"
			   "#40=IFCSLAB('slab',$,'" +
			   slabName +
			   "',$,$,#22,#38,$,$);\n"
			   "#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#40),#10);\n";
	}
} // namespace

TEST(extracts_floor_slab_from_minimal_model)
{
	Model const model = loadIfcFromText(minimalFloorText("床版"));
	std::vector<FloorCommand> const floors = buildFloorCommands(model);

	CHECK_EQ(floors.size(), static_cast<std::size_t>(1));
	if (floors.empty())
		return;
	const FloorCommand& floor = floors.front();
	CHECK_EQ(floor.layer, std::string("1-FL"));
	CHECK_EQ(floor.drawClass, std::string(CLASS_FLOOR));
	// スラブスタイルは階ごと（1 階なので "1F-床スタイル"）。
	CHECK_EQ(floor.styleName, std::string("1F-床スタイル"));
	// スラブ構成は上から 床仕上げ（FL 0 − 横架材天端 -120 − 床下地 24 = 96）＋ 床下地 24。
	// IFC の押し出し厚（28）は使わない。
	CHECK_EQ(floor.components.size(), static_cast<std::size_t>(2));
	if (floor.components.size() == 2)
	{
		CHECK_EQ(floor.components[0].name, std::string("床仕上げ"));
		CHECK(near(floor.components[0].thickness, 96.0));
		CHECK_EQ(floor.components[1].name, std::string("床下地"));
		CHECK(near(floor.components[1].thickness, 24.0));
	}
	CHECK(near(totalThickness(floor), 120.0));
	// 床仕上げ上端の絶対 Z ＝ FL（0）。段差が無いので FL ちょうど。
	CHECK(near(floor.elevation, 0.0));
	// 高さ基準は床仕上げ上端（Top）＝FL レベルで、段差が無いので offset は 0。
	CHECK(floor.datum == HomeskzIfcImport::core::SlabDatum::Top);
	CHECK_EQ(floor.bound.storyOffset, 0);
	CHECK_EQ(floor.bound.level, std::string("FL"));
	CHECK(near(floor.bound.offset, 0.0));
	// 外形は矩形 4 点（通り芯が無いモデルなのでセンタリングは無し）。
	CHECK_EQ(floor.boundary.size(), static_cast<std::size_t>(4));
}

TEST(ignores_slabs_with_other_names)
{
	// 屋根版など "床版" 以外の IfcSlab は床板として拾わない。
	Model const model = loadIfcFromText(minimalFloorText("屋根版"));
	CHECK(buildFloorCommands(model).empty());
}

TEST(skips_floor_slab_without_solid)
{
	// 形状表現を持たない（押し出しを解決できない）床版はスキップする。1 枚の欠損で
	// 全体を止めない（CLAUDE.md「エラーハンドリング」）。
	Model const model =
		loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
						"#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
						"#3=IFCLOCALPLACEMENT($,#2);\n"
						"#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
						"#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n"
						"#40=IFCSLAB('slab',$,'床版',$,$,#3,$,$,$);\n"
						"#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#40),#10);\n");
	CHECK(buildFloorCommands(model).empty());
}

TEST(returns_empty_without_stories)
{
	// ストーリが無ければ床板も置けない（空を返し、例外を投げない）。
	Model const model = loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n");
	CHECK(buildFloorCommands(model).empty());
}

TEST(top_story_floor_is_a_loft)
{
	// 最上階（屋根）に属する床版はロフト（小屋裏収納）の床として "R-FL" に配置する。
	// 高さ基準は床下地下端（軒高レベル）で、構成は 床仕上げ 12 ＋ 床下地 24（＝軒高+36）。
	std::string text = minimalFloorText("床版");
	// 収容先を 1FL(#10) から最上階 2FL(#11) へ差し替える。
	const std::string from = "(#40),#10)";
	const std::string to = "(#40),#11)";
	text.replace(text.find(from), from.size(), to);

	Model const model = loadIfcFromText(text);
	std::vector<FloorCommand> const floors = buildFloorCommands(model);
	CHECK_EQ(floors.size(), static_cast<std::size_t>(1));
	if (floors.empty())
		return;
	const FloorCommand& loft = floors.front();
	CHECK_EQ(loft.layer, std::string("R-FL"));
	CHECK_EQ(loft.styleName, std::string("屋根-床スタイル"));
	CHECK_EQ(loft.components.size(), static_cast<std::size_t>(2));
	if (loft.components.size() == 2)
	{
		CHECK(near(loft.components[0].thickness, 12.0)); // 36 − 24
		CHECK(near(loft.components[1].thickness, 24.0));
	}
	CHECK(near(totalThickness(loft), 36.0));
	// 基準面は床下地下端（＝軒高）。床版のローカル最下端 Z(−120) が軒高（3000）からの
	// 高低差になる: 基準面の絶対 Z = 3000 + (−120) = 2880、offset も −120。
	CHECK(loft.datum == HomeskzIfcImport::core::SlabDatum::Bottom);
	CHECK_EQ(loft.bound.level, std::string("軒高"));
	CHECK(near(loft.bound.offset, -120.0));
	CHECK(near(loft.elevation, 2880.0));
}

TEST(story_has_floor_slab_detects_loft)
{
	// parse/Story が屋根階へ FL レベルを足すかの判定に使う。
	std::string text = minimalFloorText("床版");
	const std::string from = "(#40),#10)";
	const std::string to = "(#40),#11)";
	text.replace(text.find(from), from.size(), to);

	Model const model = loadIfcFromText(text);
	CHECK(HomeskzIfcImport::parse::storyHasFloorSlab(model, 11));
	CHECK(!HomeskzIfcImport::parse::storyHasFloorSlab(model, 10));
}

// ---------------------------------------------------------------------------
// スラブスタイル名（階ごと）
// ---------------------------------------------------------------------------

TEST(slab_style_name_is_per_story)
{
	// 一般階は "{階}F-床スタイル"。階により構成（床仕上げ厚）が異なるため階ごとに作る。
	CHECK_EQ(floorSlabStyleName(0, false), std::string("1F-床スタイル"));
	CHECK_EQ(floorSlabStyleName(1, false), std::string("2F-床スタイル"));
	CHECK_EQ(floorSlabStyleName(2, false), std::string("3F-床スタイル"));
	// 最上階は "屋根-床スタイル"（屋根の床＝小屋裏収納・ロフトの床）。
	CHECK_EQ(floorSlabStyleName(3, true), std::string("屋根-床スタイル"));
}

TEST(sample1_style_name_matches_layer)
{
	// 実フィクスチャでも命令のスタイル名が階（レイヤ接頭辞）と対応する。
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	for (const FloorCommand& floor : buildFloorCommands(model))
	{
		// "1-FL" → "1F-床スタイル"、"2-FL" → "2F-床スタイル"。
		const std::string prefix = floor.layer.substr(0, floor.layer.find('-'));
		CHECK_EQ(floor.styleName, prefix + "F-床スタイル");
	}
}

// ---------------------------------------------------------------------------
// 実フィクスチャ: サンプル1（段差の無い床）
// ---------------------------------------------------------------------------

TEST(sample1_has_floor_on_each_non_top_fl_layer)
{
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	std::vector<FloorCommand> const floors = buildFloorCommands(model);

	// 床版は 1FL・2FL に 1 枚ずつ（RFL＝屋根には床板は無い）。
	CHECK_EQ(floors.size(), static_cast<std::size_t>(2));
	CHECK_EQ(onLayer(floors, "1-FL").size(), static_cast<std::size_t>(1));
	CHECK_EQ(onLayer(floors, "2-FL").size(), static_cast<std::size_t>(1));
}

TEST(sample1_components_and_class_are_fixed)
{
	// スラブ構成は上から 床仕上げ（FL − 横架材天端 − 24）＋ 床下地（24mm 固定）で、
	// 合計は FL − 横架材天端。IFC の押し出し厚（28mm 等）は使わない。
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	const std::map<std::string, double> fl = flByLayer(model);
	const std::map<std::string, double> beamTop = beamTopByLayer(model);

	std::vector<FloorCommand> const floors = buildFloorCommands(model);
	CHECK(!floors.empty());
	for (const FloorCommand& floor : floors)
	{
		CHECK_EQ(floor.drawClass, std::string(CLASS_FLOOR));
		CHECK_EQ(floor.components.size(), static_cast<std::size_t>(2));
		if (floor.components.size() != 2)
			continue;
		const auto flIt = fl.find(floor.layer);
		const auto beamIt = beamTop.find(floor.layer);
		CHECK(flIt != fl.end() && beamIt != beamTop.end());
		if (flIt == fl.end() || beamIt == beamTop.end())
			continue;

		const double slab = flIt->second - beamIt->second;
		CHECK(slab > kSubfloorThickness); // 実データは仕上げが 0 に潰れない
		CHECK_EQ(floor.components[0].name, std::string("床仕上げ"));
		CHECK(near(floor.components[0].thickness, slab - kSubfloorThickness));
		CHECK_EQ(floor.components[1].name, std::string("床下地"));
		CHECK(near(floor.components[1].thickness, kSubfloorThickness));
		CHECK(near(totalThickness(floor), slab));
	}
}

TEST(sample1_finish_top_equals_fl_when_no_step)
{
	// 段差の無い床は床仕上げ上端（elevation）が FL（絶対 Z）に一致し、
	// FL レベルへ offset 0 でバインドされる。
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	const std::map<std::string, double> fl = flByLayer(model);

	for (const FloorCommand& floor : buildFloorCommands(model))
	{
		const auto found = fl.find(floor.layer);
		CHECK(found != fl.end());
		if (found == fl.end())
			continue;
		CHECK(near(floor.elevation, found->second));
		CHECK_EQ(floor.bound.storyOffset, 0);
		CHECK_EQ(floor.bound.level, std::string("FL"));
		CHECK(near(floor.bound.offset, 0.0));
	}
}

// ---------------------------------------------------------------------------
// 実フィクスチャ横断: elevation ＝ 横架材天端 ＋ bound.offset の不変条件
// ---------------------------------------------------------------------------

TEST(elevation_equals_fl_plus_offset_in_all_fixtures)
{
	// 床仕上げ上端の絶対 Z（elevation）は「FL」＋「床レベル指定による高低差（offset）」
	// で表される。この不変条件を全フィクスチャで検証する。
	const std::vector<std::string> files = {"サンプル1 (住木邸新築工事).ifc",
											"スキップフロア_サンプル.ifc",
											"グレー本モデルプラン1【3階】.ifc"};
	for (const std::string& file : files)
	{
		bool ok = false;
		Model const model = fixture(file, ok);
		CHECK(ok);
		const std::map<std::string, double> fl = flByLayer(model);

		for (const FloorCommand& floor : buildFloorCommands(model))
		{
			CHECK_EQ(floor.bound.level, std::string("FL"));
			const auto found = fl.find(floor.layer);
			CHECK(found != fl.end());
			if (found == fl.end())
				continue;
			CHECK(near(floor.elevation, found->second + floor.bound.offset));
			// スラブ構成の合計は FL − 横架材天端（＝床仕上げ上端から床下地下端まで）。
			CHECK(totalThickness(floor) >= kSubfloorThickness);
		}
	}
}

TEST(floors_only_on_fl_layers)
{
	// 振り分け先は必ずどこかの階の FL レイヤ（実フィクスチャの床版は非最上階のみ）。
	const std::vector<std::string> files = {"サンプル1 (住木邸新築工事).ifc",
											"スキップフロア_サンプル.ifc",
											"グレー本モデルプラン1【3階】.ifc"};
	for (const std::string& file : files)
	{
		bool ok = false;
		Model const model = fixture(file, ok);
		CHECK(ok);
		const std::map<std::string, double> valid = flByLayer(model);
		for (const FloorCommand& floor : buildFloorCommands(model))
			CHECK(valid.find(floor.layer) != valid.end());
	}
}

// ---------------------------------------------------------------------------
// 実フィクスチャ: スキップフロア（段差）と横架材天端より上の床
// ---------------------------------------------------------------------------

TEST(skip_floor_steps_are_represented)
{
	// スキップフロア_サンプルの 2FL には段差のある床（832mm 下がる）と横架材天端に
	// ある通常の床が混在する。床ごとに実際の高さ（elevation）を持ち、offset に高低差が
	// 現れる（全床を横架材天端へ潰すと段差が失われる）。
	bool ok = false;
	Model const model = fixture("スキップフロア_サンプル.ifc", ok);
	CHECK(ok);
	const std::vector<FloorCommand> twoFL = onLayer(buildFloorCommands(model), "2-FL");
	CHECK(!twoFL.empty());

	bool hasZero = false;
	bool hasStep = false;
	std::set<long long> elevations;
	for (const FloorCommand& floor : twoFL)
	{
		if (near(floor.bound.offset, 0.0))
			hasZero = true;
		if (near(floor.bound.offset, -832.0))
			hasStep = true;
		// 高さが 1 種類に潰れていないことを見るため mm 単位で丸めて数える。
		elevations.insert(static_cast<long long>(std::llround(floor.elevation * 1000.0)));
	}
	CHECK(hasZero);
	CHECK(hasStep);
	CHECK(elevations.size() >= 2);
}

TEST(floor_above_beam_top_respects_ifc_position)
{
	// グレー本モデルプラン1の床は横架材天端より 100〜150mm 高い位置にある。
	// 横架材天端へ潰さず、IFC の床位置（正の offset）を保つ。
	bool ok = false;
	Model const model = fixture("グレー本モデルプラン1【3階】.ifc", ok);
	CHECK(ok);
	std::vector<FloorCommand> const floors = buildFloorCommands(model);
	CHECK(!floors.empty());
	for (const FloorCommand& floor : floors)
		CHECK(floor.bound.offset > 0.0);
}

// ---------------------------------------------------------------------------
// 外形・決定性
// ---------------------------------------------------------------------------

TEST(boundary_is_centered_polygon)
{
	// 外形はグリッド中心オフセット済みの 3 点以上のポリゴン。生の IFC 座標は数万 mm
	// なので、センタリングにより原点近傍（|v| < 30000）に頂点が分布する。
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	std::vector<FloorCommand> const floors = buildFloorCommands(model);
	CHECK(!floors.empty());

	double maxAbs = 0.0;
	for (const FloorCommand& floor : floors)
	{
		CHECK(floor.boundary.size() >= 3);
		for (const core::Vec2& point : floor.boundary)
		{
			maxAbs = std::max(maxAbs, std::abs(point.x));
			maxAbs = std::max(maxAbs, std::abs(point.y));
		}
	}
	CHECK(maxAbs < 30000.0);
}

TEST(is_deterministic)
{
	// 同じ入力からは同じ命令列（順序・値）が得られる（エンティティ列挙順に依存しない）。
	bool ok = false;
	Model const model = fixture("スキップフロア_サンプル.ifc", ok);
	CHECK(ok);
	std::vector<FloorCommand> const first = buildFloorCommands(model);
	std::vector<FloorCommand> const second = buildFloorCommands(model);

	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size() && i < second.size(); ++i)
	{
		CHECK_EQ(first[i].layer, second[i].layer);
		CHECK(near(first[i].elevation, second[i].elevation));
		CHECK(near(first[i].bound.offset, second[i].bound.offset));
		CHECK_EQ(first[i].boundary.size(), second[i].boundary.size());
	}
}

TEST_MAIN()
