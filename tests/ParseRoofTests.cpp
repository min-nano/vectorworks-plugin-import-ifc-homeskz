//
//	ParseRoofTests.cpp
//
//	野地板解析（src/parse/Roof）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:
//	core/ parse/ は無 SDK で単体テスト）。Python 版 test_ifc_roof.py の全ケースを
//	1 対 1 で写している。
//
//	検証項目（ROADMAP.md M6）: 厚み 12mm 固定・クラスとレイヤ・平面外形（footprint）・
//	軒（屋根軸）が最も低い辺に乗ること・upslope が棟側を指すこと・勾配（rise/run）・
//	軒の目標 Z（屋根版の平面＋垂木せいの鉛直換算）・センタリング・退化面のスキップ・
//	屋根版 1 面 = 野地板 1 枚・決定性。実フィクスチャのパスは CMake が
//	HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"
#include "Fixtures.h"
#include "RoofSample.h"

#include "core/Document.h"
#include "parse/IfcGeometry.h"
#include "parse/Loader.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::RoofCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::Vec3;
using HomeskzIfcImport::parse::buildRoofCommands;
using HomeskzIfcImport::parse::CLASS_ROOF_SHEATHING;
using HomeskzIfcImport::parse::kDefaultRafterHeight;
using HomeskzIfcImport::parse::kNojiitaThickness;
using HomeskzIfcImport::parse::loadIfc;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::roofCommandForPlane;
using HomeskzIfcImport::parse::RoofPlane;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::minimalRoofText;
using HomeskzIfcTests::near;
using HomeskzIfcTests::shedPlane;

namespace
{
	// 試験用の屋根面（4m×3m の片流れ）と、それに対応する最小の屋根版 IFC は
	// tests/RoofSample.h が唯一の定義。垂木（ParseRafterTests）と共有する。

	// 上の屋根面から野地板命令を作る（作れなければテスト側で CHECK 失敗させる）。
	RoofCommand shedRoof(bool& ok, double storeyElevation = 0.0,
						 const Vec2& center = Vec2{0.0, 0.0})
	{
		std::optional<RoofCommand> command =
			roofCommandForPlane(shedPlane(), "R-野地板", storeyElevation, center);
		ok = command.has_value();
		return ok ? *command : RoofCommand{};
	}
} // namespace

// ---------------------------------------------------------------------------
// 1 つの屋根面からの野地板（roofCommandForPlane）
// ---------------------------------------------------------------------------

TEST(default_thickness_is_12mm)
{
	bool ok = false;
	RoofCommand const roof = shedRoof(ok);
	CHECK(ok);
	CHECK(near(roof.thickness, 12.0));
	CHECK(near(kNojiitaThickness, 12.0));
}

TEST(roof_class_and_layer)
{
	bool ok = false;
	RoofCommand const roof = shedRoof(ok);
	CHECK(ok);
	CHECK_EQ(roof.drawClass, std::string(CLASS_ROOF_SHEATHING));
	CHECK_EQ(roof.layer, std::string("R-野地板"));
}

TEST(boundary_is_plan_footprint)
{
	// 平面外形の XY（押し出しの水平投影）。4 頂点で、入力の周り方向を保つ。
	bool ok = false;
	RoofCommand const roof = shedRoof(ok);
	CHECK(ok);
	CHECK_EQ(roof.boundary.size(), static_cast<std::size_t>(4));
	if (roof.boundary.size() != 4)
		return;
	CHECK(near(roof.boundary[0].x, 0.0) && near(roof.boundary[0].y, 0.0));
	CHECK(near(roof.boundary[1].x, 4000.0) && near(roof.boundary[1].y, 0.0));
	CHECK(near(roof.boundary[2].x, 4000.0) && near(roof.boundary[2].y, 3000.0));
	CHECK(near(roof.boundary[3].x, 0.0) && near(roof.boundary[3].y, 3000.0));
}

TEST(axis_lies_on_eaves_low_edge)
{
	bool ok = false;
	RoofCommand const roof = shedRoof(ok);
	CHECK(ok);
	// 軒（軸）は最も低い辺 y=0 上。軸の 2 点はどちらも y=0。
	CHECK(near(roof.axisStart.y, 0.0));
	CHECK(near(roof.axisEnd.y, 0.0));
	// 軸は軒に沿って X 方向（footprint 幅 4000）に伸びる。
	CHECK(near(std::abs(roof.axisEnd.x - roof.axisStart.x), 4000.0));
}

TEST(upslope_points_toward_ridge)
{
	bool ok = false;
	RoofCommand const roof = shedRoof(ok);
	CHECK(ok);
	// upslope 定義点は軒（y=0）から棟（+Y）側を指す。
	CHECK(roof.upslope.y > roof.axisStart.y);
}

TEST(rise_run_encode_slope)
{
	bool ok = false;
	RoofCommand const roof = shedRoof(ok);
	CHECK(ok);
	// slope = rise/run = dh/nz = tan(勾配角)。この面は Y 方向に 1/3 勾配。
	CHECK(roof.run > 0.0);
	if (roof.run > 0.0)
		CHECK(near(roof.rise / roof.run, 1.0 / 3.0, 1e-9));
}

TEST(elevation_is_rafter_top)
{
	bool ok = false;
	RoofCommand const roof = shedRoof(ok, 6300.0);
	CHECK(ok);
	// 軒の目標 Z ＝ 屋根版の平面（1000 ＋ ストーリ Elevation）から垂木せい（45）を
	// 鉛直換算（÷cosθ＝nz）して持ち上げた値（野地板下端＝垂木上端。垂木下端＝屋根版の
	// 平面は Python 版の VW 上の実測で確認済み）。
	const double nz = 3.0 / std::sqrt(10.0);
	const double lift = kDefaultRafterHeight / nz;
	CHECK(near(roof.elevation, 1000.0 + 6300.0 + lift));
}

TEST(roof_center_offset_subtracted_from_xy)
{
	bool ok = false;
	RoofCommand const roof = shedRoof(ok, 0.0, Vec2{100.0, 200.0});
	CHECK(ok);
	CHECK(!roof.boundary.empty());
	if (roof.boundary.empty())
		return;
	CHECK(near(roof.boundary.front().x, -100.0));
	CHECK(near(roof.boundary.front().y, -200.0));
	CHECK(near(roof.axisStart.y, -200.0));
}

TEST(flat_plane_returns_no_command)
{
	// 法線が鉛直（水平な面）なら勾配方向が定まらず命令を作らない。
	RoofPlane flat;
	flat.vertices = {Vec3{0.0, 0.0, 0.0}, Vec3{1000.0, 0.0, 0.0}, Vec3{1000.0, 1000.0, 0.0}};
	flat.normal = Vec3{0.0, 0.0, 1.0};
	CHECK(!roofCommandForPlane(flat, "R-野地板", 0.0, Vec2{0.0, 0.0}).has_value());
}

TEST(vertical_plane_returns_no_command)
{
	// 法線が水平（鉛直な面）は勾配・天端 Z が定まらない（平面式が nz で除算する）。
	RoofPlane vertical;
	vertical.vertices = {Vec3{0.0, 0.0, 0.0}, Vec3{1000.0, 0.0, 0.0}, Vec3{1000.0, 0.0, 1000.0}};
	vertical.normal = Vec3{0.0, 1.0, 0.0};
	CHECK(!roofCommandForPlane(vertical, "R-野地板", 0.0, Vec2{0.0, 0.0}).has_value());
}

TEST(degenerate_span_returns_no_command)
{
	// 広がりが極小（線状）の屋根版はスキップする。
	const double s = std::sqrt(10.0);
	RoofPlane sliver;
	sliver.vertices = {Vec3{0.0, 0.0, 1000.0}, Vec3{0.5, 0.0, 1000.0}, Vec3{0.5, 0.9, 1000.3},
					   Vec3{0.0, 0.9, 1000.3}};
	sliver.normal = Vec3{0.0, -1.0 / s, 3.0 / s};
	CHECK(!roofCommandForPlane(sliver, "R-野地板", 0.0, Vec2{0.0, 0.0}).has_value());
}

// ---------------------------------------------------------------------------
// 合成モデル: 屋根版の抽出条件
// ---------------------------------------------------------------------------

// 最小の屋根版 IFC（minimalRoofText）は tests/RoofSample.h が唯一の定義で、垂木
// （ParseRafterTests）と共有する。slabName を "屋根版" 以外にすると拾われないことの確認に使う。

TEST(extracts_one_roof_per_roof_slab)
{
	Model const model = loadIfcFromText(minimalRoofText("屋根版:1"));
	std::vector<RoofCommand> const roofs = buildRoofCommands(model);
	CHECK_EQ(roofs.size(), static_cast<std::size_t>(1));
	if (roofs.empty())
		return;
	CHECK_EQ(roofs.front().layer, std::string("R-野地板"));
	CHECK_EQ(roofs.front().drawClass, std::string(CLASS_ROOF_SHEATHING));
	CHECK(near(roofs.front().thickness, 12.0));
	CHECK(roofs.front().boundary.size() >= 3);
}

TEST(ignores_slabs_with_other_names)
{
	Model const model = loadIfcFromText(minimalRoofText("床版"));
	CHECK(buildRoofCommands(model).empty());
}

TEST(skips_roof_slab_without_plane)
{
	// 形状表現を持たない（屋根面を解決できない）屋根版はスキップする。
	Model const model =
		loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
						"#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
						"#3=IFCLOCALPLACEMENT($,#2);\n"
						"#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
						"#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n"
						"#40=IFCSLAB('slab',$,'屋根版:1',$,$,#3,$,$,$);\n"
						"#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#40),#11);\n");
	CHECK(buildRoofCommands(model).empty());
}

TEST(returns_empty_without_stories)
{
	Model const model = loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n");
	CHECK(buildRoofCommands(model).empty());
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(fixture_roofs_are_valid)
{
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	std::vector<RoofCommand> const roofs = buildRoofCommands(model);
	CHECK(!roofs.empty());
	for (const RoofCommand& roof : roofs)
	{
		CHECK(near(roof.thickness, 12.0));
		CHECK_EQ(roof.drawClass, std::string(CLASS_ROOF_SHEATHING));
		CHECK(roof.layer.rfind("-野地板") == roof.layer.size() - std::string("-野地板").size());
		CHECK(roof.boundary.size() >= 3);
		CHECK(roof.run > 0.0);
		// 軸は退化しない（BeginRoof が軒の向きを取れる長さを持つ）。
		CHECK(std::hypot(roof.axisEnd.x - roof.axisStart.x, roof.axisEnd.y - roof.axisStart.y) >
			  0.0);
	}
}

TEST(fixture_layers_map_to_roof_storeys)
{
	// 伏図次郎: 下屋根（2FL）→ "2-野地板"、主屋根（RFL）→ "R-野地板"。
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	std::set<std::string> layers;
	for (const RoofCommand& roof : buildRoofCommands(model))
		layers.insert(roof.layer);
	CHECK_EQ(layers.size(), static_cast<std::size_t>(2));
	CHECK(layers.count("2-野地板") == 1);
	CHECK(layers.count("R-野地板") == 1);
}

TEST(one_roof_per_roof_slab_plane)
{
	// 野地板は屋根版 1 面につき 1 枚（垂木のように 455 間隔で割らない）。勾配のある面の
	// 数だけできるので、屋根版の枚数以下（水平面はスキップ）。
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	std::size_t roofSlabs = 0;
	for (const int id : model.byType("IFCSLAB"))
	{
		const HomeskzIfcImport::parse::Entity* slab = model.entity(id);
		if (slab == nullptr)
			continue;
		const HomeskzIfcImport::parse::Value& name = slab->attribute(2);
		if (name.type == HomeskzIfcImport::parse::ValueType::String &&
			name.text.compare(0, std::string("屋根版").size(), "屋根版") == 0)
			++roofSlabs;
	}
	const std::size_t roofs = buildRoofCommands(model).size();
	CHECK(roofs > 0);
	CHECK(roofs <= roofSlabs);
}

TEST(roofs_are_deterministic)
{
	bool ok = false;
	Model const model = fixture("グレー本モデルプラン1【3階】.ifc", ok);
	CHECK(ok);
	std::vector<RoofCommand> const first = buildRoofCommands(model);
	std::vector<RoofCommand> const second = buildRoofCommands(model);

	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size() && i < second.size(); ++i)
	{
		CHECK_EQ(first[i].layer, second[i].layer);
		CHECK(near(first[i].elevation, second[i].elevation));
		CHECK(near(first[i].rise, second[i].rise));
		CHECK(near(first[i].run, second[i].run));
		CHECK_EQ(first[i].boundary.size(), second[i].boundary.size());
	}
}

TEST_MAIN()
