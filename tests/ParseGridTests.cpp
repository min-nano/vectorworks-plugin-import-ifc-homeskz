//
//	ParseGridTests.cpp
//
//	通り芯解析（src/parse/Grid）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:
//	core/ parse/ は無 SDK で単体テスト）。Python 版 test_ifc_grid.py の意図を写す。
//
//	検証項目（ROADMAP.md M1）: ポリライン端点の取得・重複線除去・bbox 中心での
//	センタリング・X/Y 通り判定（名前優先／幾何フォールバック）・クラス付与・決定性。
//	実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Grid.h"
#include "parse/Loader.h"

#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::GridCommand;
using HomeskzIfcImport::parse::buildGridCommands;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// Python 版 ifc/grid.py の CLASS_X / CLASS_Y と一致するクラス名。
	const std::string kClassX = "01作図-01線-01基準線-01通り芯-X通り";
	const std::string kClassY = "01作図-01線-01基準線-01通り芯-Y通り";

	// label の通り芯を探す（見つからなければ nullptr）。
	const GridCommand* find(const std::vector<GridCommand>& grids, const std::string& label)
	{
		for (const GridCommand& g : grids)
			if (g.label == label)
				return &g;
		return nullptr;
	}
} // namespace

// ---------------------------------------------------------------------------
// 端点取得・X/Y 判定（名前優先）・センタリング
// ---------------------------------------------------------------------------

TEST(parses_endpoints_and_centers_by_bbox)
{
	// X1/X2 は鉛直線、Y1 は水平線。bbox は x∈[0,3640], y∈[0,4550]、中心 (1820,2275)。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,4550.,0.));\n"
										"#12=IFCCARTESIANPOINT((3640.,0.,0.));\n"
										"#13=IFCCARTESIANPOINT((3640.,4550.,0.));\n"
										"#20=IFCPOLYLINE((#10,#11));\n"
										"#21=IFCPOLYLINE((#12,#13));\n"
										"#22=IFCPOLYLINE((#10,#12));\n"
										"#30=IFCGRIDAXIS('X1',#20,.T.);\n"
										"#31=IFCGRIDAXIS('X2',#21,.T.);\n"
										"#32=IFCGRIDAXIS('Y1',#22,.T.);\n");
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(3));

	const GridCommand* x1 = find(grids, "X1");
	CHECK(x1 != nullptr);
	if (x1 != nullptr)
	{
		// (0,0)-(0,4550) を中心 (1820,2275) で寄せる → (-1820,-2275)-(-1820,2275)。
		CHECK(near(x1->start.x, -1820.0));
		CHECK(near(x1->start.y, -2275.0));
		CHECK(near(x1->end.x, -1820.0));
		CHECK(near(x1->end.y, 2275.0));
		// 既定レイヤは "共通"。
		CHECK_EQ(x1->layer, std::string("共通"));
	}

	const GridCommand* y1 = find(grids, "Y1");
	CHECK(y1 != nullptr);
	if (y1 != nullptr)
	{
		// (0,0)-(3640,0) → (-1820,-2275)-(1820,-2275)。
		CHECK(near(y1->start.x, -1820.0));
		CHECK(near(y1->start.y, -2275.0));
		CHECK(near(y1->end.x, 1820.0));
		CHECK(near(y1->end.y, -2275.0));
	}
}

TEST(classifies_x_and_y_by_name)
{
	// 名前が X/Y で始まればその通り（大文字小文字は無視）。X1/x2 → X、Y1 → Y。
	// x2 はあえて水平線に載せ、判定が向きでなく「名前優先」であることを示す。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,1000.,0.));\n"
										"#12=IFCCARTESIANPOINT((1000.,0.,0.));\n"
										"#13=IFCCARTESIANPOINT((1000.,1000.,0.));\n"
										"#20=IFCPOLYLINE((#10,#11));\n" // 鉛直
										"#21=IFCPOLYLINE((#10,#12));\n" // 水平
										"#22=IFCPOLYLINE((#11,#13));\n" // 水平（x2 用）
										"#30=IFCGRIDAXIS('X1',#20,.T.);\n"
										"#31=IFCGRIDAXIS('x2',#22,.T.);\n"
										"#32=IFCGRIDAXIS('Y1',#21,.T.);\n");
	std::vector<GridCommand> const grids = buildGridCommands(model);

	const GridCommand* x1 = find(grids, "X1");
	const GridCommand* x2 = find(grids, "x2");
	const GridCommand* y1 = find(grids, "Y1");
	CHECK(x1 != nullptr && x2 != nullptr && y1 != nullptr);
	if (x1 != nullptr)
		CHECK_EQ(x1->drawClass, kClassX);
	if (x2 != nullptr)
		CHECK_EQ(x2->drawClass, kClassX);
	if (y1 != nullptr)
		CHECK_EQ(y1->drawClass, kClassY);
}

TEST(classifies_by_geometry_when_name_absent)
{
	// 軸名が無い（$）ときは向きで判定: 鉛直線(|Δx|<|Δy|)→X、水平線→Y。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,1000.,0.));\n"
										"#12=IFCCARTESIANPOINT((1000.,0.,0.));\n"
										"#20=IFCPOLYLINE((#10,#11));\n" // 鉛直
										"#21=IFCPOLYLINE((#10,#12));\n" // 水平
										"#30=IFCGRIDAXIS($,#20,.T.);\n"
										"#31=IFCGRIDAXIS($,#21,.T.);\n");
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(2));
	// 名前が無いので label は空。向きでクラスが決まる（鉛直=X、水平=Y）。
	bool sawX = false;
	bool sawY = false;
	for (const GridCommand& g : grids)
	{
		CHECK(g.label.empty());
		if (g.drawClass == kClassX)
			sawX = true;
		if (g.drawClass == kClassY)
			sawY = true;
	}
	CHECK(sawX);
	CHECK(sawY);
}

// ---------------------------------------------------------------------------
// 重複線除去
// ---------------------------------------------------------------------------

TEST(removes_duplicate_lines_including_reversed)
{
	// 同一線分（向き反転を含む）は 1 本に畳む。最初に現れた軸を残す（決定的）。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,1000.,0.));\n"
										"#20=IFCPOLYLINE((#10,#11));\n"
										"#21=IFCPOLYLINE((#11,#10));\n" // 反転（同一線分）
										"#30=IFCGRIDAXIS('X1',#20,.T.);\n"
										"#31=IFCGRIDAXIS('X1dup',#21,.T.);\n");
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(1));
	CHECK(find(grids, "X1") != nullptr);	// 最初の軸が残る
	CHECK(find(grids, "X1dup") == nullptr); // 反転重複は除去
}

// ---------------------------------------------------------------------------
// 欠損・空
// ---------------------------------------------------------------------------

TEST(skips_axes_with_unresolvable_or_short_curve)
{
	// 曲線が解決できない軸・点が 1 つの軸はスキップし、健全な 1 本だけ返す。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,1000.,0.));\n"
										"#12=IFCCARTESIANPOINT((5.,5.,0.));\n"
										"#20=IFCPOLYLINE((#10,#11));\n"
										"#21=IFCPOLYLINE((#12));\n" // 点 1 つ（不足）
										"#30=IFCGRIDAXIS('X1',#20,.T.);\n"
										"#31=IFCGRIDAXIS('X2',#21,.T.);\n"
										"#32=IFCGRIDAXIS('X3',#999,.T.);\n"); // 未解決参照
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(1));
	CHECK(find(grids, "X1") != nullptr);
}

TEST(skips_axes_with_bad_points)
{
	// cartesianPoint の失敗 2 系統をスキップさせる:
	//   (a) ポリラインの点参照が未解決（#900 が存在しない）→ 始点解決失敗。
	//   (b) 座標が 1 つしかない点（#12）→ 座標不足で解決失敗。
	// どちらの軸も落とし、健全な X1 の 1 本だけ返す（1 軸の欠損で全体を止めない）。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,1000.,0.));\n"
										"#12=IFCCARTESIANPOINT((5.));\n" // 座標 1 つ（不足）
										"#20=IFCPOLYLINE((#10,#11));\n"	 // 健全
										"#21=IFCPOLYLINE((#900,#901));\n" // 点参照が未解決
										"#22=IFCPOLYLINE((#12,#10));\n" // 始点の座標が不足
										"#30=IFCGRIDAXIS('X1',#20,.T.);\n"
										"#31=IFCGRIDAXIS('X2',#21,.T.);\n"
										"#32=IFCGRIDAXIS('X3',#22,.T.);\n");
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(1));
	CHECK(find(grids, "X1") != nullptr);
}

TEST(skips_non_polyline_curve)
{
	// AxisCurve がポリラインでない軸はスキップする（Python の is_a('IfcPolyline') 判定）。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,1000.,0.));\n"
										"#20=IFCPOLYLINE((#10,#11));\n"
										"#21=IFCCIRCLE(#10,500.);\n" // ポリラインでない曲線
										"#30=IFCGRIDAXIS('X1',#20,.T.);\n"
										"#31=IFCGRIDAXIS('C1',#21,.T.);\n");
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(1));
	CHECK(find(grids, "X1") != nullptr);
	CHECK(find(grids, "C1") == nullptr);
}

// ---------------------------------------------------------------------------
// 多点ポリライン（区間ごとに 1 本）
// ---------------------------------------------------------------------------

TEST(emits_one_line_per_polyline_segment)
{
	// 3 点のポリラインは連続する点対ごとに 2 本の線分になる（Python resolve_lines の
	// `for i in range(len(pts) - 1)`）。(0,0)-(0,1000)-(0,2000) は同名 X1 の 2 区間。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCCARTESIANPOINT((0.,1000.,0.));\n"
										"#12=IFCCARTESIANPOINT((0.,2000.,0.));\n"
										"#20=IFCPOLYLINE((#10,#11,#12));\n"
										"#30=IFCGRIDAXIS('X1',#20,.T.);\n");
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(2));
	for (const GridCommand& g : grids)
	{
		CHECK_EQ(g.label, std::string("X1"));
		CHECK_EQ(g.drawClass, kClassX);
	}
}

TEST(empty_model_yields_no_grids)
{
	Model const model =
		loadIfcFromText("#1=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,0.);\n");
	CHECK(buildGridCommands(model).empty());
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(reads_minimal_grid_fixture)
{
	// minimal_grid.ifc は X1/X2（鉛直）と Y1（水平）の 3 本（ParseSummaryTests と同じ内訳）。
	bool ok = false;
	Model const model = fixture("minimal_grid.ifc", ok);
	CHECK(ok);
	std::vector<GridCommand> const grids = buildGridCommands(model);

	CHECK_EQ(grids.size(), static_cast<std::size_t>(3));
	const GridCommand* x1 = find(grids, "X1");
	const GridCommand* y1 = find(grids, "Y1");
	CHECK(x1 != nullptr);
	CHECK(y1 != nullptr);
	if (x1 != nullptr)
		CHECK_EQ(x1->drawClass, kClassX);
	if (y1 != nullptr)
		CHECK_EQ(y1->drawClass, kClassY);

	// bbox 中心が原点へ来る（全端点の min+max が各軸で 0 になる）。
	double minX = 1e18;
	double minY = 1e18;
	double maxX = -1e18;
	double maxY = -1e18;
	for (const GridCommand& g : grids)
	{
		for (const auto& p : {g.start, g.end})
		{
			minX = std::min(minX, p.x);
			maxX = std::max(maxX, p.x);
			minY = std::min(minY, p.y);
			maxY = std::max(maxY, p.y);
		}
	}
	CHECK(near(minX + maxX, 0.0));
	CHECK(near(minY + maxY, 0.0));
}

TEST(reads_real_homeskz_fixture)
{
	// ホームズ君の実モデル。多数の x*/y* 軸を含み、解析が例外なく通り、命令が出る。
	bool ok = false;
	Model const model = fixture("グレー本モデルプラン1【3階】.ifc", ok);
	CHECK(ok);
	std::vector<GridCommand> const grids = buildGridCommands(model);

	// x1..x12 と y1..y* の多数の通り芯が検出される（重複除去後）。
	CHECK(grids.size() > 10);
	// x で始まる軸は X クラス、y で始まる軸は Y クラスに分かれる。
	bool sawX = false;
	bool sawY = false;
	for (const GridCommand& g : grids)
	{
		if (g.drawClass == kClassX)
			sawX = true;
		if (g.drawClass == kClassY)
			sawY = true;
	}
	CHECK(sawX);
	CHECK(sawY);
}

TEST_MAIN();
