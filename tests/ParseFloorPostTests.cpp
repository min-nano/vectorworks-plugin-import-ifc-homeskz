//
//	ParseFloorPostTests.cpp
//
//	床束解析（src/parse/FloorPost）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	Python 版 test_ifc_floor_post.py のケースを 1 対 1 で写している（期待値は手書き。
//	ROADMAP.md「Python 版出力との比較はしない」）。
//
//	検証項目（ROADMAP.md M11）: 910mm 間隔の割り付け（端点には置かない）・支持材芯の探索
//	（半支持材厚以内・区間内・平行は除外）・同一直線上の継手統合（すき間 ≤ 半モジュール）・
//	支持材に土台だけでなく大引も含めること・基礎が無いモデルは空・配置先レイヤ（F-床束）・
//	センタリング・決定性。実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/FloorPost.h"
#include "parse/Footing.h"
#include "parse/Loader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::SymbolCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildFloorPostCommands;
using HomeskzIfcImport::parse::collectOhbikiLines;
using HomeskzIfcImport::parse::collectSupportLines;
using HomeskzIfcImport::parse::collinearGap;
using HomeskzIfcImport::parse::floorPostOffsets;
using HomeskzIfcImport::parse::hasFoundation;
using HomeskzIfcImport::parse::kLayerFoundationFloorPost;
using HomeskzIfcImport::parse::kSymbolFloorPost;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::mergeCollinearOhbiki;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::OhbikiRun;
using HomeskzIfcImport::parse::shinReference;
using HomeskzIfcImport::parse::SupportLine;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// 幅 105mm の支持材（土台または他の大引）を x=0 の位置に Y 方向へ通す
	// （芯線 x=0、区間 y ∈ [−1000, 1000]）。Python 版 TestShinReference.SUPPORT と同じ。
	const std::vector<SupportLine> kSupport = {
		SupportLine{Vec2{0.0, -1000.0}, Vec2{0.0, 1.0}, 2000.0, 105.0}};

	// 命令からの位置一致判定（許容付き）。
	bool hasPost(const std::vector<SymbolCommand>& posts, double x, double y)
	{
		for (const SymbolCommand& post : posts)
		{
			if (near(post.position.x, x, 1e-6) && near(post.position.y, y, 1e-6))
				return true;
		}
		return false;
	}
} // namespace

// --- 配置間隔（Python 版 TestPostOffsets）------------------------------------

TEST(floor_post_single_module_gets_no_post)
{
	// 910mm 以下（単モジュールの大引＝805mm）は床束 0 本（両端が支持材に受けられる）。
	CHECK(floorPostOffsets(805.0).empty());
}

TEST(floor_post_exactly_interval_gets_no_post)
{
	// ちょうど 910mm は終点（＝支持材芯）に来るため 0 本（端点には置かない）。
	CHECK(floorPostOffsets(910.0).empty());
}

TEST(floor_post_one_post_from_end)
{
	CHECK_EQ(floorPostOffsets(1715.0).size(), std::size_t{1});
	CHECK(near(floorPostOffsets(1715.0).front(), 910.0));
	CHECK_EQ(floorPostOffsets(1820.0).size(), std::size_t{1});
}

TEST(floor_post_keeps_910_pitch_from_start)
{
	// 始点から 910mm ずつ並び、終点側は 910mm 未満の半端でよい（等分ではなく端部起点）。
	const std::vector<double> offsets = floorPostOffsets(3640.0);
	CHECK_EQ(offsets.size(), std::size_t{3});
	CHECK(near(offsets[0], 910.0));
	CHECK(near(offsets[1], 1820.0));
	CHECK(near(offsets[2], 2730.0));

	const std::vector<double> partial = floorPostOffsets(2625.0);
	CHECK_EQ(partial.size(), std::size_t{2});
	CHECK(2625.0 - partial.back() > 0.0);
	CHECK(2625.0 - partial.back() < 910.0);
}

TEST(floor_post_zero_length_returns_empty)
{
	CHECK(floorPostOffsets(0.0).empty());
	CHECK(floorPostOffsets(-100.0).empty());
}

// --- 支持材芯の探索（Python 版 TestShinReference）----------------------------

TEST(floor_post_end_inset_from_support_returns_shin)
{
	// 大引端が支持材芯より半支持材厚（52.5mm）内側にある（x=52.5）→ 支持材芯 x=0 を返す。
	const std::optional<Vec2> ref = shinReference(Vec2{52.5, 300.0}, Vec2{1.0, 0.0}, kSupport);
	CHECK(ref.has_value());
	CHECK(near(ref->x, 0.0));
	CHECK(near(ref->y, 300.0));
}

TEST(floor_post_end_flush_with_shin_returns_same_point)
{
	const std::optional<Vec2> ref = shinReference(Vec2{0.0, 100.0}, Vec2{1.0, 0.0}, kSupport);
	CHECK(ref.has_value());
	CHECK(near(ref->x, 0.0));
}

TEST(floor_post_end_far_from_support_returns_none)
{
	// 端が支持材の footprint（半支持材厚）より遠い（x=200）→ 受けていない。
	CHECK(!shinReference(Vec2{200.0, 300.0}, Vec2{1.0, 0.0}, kSupport).has_value());
}

TEST(floor_post_parallel_support_returns_none)
{
	// 大引と平行な支持材（自身の芯線・同一直線上の大引を含む）は交点が定まらない。
	CHECK(!shinReference(Vec2{52.5, 300.0}, Vec2{0.0, 1.0}, kSupport).has_value());
}

TEST(floor_post_outside_support_segment_returns_none)
{
	// 交点が支持材の区間外（y=5000 は区間 [−1000, 1000] の外）→ 受けていない。
	CHECK(!shinReference(Vec2{52.5, 5000.0}, Vec2{1.0, 0.0}, kSupport).has_value());
}

// --- 継手の統合（Python 版 TestMergeCollinear）-------------------------------

TEST(floor_post_gap_none_when_not_parallel)
{
	const OhbikiRun a{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}};
	const OhbikiRun b{Vec2{500.0, 0.0}, Vec2{500.0, 1000.0}};
	CHECK(!collinearGap(a, b).has_value());
}

TEST(floor_post_gap_none_when_offset_line)
{
	// 平行だが別の直線上（直交距離あり）はすき間なし。
	const OhbikiRun a{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}};
	const OhbikiRun b{Vec2{1200.0, 50.0}, Vec2{2000.0, 50.0}};
	CHECK(!collinearGap(a, b).has_value());
}

TEST(floor_post_gap_between_collinear_segments)
{
	// 同一直線上・105mm すき間（継手＝支持材幅ぶんの継目）。
	const OhbikiRun a{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}};
	const OhbikiRun b{Vec2{1105.0, 0.0}, Vec2{2000.0, 0.0}};
	const std::optional<double> gap = collinearGap(a, b);
	CHECK(gap.has_value());
	CHECK(near(*gap, 105.0));
}

TEST(floor_post_gap_zero_when_touching)
{
	const OhbikiRun a{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}};
	const OhbikiRun b{Vec2{1000.0, 0.0}, Vec2{2000.0, 0.0}};
	const std::optional<double> gap = collinearGap(a, b);
	CHECK(gap.has_value());
	CHECK(near(*gap, 0.0));
}

TEST(floor_post_gap_none_for_degenerate_run)
{
	// 長さ 0 の区間は方向が定まらないので同一直線判定が成り立たない。
	const OhbikiRun degenerate{Vec2{0.0, 0.0}, Vec2{0.0, 0.0}};
	const OhbikiRun normal{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}};
	CHECK(!collinearGap(degenerate, normal).has_value());
	CHECK(!collinearGap(normal, degenerate).has_value());
}

TEST(floor_post_merge_of_empty_input_is_empty)
{
	CHECK(mergeCollinearOhbiki({}).empty());
}

TEST(floor_post_members_without_geometry_are_skipped)
{
	// 配置・断面を解決できない大引／土台は支持材にも大引にも数えない（基礎はあるが
	// 幾何が無いので床束は 0 本）。
	const Model model = loadIfcFromText("#1=IFCSLAB('s',$,'基礎底盤',$,$,$,$,$,$);\n"
										"#2=IFCBEAM('b',$,'木梁:大引:1',$,$,$,$,$);\n"
										"#3=IFCBEAM('b',$,'木梁:土台:1',$,$,$,$,$);\n");
	CHECK(hasFoundation(model));
	CHECK(collectOhbikiLines(model).empty());
	CHECK(collectSupportLines(model).empty());
	CHECK(buildFloorPostCommands(model).empty());
}

TEST(floor_post_joint_merged_into_one_run)
{
	// 継手（105mm すき間）で分断された 3 本は 1 連（0〜4105）に統合される。
	const std::vector<OhbikiRun> lines = {
		{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}},
		{Vec2{1105.0, 0.0}, Vec2{2000.0, 0.0}},
		{Vec2{2105.0, 0.0}, Vec2{4105.0, 0.0}},
	};
	const std::vector<OhbikiRun> runs = mergeCollinearOhbiki(lines);
	CHECK_EQ(runs.size(), std::size_t{1});
	CHECK(near(runs.front().start.x, 0.0));
	CHECK(near(runs.front().end.x, 4105.0));
	CHECK(near(runs.front().start.y, 0.0));
	CHECK(near(runs.front().end.y, 0.0));
}

TEST(floor_post_distant_collinear_not_merged)
{
	// 同一直線上でも 1 モジュール以上（> 半モジュール 455mm）離れた大引は別材。
	const std::vector<OhbikiRun> lines = {
		{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}},
		{Vec2{2000.0, 0.0}, Vec2{3000.0, 0.0}},
	};
	CHECK_EQ(mergeCollinearOhbiki(lines).size(), std::size_t{2});
}

TEST(floor_post_perpendicular_not_merged)
{
	const std::vector<OhbikiRun> lines = {
		{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}},
		{Vec2{500.0, 0.0}, Vec2{500.0, 1000.0}},
	};
	CHECK_EQ(mergeCollinearOhbiki(lines).size(), std::size_t{2});
}

TEST(floor_post_merge_is_order_independent)
{
	// 入力順を変えても同じ 1 連にまとまる（代表は最小インデックス、出力は昇順）。
	const std::vector<OhbikiRun> forward = {
		{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}},
		{Vec2{1105.0, 0.0}, Vec2{2000.0, 0.0}},
	};
	const std::vector<OhbikiRun> reversed = {
		{Vec2{1105.0, 0.0}, Vec2{2000.0, 0.0}},
		{Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}},
	};
	const std::vector<OhbikiRun> a = mergeCollinearOhbiki(forward);
	const std::vector<OhbikiRun> b = mergeCollinearOhbiki(reversed);
	CHECK_EQ(a.size(), std::size_t{1});
	CHECK_EQ(b.size(), std::size_t{1});
	// 区間そのもの（[最小, 最大]）は同じ。向きは先頭材の向きに従うので、両端の集合で比べる。
	CHECK(near(std::min(a.front().start.x, a.front().end.x),
			   std::min(b.front().start.x, b.front().end.x)));
	CHECK(near(std::max(a.front().start.x, a.front().end.x),
			   std::max(b.front().start.x, b.front().end.x)));
}

// --- 基礎が無いモデル（Python 版 test_no_foundation_returns_empty）-----------

TEST(floor_post_without_foundation_is_empty)
{
	// 基礎が無いモデルでは配置先レイヤ（F-床束）が生成されないため空。大引があっても出さない。
	const Model model = loadIfcFromText("#1=IFCBEAM('b',$,'木梁:大引:1',$,$,$,$,$);\n");
	CHECK(!hasFoundation(model));
	CHECK(buildFloorPostCommands(model).empty());
}

TEST(floor_post_foundation_detected_from_base_slab)
{
	// 底盤の IfcSlab があれば基礎あり（Python 版 _iter_footing_elements / has_foundation）。
	CHECK(hasFoundation(loadIfcFromText("#1=IFCSLAB('s',$,'基礎底盤',$,$,$,$,$,$);\n")));
	// 立上り（基礎梁…）の IfcFooting でも基礎あり。
	CHECK(hasFoundation(loadIfcFromText("#1=IFCFOOTING('f',$,'基礎梁:1',$,$,$,$,$,$);\n")));
	// 床版・屋根版の IfcSlab は基礎ではない。
	CHECK(!hasFoundation(loadIfcFromText("#1=IFCSLAB('s',$,'床版',$,$,$,$,$,$);\n")));
}

// --- 実フィクスチャ（Python 版 TestBuildFromFixture）-------------------------

TEST(floor_post_fixture_shape)
{
	bool ok = false;
	const Model model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> posts = buildFloorPostCommands(model);
	CHECK(!posts.empty());
	for (const SymbolCommand& post : posts)
	{
		CHECK_EQ(post.layer, std::string(kLayerFoundationFloorPost));
		CHECK_EQ(post.symbol, std::string(kSymbolFloorPost));
		// 床束は軸対称なので回転角を持たない。
		CHECK(near(post.angle, 0.0));
	}
}

TEST(floor_post_support_lines_include_ohbiki)
{
	// 支持材芯には土台だけでなく大引も含める（二次大引の端を大引芯基準にするため）。
	bool ok = false;
	const Model model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<SupportLine> supports = collectSupportLines(model);
	const std::vector<OhbikiRun> ohbiki = collectOhbikiLines(model);
	CHECK(!ohbiki.empty());
	// 大引を含めるぶん、支持材の数は大引の数より多い（土台も入る）。
	CHECK(supports.size() > ohbiki.size());
}

TEST(floor_post_collinear_ohbiki_are_merged_in_fixture)
{
	// 継手で分断された大引が統合され、連の数は元の大引本数より少なくなる。
	bool ok = false;
	const Model model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<OhbikiRun> lines = collectOhbikiLines(model);
	CHECK(!lines.empty());
	CHECK(mergeCollinearOhbiki(lines).size() < lines.size());
}

TEST(floor_post_count_matches_merged_run_shin_spans)
{
	// 床束の総数は「継手統合後の大引 1 連の支持材芯どうしの区間」に floorPostOffsets を
	// 適用した合計と一致する（継手は端部として扱わず、支持材芯を端部にする）。
	bool ok = false;
	const Model model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<SupportLine> supports = collectSupportLines(model);
	const std::vector<OhbikiRun> runs = mergeCollinearOhbiki(collectOhbikiLines(model));
	std::size_t expected = 0;
	for (const OhbikiRun& run : runs)
	{
		const Vec2 delta = run.end - run.start;
		const double length = std::hypot(delta.x, delta.y);
		if (length <= 0.0)
			continue;
		const Vec2 direction{delta.x / length, delta.y / length};
		const Vec2 start = shinReference(run.start, direction, supports).value_or(run.start);
		const Vec2 end = shinReference(run.end, direction, supports).value_or(run.end);
		const Vec2 span = end - start;
		const double spanLength = (span.x * direction.x) + (span.y * direction.y);
		if (spanLength <= 0.0)
			continue;
		expected += floorPostOffsets(spanLength).size();
	}

	CHECK(!runs.empty());
	CHECK(expected > 0);
	CHECK_EQ(buildFloorPostCommands(model).size(), expected);
}

TEST(floor_post_synthetic_run_places_posts_at_shin_pitch)
{
	// 土台（幅 105mm）を x=0 と x=3640 に通し、その間へ大引を 1 本渡す。ホームズ君 IFC と
	// 同じく大引の端は支持材芯より半支持材厚（52.5mm）内側に納まっている。支持材芯どうしの
	// 区間 3640mm には 910/1820/2730 の 3 本が入る（端＝支持材芯には置かない）。
	std::string text;
	int next = 1;
	const auto add = [&text, &next](const std::string& body)
	{
		const int id = next++;
		text += "#" + std::to_string(id) + "=" + body + ";\n";
		return id;
	};
	// 大引・土台を「配置点＋押し出し軸＋矩形断面」で作る（parse/Member の読み方に合わせる）。
	const auto makeBeam = [&add](const std::string& name, double ox, double oy, double ax,
								 double ay, double width, double height, double length)
	{
		const int location =
			add("IFCCARTESIANPOINT((" + std::to_string(ox) + "," + std::to_string(oy) + ",0.))");
		const int axis =
			add("IFCDIRECTION((" + std::to_string(ax) + "," + std::to_string(ay) + ",0.))");
		const int placement = add("IFCAXIS2PLACEMENT3D(#" + std::to_string(location) + ",#" +
								  std::to_string(axis) + ",$)");
		const int localPlacement = add("IFCLOCALPLACEMENT($,#" + std::to_string(placement) + ")");
		const int profile = add("IFCRECTANGLEPROFILEDEF(.AREA.,$,$," + std::to_string(width) + "," +
								std::to_string(height) + ")");
		const int extrudeDir = add("IFCDIRECTION((0.,0.,1.))");
		const int solid = add("IFCEXTRUDEDAREASOLID(#" + std::to_string(profile) + ",$,#" +
							  std::to_string(extrudeDir) + "," + std::to_string(length) + ")");
		const int shape =
			add("IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#" + std::to_string(solid) + "))");
		const int product = add("IFCPRODUCTDEFINITIONSHAPE($,$,(#" + std::to_string(shape) + "))");
		add("IFCBEAM('b',$,'" + name + "',$,$,#" + std::to_string(localPlacement) + ",#" +
			std::to_string(product) + ",$)");
	};

	// 基礎（底盤）が無いと床束は出ないので 1 枚置く。
	add("IFCSLAB('s',$,'基礎底盤',$,$,$,$,$,$)");
	// 土台 2 本（Y 方向・幅 105mm）。芯線 x=0 と x=3640。
	makeBeam("木梁:土台:1", 0.0, -1000.0, 0.0, 1.0, 105.0, 105.0, 2000.0);
	makeBeam("木梁:土台:2", 3640.0, -1000.0, 0.0, 1.0, 105.0, 105.0, 2000.0);
	// 大引 1 本（X 方向）。端は支持材芯より 52.5mm 内側＝実長 3640 − 105 = 3535mm。
	makeBeam("木梁:大引:1", 52.5, 0.0, 1.0, 0.0, 105.0, 105.0, 3535.0);

	const std::vector<SymbolCommand> posts = buildFloorPostCommands(loadIfcFromText(text));
	CHECK_EQ(posts.size(), std::size_t{3});
	// 通り芯が無いモデルなのでセンタリング補正は掛からない。
	CHECK(hasPost(posts, 910.0, 0.0));
	CHECK(hasPost(posts, 1820.0, 0.0));
	CHECK(hasPost(posts, 2730.0, 0.0));
}

TEST(floor_post_is_deterministic)
{
	bool ok = false;
	const Model model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> first = buildFloorPostCommands(model);
	const std::vector<SymbolCommand> second = buildFloorPostCommands(model);
	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size(); ++i)
	{
		CHECK(near(first[i].position.x, second[i].position.x));
		CHECK(near(first[i].position.y, second[i].position.y));
	}
}

TEST_MAIN();
