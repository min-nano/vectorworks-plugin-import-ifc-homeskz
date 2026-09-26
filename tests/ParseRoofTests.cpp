//
//	ParseRoofTests.cpp
//
//	野地板解析（src/parse/Roof）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:core/ parse/ は無 SDK
//	で単体テスト）。**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M6）: 厚み 12mm 固定・クラスとレイヤ・平面外形（footprint）・
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
	// 点列を dir へ射影した [最小, 最大]。
	void projectRange(const std::vector<Vec2>& points, const Vec2& dir, double& outMin,
					  double& outMax)
	{
		outMin = 0.0;
		outMax = 0.0;
		for (std::size_t i = 0; i < points.size(); ++i)
		{
			const double v = (points[i].x * dir.x) + (points[i].y * dir.y);
			if (i == 0 || v < outMin)
				outMin = v;
			if (i == 0 || v > outMax)
				outMax = v;
		}
	}

	// 軒軸の不変条件（core/Document.h の RoofCommand）を、**軸の向きから勾配座標系を
	// 復元して**確かめる。
	//   * 軸は軒方向 e で footprint の射影範囲ちょうどを端から端まで張る
	//   * 軸の 2 点は勾配方向 d の片方の端（軒側）に乗る
	//   * upslope 定義点は逆の端に乗り、e は範囲の内側
	//
	// ★**xy の外接矩形では見ない。** 非矩形の footprint では外接矩形の外に出うる
	// （三角形の屋根面では軒が 1 頂点に退化するため。理由は core/Document.h）。ここで
	// 押さえたいのは「射影範囲 1 つぶん余計に伸びていないこと」である。
	//
	// CHECK マクロは囲みスコープの failures 変数を使う設計（TestFramework.h）なので、
	// 呼び出し側の failures を明示的に受け取る（tests/Fixtures.h の forEachFixture と同じ）。
	void checkAxisSpansFootprint(int& failures, const RoofCommand& roof)
	{
		const Vec2 delta{roof.axisEnd.x - roof.axisStart.x, roof.axisEnd.y - roof.axisStart.y};
		const double length = std::hypot(delta.x, delta.y);
		CHECK(length > 0.0);
		if (!(length > 0.0))
			return;
		const Vec2 along{delta.x / length, delta.y / length};
		const Vec2 down{-along.y, along.x}; // 軸に直交（向きの符号は問わない）
		constexpr double kTol = 1e-4; // mm。射影を数回通すだけなので十分厳しい

		double eMin = 0.0;
		double eMax = 0.0;
		double dMin = 0.0;
		double dMax = 0.0;
		projectRange(roof.boundary, along, eMin, eMax);
		projectRange(roof.boundary, down, dMin, dMax);

		double axisEMin = 0.0;
		double axisEMax = 0.0;
		double axisDMin = 0.0;
		double axisDMax = 0.0;
		const std::vector<Vec2> axis{roof.axisStart, roof.axisEnd};
		projectRange(axis, along, axisEMin, axisEMax);
		projectRange(axis, down, axisDMin, axisDMax);

		// 軒方向は footprint の広がりちょうど。**旧実装はここで落ちる**（選んだ頂点から
		// 広がりぶん伸ばしていたので、軸の範囲が footprint の範囲とずれる）。
		CHECK(std::abs(axisEMin - eMin) <= kTol);
		CHECK(std::abs(axisEMax - eMax) <= kTol);
		// 軸は勾配方向のどちらか片方の端に乗る（2 点とも同じ d）。
		CHECK(std::abs(axisDMax - axisDMin) <= kTol);
		const bool axisAtLowEnd = std::abs(axisDMin - dMin) <= kTol;
		const bool axisAtHighEnd = std::abs(axisDMax - dMax) <= kTol;
		CHECK(axisAtLowEnd || axisAtHighEnd);

		// upslope 定義点は逆の端、e は範囲の内側。
		const double upslopeD = (roof.upslope.x * down.x) + (roof.upslope.y * down.y);
		const double upslopeE = (roof.upslope.x * along.x) + (roof.upslope.y * along.y);
		CHECK(std::abs(upslopeD - (axisAtHighEnd ? dMin : dMax)) <= kTol);
		CHECK(upslopeE >= eMin - kTol && upslopeE <= eMax + kTol);
	}
} // namespace

// --------------------------------------------------------------------------
// - 1 つの屋根面からの野地板（roofCommandForPlane）
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

TEST(axis_stays_inside_the_footprint_whatever_the_winding)
{
	// ★**軸を footprint の外へ出さない**（M29）。かつては「最も軒側の頂点を 1 つ選び、
	// そこから軒方向へ広がりぶん伸ばす」作りで、**選ばれた頂点が軒方向の終わり側に在ると
	// 終点が footprint 1 つぶん外へ飛び出して**いた。屋根面オブジェクトはこの軸を勾配の
	// 基準線として図に描くので、飛び出した軸がビューポートの外形を広げ、伏図が用紙に
	// 収まらなくなる（実機の母屋伏図が縦に 5,680mm 大きく測られた）。
	//
	// shedPlane（頂点が (0,0) から始まる）ではたまたま正しい端が選ばれるので、**周り方向を
	// 逆にした同じ矩形**で押さえる——軒（y=0）の頂点が先に (4000,0) の側で見つかる並び。
	using HomeskzIfcImport::parse::RoofPlane;
	const double s = std::sqrt(10.0);
	RoofPlane plane;
	plane.vertices = {Vec3{4000.0, 0.0, 1000.0}, Vec3{0.0, 0.0, 1000.0}, Vec3{0.0, 3000.0, 2000.0},
					  Vec3{4000.0, 3000.0, 2000.0}};
	plane.normal = Vec3{0.0, -1.0 / s, 3.0 / s};

	std::optional<RoofCommand> const command =
		roofCommandForPlane(plane, "R-野地板", 0.0, Vec2{0.0, 0.0});
	CHECK(command.has_value());
	if (!command.has_value())
		return;
	const RoofCommand& roof = *command;

	// 軸は軒（y=0）の辺を端から端まで：x は 0〜4000 に収まり、長さは軒の広がりちょうど。
	CHECK(near(roof.axisStart.y, 0.0));
	CHECK(near(roof.axisEnd.y, 0.0));
	CHECK(near(std::abs(roof.axisEnd.x - roof.axisStart.x), 4000.0));
	for (const Vec2& point : {roof.axisStart, roof.axisEnd})
	{
		CHECK(point.x >= -1e-6 && point.x <= 4000.0 + 1e-6);
	}
	// upslope 定義点も footprint の内側（棟側の中央）。
	CHECK(near(roof.upslope.y, 3000.0));
	CHECK(roof.upslope.x >= -1e-6 && roof.upslope.x <= 4000.0 + 1e-6);
	checkAxisSpansFootprint(failures, roof);
}

TEST(axis_spans_the_projection_range_on_a_non_rectangular_face)
{
	// **非矩形の footprint でも射影範囲ちょうど**であることを、勾配の向きを 45°振った
	// 直角三角形で押さえる（実フィクスチャには三角形の屋根面が 4 面ある）。
	//
	// ★**この面では「xy の外接矩形に収まる」は成り立たない**——軒（勾配方向 d の最大）が
	// 1 頂点に退化するので、その点を通る軒の直線上に長さを持つ線分を取れば、必ず外接矩形の
	// 外へ出る。原理的に避けられないので、**不変条件は射影範囲で言う**（core/Document.h の
	// RoofCommand）。ここで守りたいのは「射影範囲 1 つぶん余計に伸びない」ことである。
	using HomeskzIfcImport::parse::RoofPlane;
	// z = (x + y)/2 ⇒ 上向き法線 ∝ (−1, −1, 2)。勾配方向は xy 平面で 45°を向く。
	const double s = std::sqrt(6.0);
	RoofPlane plane;
	plane.vertices = {Vec3{0.0, 0.0, 0.0}, Vec3{10000.0, 0.0, 5000.0}, Vec3{0.0, 10000.0, 5000.0}};
	plane.normal = Vec3{-1.0 / s, -1.0 / s, 2.0 / s};

	std::optional<RoofCommand> const command =
		roofCommandForPlane(plane, "R-野地板", 0.0, Vec2{0.0, 0.0});
	CHECK(command.has_value());
	if (!command.has_value())
		return;
	checkAxisSpansFootprint(failures, *command);
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
	// 軒の目標 Z ＝ 屋根版の平面（1000 ＋ ストーリ Elevation）から垂木せい（45）
	// を鉛直換算（÷cosθ＝nz）して持ち上げた値（野地板下端＝垂木上端。垂木下端＝屋根版の平面で
	// あることは実機で確認済み）。
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
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
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

		// ★**軒軸は footprint の射影範囲をはみ出さない**（M29）。屋根面オブジェクトは軸を
		// 勾配の基準線として図に描くので、余計に伸びるとそのぶん図が広がり、伏図が用紙に
		// 収まらなくなる。**実データには三角形の屋根面（4 面）も非矩形の面（7 面）もある**
		// ので、xy の外接矩形ではなく射影範囲で見る（理由は checkAxisSpansFootprint）。
		checkAxisSpansFootprint(failures, roof);
	}
}

TEST(fixture_layers_map_to_roof_storeys)
{
	// 伏図次郎: 下屋根（2FL）→ "2-野地板"、主屋根（RFL）→ "R-野地板"。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
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
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
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
	const Model& model = fixture("グレー本モデルプラン1【3階】.ifc", ok);
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
