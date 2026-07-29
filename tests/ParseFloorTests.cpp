//
//	ParseFloorTests.cpp
//
//	床板解析（src/parse/Floor）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:
//	core/ parse/ は無 SDK で単体テスト）。Python 版 test_ifc_floor.py の全ケースを
//	1 対 1 で写している。
//
//	検証項目（ROADMAP.md M5）: 床版（IfcSlab "床版"）の抽出・FL レイヤ振り分け
//	（最上階＝屋根には置かない）・厚み 24mm 固定・クラス・**IFC の床位置を尊重した
//	高さ**（elevation ＝ 横架材天端 ＋ bound.offset の不変条件、スキップフロアの段差、
//	横架材天端より上の床）・グリッド中心オフセット済みの外形・決定性。実フィクスチャの
//	パスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
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
using HomeskzIfcImport::parse::kFloorThickness;
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

	// 各 FL レイヤ名 → その階の横架材天端（絶対 Z）。Python 版テストの
	// _beam_top_by_layer と同じ（最上階は FL レイヤを持たないので除く）。
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
	// 厚みは IFC の押し出し厚（28）ではなく 24mm 固定。
	CHECK(near(floor.thickness, 24.0));
	// 床下端の絶対 Z ＝ ストーリ高さ(0) ＋ ローカル最下端 Z(-120)。
	CHECK(near(floor.elevation, -120.0));
	// 横架材天端（= 0 + (-120)）ちょうどなので offset は 0。
	CHECK_EQ(floor.bound.storyOffset, 0);
	CHECK_EQ(floor.bound.level, std::string("横架材天端"));
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

TEST(skips_top_story)
{
	// 床版が最上階（屋根）に属していれば FL レイヤが無いので配置しない。
	std::string text = minimalFloorText("床版");
	// 収容先を 1FL(#10) から最上階 2FL(#11) へ差し替える。
	const std::string from = "(#40),#10)";
	const std::string to = "(#40),#11)";
	text.replace(text.find(from), from.size(), to);

	Model const model = loadIfcFromText(text);
	CHECK(buildFloorCommands(model).empty());
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

TEST(sample1_thickness_and_class_are_fixed)
{
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	std::vector<FloorCommand> const floors = buildFloorCommands(model);
	CHECK(!floors.empty());
	for (const FloorCommand& floor : floors)
	{
		// 厚みは IFC の押し出し厚（28mm 等）ではなく要件どおり 24mm 固定。
		CHECK(near(floor.thickness, kFloorThickness));
		CHECK(near(kFloorThickness, 24.0));
		CHECK_EQ(floor.drawClass, std::string(CLASS_FLOOR));
	}
}

TEST(sample1_bottom_elevation_equals_beam_top_when_no_step)
{
	// 段差の無い床は床下端（elevation）が横架材天端（絶対 Z）に一致し、
	// 横架材天端レベルへ offset 0 でバインドされる。
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	const std::map<std::string, double> beamTop = beamTopByLayer(model);

	for (const FloorCommand& floor : buildFloorCommands(model))
	{
		const auto found = beamTop.find(floor.layer);
		CHECK(found != beamTop.end());
		if (found == beamTop.end())
			continue;
		CHECK(near(floor.elevation, found->second));
		CHECK_EQ(floor.bound.storyOffset, 0);
		CHECK_EQ(floor.bound.level, std::string("横架材天端"));
		CHECK(near(floor.bound.offset, 0.0));
	}
}

// ---------------------------------------------------------------------------
// 実フィクスチャ横断: elevation ＝ 横架材天端 ＋ bound.offset の不変条件
// ---------------------------------------------------------------------------

TEST(elevation_equals_beam_top_plus_offset_in_all_fixtures)
{
	// 床下端の絶対 Z（elevation）は「標準の床高（横架材天端）」＋「基準高さからの
	// 高低差（offset）」で表される。この不変条件を全フィクスチャで検証する。
	const std::vector<std::string> files = {"サンプル1 (住木邸新築工事).ifc",
											"スキップフロア_サンプル.ifc",
											"グレー本モデルプラン1【3階】.ifc"};
	for (const std::string& file : files)
	{
		bool ok = false;
		Model const model = fixture(file, ok);
		CHECK(ok);
		const std::map<std::string, double> beamTop = beamTopByLayer(model);

		for (const FloorCommand& floor : buildFloorCommands(model))
		{
			CHECK_EQ(floor.bound.level, std::string("横架材天端"));
			const auto found = beamTop.find(floor.layer);
			CHECK(found != beamTop.end());
			if (found == beamTop.end())
				continue;
			CHECK(near(floor.elevation, found->second + floor.bound.offset));
		}
	}
}

TEST(floors_only_on_non_top_fl_layers)
{
	// 振り分け先は必ず非最上階の FL レイヤ（屋根に床板を置かない）。
	const std::vector<std::string> files = {"サンプル1 (住木邸新築工事).ifc",
											"スキップフロア_サンプル.ifc",
											"グレー本モデルプラン1【3階】.ifc"};
	for (const std::string& file : files)
	{
		bool ok = false;
		Model const model = fixture(file, ok);
		CHECK(ok);
		const std::map<std::string, double> valid = beamTopByLayer(model);
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
