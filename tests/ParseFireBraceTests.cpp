//
//	ParseFireBraceTests.cpp
//
//	火打解析（src/parse/FireBrace）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。**期待値は手書きで持
//	つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M11）: 火打の判別（Name 接頭辞＋IfcBeam/IfcMember）・端面の識別
//	（プロファイル局所 v の符号反転）・端面の延長交点（＝内角＝基準点）・回転角（内角の
//	二等分方向＋シンボル基準姿勢の 45 度補正）・配置先レイヤ（横架材天端／最上階は軒高）・
//	センタリング・決定性・全フィクスチャの通し。実フィクスチャのパスは CMake が
//	HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/FireBrace.h"
#include "parse/Loader.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::SymbolCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildFireBraceCommands;
using HomeskzIfcImport::parse::fireBraceAngle;
using HomeskzIfcImport::parse::fireBraceBasePoint;
using HomeskzIfcImport::parse::fireBraceEndFaces;
using HomeskzIfcImport::parse::isFireBrace;
using HomeskzIfcImport::parse::kSymbolFireBrace;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::Segment2D;
using HomeskzIfcImport::parse::segmentIntersection;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// 中心線 v=0 に対称な footprint（長辺 v=±5、端面が v をまたぐ）。ワールド座標は簡単のため
	// 局所座標と一致させる。
	const std::vector<Vec2> kLocal = {{0.0, -5.0}, {0.0, 5.0}, {10.0, 5.0}, {12.0, -5.0}};
	const std::vector<Vec2> kWorld = kLocal;

	// レイヤ名が接尾辞で終わるか。
	bool endsWith(const std::string& text, const std::string& suffix)
	{
		return text.size() >= suffix.size() &&
			   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}
} // namespace

// --- 端面の延長交点----------------------

TEST(fire_brace_perpendicular_lines_intersect)
{
	// y=1 の水平線分と x=2 の鉛直線分の交点は (2, 1)。
	const std::optional<Vec2> p =
		segmentIntersection(Segment2D{{0.0, 1.0}, {5.0, 1.0}}, Segment2D{{2.0, -3.0}, {2.0, 4.0}});
	CHECK(p.has_value());
	CHECK(near(p->x, 2.0));
	CHECK(near(p->y, 1.0));
}

TEST(fire_brace_parallel_lines_have_no_intersection)
{
	CHECK(!segmentIntersection(Segment2D{{0.0, 0.0}, {1.0, 0.0}}, Segment2D{{0.0, 1.0}, {1.0, 1.0}})
			   .has_value());
}

// --- 端面の識別と基準点-----------------

TEST(fire_brace_end_faces_are_the_v_crossing_edges)
{
	// 端面は局所 v の符号が始終点で反転する辺（P0-P1 と P2-P3）。長辺は v が一定なので
	// 選ばれない。
	const std::vector<Segment2D> faces = fireBraceEndFaces(kWorld, kLocal);
	CHECK_EQ(faces.size(), std::size_t{2});
	CHECK(near(faces[0].a.x, 0.0) && near(faces[0].a.y, -5.0));
	CHECK(near(faces[0].b.x, 0.0) && near(faces[0].b.y, 5.0));
	CHECK(near(faces[1].a.x, 10.0) && near(faces[1].a.y, 5.0));
	CHECK(near(faces[1].b.x, 12.0) && near(faces[1].b.y, -5.0));
}

TEST(fire_brace_base_point_is_end_face_intersection)
{
	// 端面 1（x=0）と端面 2（P2-P3 の延長）の交点。P2-P3 は y=5→−5 で x=10→12 なので、
	// x=0 では y = 5 + (0−10)/(12−10)·(−10) = 55。
	const std::optional<Vec2> base = fireBraceBasePoint(fireBraceEndFaces(kWorld, kLocal));
	CHECK(base.has_value());
	CHECK(near(base->x, 0.0));
	CHECK(near(base->y, 55.0));
}

TEST(fire_brace_base_point_requires_exactly_two_faces)
{
	CHECK(!fireBraceBasePoint({}).has_value());
	CHECK(!fireBraceBasePoint({Segment2D{{0.0, 0.0}, {1.0, 1.0}}}).has_value());
}

TEST(fire_brace_end_faces_need_matching_vertex_counts)
{
	// world と local の並びが食い違う（＝解決に失敗した）ときは端面を出さない。
	CHECK(fireBraceEndFaces(kWorld, {{0.0, -5.0}, {0.0, 5.0}}).empty());
}

// --- 回転角--------------------------------------------

TEST(fire_brace_angle_points_from_base_to_centroid)
{
	// 基準点 (0,0)・重心が (+1,−1) 方向＝二等分方向 −45 度。シンボル基準姿勢の補正
	// （反時計方向 45 度）を足して 0 度になる。
	const std::vector<Vec2> world = {{2.0, -2.0}, {2.0, -2.0}, {2.0, -2.0}, {2.0, -2.0}};
	CHECK(near(fireBraceAngle(Vec2{0.0, 0.0}, world), 0.0));
}

TEST(fire_brace_angle_applies_symbol_offset)
{
	// 二等分方向が 0 度（重心が +X 方向）なら補正後は 45 度。
	const std::vector<Vec2> world = {{2.0, 0.0}, {2.0, 0.0}, {2.0, 0.0}, {2.0, 0.0}};
	CHECK(near(fireBraceAngle(Vec2{0.0, 0.0}, world), 45.0));
}

// --- 火打の判別----------------------------------

TEST(fire_brace_is_matched_by_name_and_type)
{
	const Model model = loadIfcFromText("#1=IFCMEMBER('m',$,'火打:1_1',$,$,$,$,$);\n"
										"#2=IFCBEAM('b',$,'木梁:土台:1',$,$,$,$,$);\n"
										"#3=IFCCOLUMN('c',$,'火打:1_1',$,$,$,$,$);\n"
										"#4=IFCBEAM('b',$,'火打:0_1',$,$,$,$,$);\n");
	CHECK(isFireBrace(*model.entity(1)));  // IfcMember + "火打…"
	CHECK(!isFireBrace(*model.entity(2))); // 種別が違う横架材
	CHECK(!isFireBrace(*model.entity(3))); // 名前は火打でも IfcColumn は対象外
	CHECK(isFireBrace(*model.entity(4)));  // IfcBeam + "火打…"
}

TEST(fire_brace_angle_of_empty_footprint_is_zero)
{
	// 外形が空なら重心が定まらない。呼び出し側はここへ空の外形を渡さないが、
	// 0 を返して落ちないことを守る（1 本の欠損で全体を止めない）。
	CHECK(near(fireBraceAngle(Vec2{1.0, 2.0}, {}), 0.0));
}

// --- 解決できない火打はスキップする -------------------------------------------

TEST(fire_brace_without_solid_is_skipped)
{
	// 押し出しソリッドを解決できない火打（形状表現なし）は命令を出さない。ストーリと
	// 所属関係だけがあり、火打の名前は付いている状態。
	const Model model =
		loadIfcFromText("#1=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,0.);\n"
						"#2=IFCMEMBER('m',$,'火打:1_1',$,$,$,$,$);\n"
						"#3=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#2),#1);\n");
	CHECK(buildFireBraceCommands(model).empty());
}

TEST(fire_brace_without_storeys_is_empty)
{
	// FL ストーリが 1 つも無いモデルは配置先レイヤが決まらないので空。
	CHECK(buildFireBraceCommands(loadIfcFromText("#1=IFCMEMBER('m',$,'火打:1_1',$,$,$,$,$);\n"))
			  .empty());
}

// --- 実フィクスチャ-------------------------

TEST(fire_brace_fixture_count_and_shape)
{
	// 伏図次郎: 28 本（解析を変えたときに件数が動けば気付けるようにするための固定値）。
	// レイヤは横架材と同じ（一般階＝横架材天端、最上階＝軒高）。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> braces = buildFireBraceCommands(model);
	CHECK_EQ(braces.size(), std::size_t{28});

	bool sawEaves = false;
	for (const SymbolCommand& brace : braces)
	{
		CHECK_EQ(brace.symbol, std::string(kSymbolFireBrace));
		CHECK(endsWith(brace.layer, "横架材天端") || endsWith(brace.layer, "軒高"));
		sawEaves = sawEaves || endsWith(brace.layer, "軒高");
	}
	// 最上階（屋根）の火打は軒高レイヤに載る。
	CHECK(sawEaves);
}

TEST(fire_brace_fixture_positions_are_centered)
{
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> braces = buildFireBraceCommands(model);
	CHECK(!braces.empty());
	double minX = braces.front().position.x;
	double maxX = braces.front().position.x;
	double minY = braces.front().position.y;
	double maxY = braces.front().position.y;
	for (const SymbolCommand& brace : braces)
	{
		minX = std::min(minX, brace.position.x);
		maxX = std::max(maxX, brace.position.x);
		minY = std::min(minY, brace.position.y);
		maxY = std::max(maxY, brace.position.y);
	}
	CHECK(minX < 0.0 && maxX > 0.0);
	CHECK(minY < 0.0 && maxY > 0.0);
}

TEST(fire_brace_all_fixtures_build)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<SymbolCommand> braces = buildFireBraceCommands(model);
		CHECK(!braces.empty());
		for (const SymbolCommand& brace : braces)
		{
			CHECK_EQ(brace.symbol, std::string(kSymbolFireBrace));
			CHECK(endsWith(brace.layer, "横架材天端") || endsWith(brace.layer, "軒高"));
		}
	}
}

TEST(fire_brace_is_deterministic)
{
	bool ok = false;
	const Model& model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> first = buildFireBraceCommands(model);
	const std::vector<SymbolCommand> second = buildFireBraceCommands(model);
	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size(); ++i)
	{
		CHECK_EQ(first[i].layer, second[i].layer);
		CHECK(near(first[i].position.x, second[i].position.x));
		CHECK(near(first[i].position.y, second[i].position.y));
		CHECK(near(first[i].angle, second[i].angle));
	}
}

TEST_MAIN();
