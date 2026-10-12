//
//	ParsePlanLevelTests.cpp
//
//	伏図レベル（src/parse/PlanLevel。横架材の高さごとに伏図を作る）の単体テスト。VectorWorks
//	SDK を一切 include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md
//	「テスト方針」）。**期待値は手書きで持つ**。
//
//	検証項目（docs/DEV-NOTES.md「横架材の高さごとに伏図を作る」）: 高さの集め方（1mm でも
//	違えば別・傾いた材は数えない）・まとめる設定（同じ階の中だけ）・標準の伏図レベル（名前を
//	変えない）と印・近い／届く伏図レベルの引き方・横架材の振り分け（傾いた材は水下側）・
//	柱の span の番号（伏図レベルの通し番号）・タイトルに添える高さ・設定ダイアログの候補・
//	実フィクスチャ（スキップフロアだけが高さを複数持つ）。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "parse/BuildDocument.h"
#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/Loader.h"
#include "parse/PlanLevel.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::ImportOptions;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::PlanLevelKey;
using HomeskzIfcImport::parse::assignMemberPlanLevels;
using HomeskzIfcImport::parse::buildPlanLevels;
using HomeskzIfcImport::parse::collectBeamHeights;
using HomeskzIfcImport::parse::Context;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::nearestPlanLevel;
using HomeskzIfcImport::parse::noboribariSpan;
using HomeskzIfcImport::parse::PlanLevel;
using HomeskzIfcImport::parse::planLevelAbove;
using HomeskzIfcImport::parse::planLevelLayer;
using HomeskzIfcImport::parse::planLevelShift;
using HomeskzIfcImport::parse::planLevelTitleSuffix;
using HomeskzIfcImport::parse::StoryInfo;
using HomeskzIfcImport::parse::storyOfOrdinal;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::forEachFixture;
using HomeskzIfcTests::near;

namespace
{
	// 2 階建て相当（1FL=600・2FL=3500・RFL=6300。横架材天端は 1FL=590・2FL=3490）。
	std::vector<StoryInfo> twoStories()
	{
		StoryInfo first;
		first.id = 1;
		first.elevation = 600.0;
		first.beamOffset = -10.0;
		StoryInfo second;
		second.id = 2;
		second.elevation = 3500.0;
		second.beamOffset = -10.0;
		StoryInfo roof;
		roof.id = 3;
		roof.elevation = 6300.0;
		roof.isTop = true;
		return {first, second, roof};
	}

	MemberCommand beam(const std::string& layer, double startZ, double endZ)
	{
		MemberCommand member;
		member.layer = layer;
		member.start = {0.0, 0.0};
		member.end = {1000.0, 0.0};
		member.width = 105.0;
		member.height = 150.0;
		member.elevation = startZ;
		member.endElevation = endZ;
		return member;
	}
} // namespace

// ---------------------------------------------------------------------------
// core の印（core::planLevelTag / stripPlanLevelTag）
// ---------------------------------------------------------------------------

TEST(plan_level_tag_is_the_step_from_the_standard_level)
{
	// 印は高さの数値ではなく標準からの段数（ご要望。高さを変えても名前が食い違わない）。
	CHECK_EQ(core::planLevelTag(-1), std::string("(-1)"));
	CHECK_EQ(core::planLevelTag(2), std::string("(+2)"));
	CHECK_EQ(core::planLevelTag(0), std::string("(±0)"));
	CHECK_EQ(core::planLevelStepText(0.5), std::string("+0.5"));
	CHECK_EQ(core::planLevelStepText(-1.5), std::string("-1.5"));
	// 高さの表記（伏図のタイトル・設定ダイアログ）は FL（最上階は軒高）から測る。
	CHECK_EQ(core::planLevelHeightText(5542, 6374, true), std::string("軒高-832"));
	CHECK_EQ(core::planLevelHeightText(2699, 3571, false), std::string("FL-872"));
}

TEST(parse_plan_level_step_text_round_trips)
{
	double step = 9.0;
	CHECK(core::parsePlanLevelStepText("±0", step) && step == 0.0);
	CHECK(core::parsePlanLevelStepText("-1", step) && step == -1.0);
	CHECK(core::parsePlanLevelStepText("+12", step) && step == 12.0);
	CHECK(core::parsePlanLevelStepText("+0.5", step) && step == 0.5);
	CHECK(!core::parsePlanLevelStepText("", step));
	CHECK(!core::parsePlanLevelStepText("1", step));
	CHECK(!core::parsePlanLevelStepText("+0", step));
	CHECK(!core::parsePlanLevelStepText("-0.0", step));
	CHECK(!core::parsePlanLevelStepText("+1.25", step));
	CHECK(!core::parsePlanLevelStepText("+1e3", step));
	CHECK(!core::parsePlanLevelStepText("+.5", step));
}

TEST(strip_plan_level_tag_restores_the_base_name)
{
	CHECK_EQ(core::stripPlanLevelTag("横架材天端(-1)"), std::string("横架材天端"));
	CHECK_EQ(core::stripPlanLevelTag("軒高(+2)"), std::string("軒高"));
	CHECK_EQ(core::stripPlanLevelTag("FL(-1)"), std::string("FL"));
	// 印の無い名前・印に見えない括弧はそのまま。
	CHECK_EQ(core::stripPlanLevelTag("横架材天端"), std::string("横架材天端"));
	CHECK_EQ(core::stripPlanLevelTag("柱(通し)"), std::string("柱(通し)"));
	CHECK_EQ(core::stripPlanLevelTag("柱(FL)"), std::string("柱(FL)"));
	CHECK_EQ(core::stripPlanLevelTag("FL(FL-872)"), std::string("FL(FL-872)"));
	CHECK_EQ(core::stripPlanLevelTag(""), std::string());
}

TEST(layer_order_treats_tagged_levels_like_their_base_type)
{
	// 床は背面・耐力壁は前面へ回す決まり（core::desiredStoryLayerOrder）が、伏図レベルの
	// 印の付いたレベルにも適用される。
	core::StoryCommand story;
	story.name = "2階";
	story.suffix = "2";
	story.levels = {core::LevelCommand{"FL", 0.0, "2±0-FL"},
					core::LevelCommand{"FL(-1)", -832.0, "2-1-FL"},
					core::LevelCommand{"耐力壁(-1)", -872.0, "2-1-耐力壁"},
					core::LevelCommand{"横架材天端", -40.0, "2±0-横架材"}};
	const std::vector<std::string> order = core::desiredStoryLayerOrder({story}, {});
	CHECK_EQ(order.size(), std::size_t(5));
	if (order.size() != 5)
		return;
	CHECK_EQ(order[0], std::string(core::kGridLayer));
	CHECK_EQ(order[1], std::string("2-1-耐力壁"));
	CHECK_EQ(order[2], std::string("2±0-横架材"));
	CHECK_EQ(order[3], std::string("2±0-FL"));
	CHECK_EQ(order[4], std::string("2-1-FL"));
}

// ---------------------------------------------------------------------------
// 高さの集め方
// ---------------------------------------------------------------------------

TEST(beam_heights_are_distinct_to_the_millimetre)
{
	// **1mm でも違えば別のレベル**（まとめるかどうかは設計者が決める。ご要望）。0.4mm の
	// 違いは丸めて同じ高さ。傾いた材・母屋レイヤの材は数えない。
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<MemberCommand> members = {
		beam("2±0-横架材", 3490.0, 3490.0), beam("2±0-横架材", 3490.4, 3490.4),
		beam("2±0-横架材", 3489.0, 3489.0), beam("2±0-横架材", 2700.0, 2700.0),
		beam("2±0-横架材", 3000.0, 3400.0), beam("2-母屋", 3900.0, 3900.0),
		beam("1±0-横架材", 590.0, 590.0)};
	const std::vector<std::vector<long long>> heights = collectBeamHeights(stories, members);
	CHECK_EQ(heights.size(), std::size_t(3));
	CHECK(heights[0] == std::vector<long long>{590});
	CHECK((heights[1] == std::vector<long long>{2700, 3489, 3490}));
	// 水平な横架材の無い階は標準の天端（最上階は軒高）1 つ。
	CHECK(heights[2] == std::vector<long long>{6300});
}

// ---------------------------------------------------------------------------
// 伏図レベルの組み立て
// ---------------------------------------------------------------------------

TEST(plan_levels_number_every_height_and_mark_the_standard)
{
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	CHECK_EQ(levels.size(), std::size_t(4));
	if (levels.size() != 4)
		return;
	// 通し番号は下から振り、各階のあとに母屋のレベルの番号を 1 つ空ける（1 / (2) / 3, 4 /
	// (5) / 6 / (7)。柱の span の番号になる）。
	const std::vector<int> ordinals = {1, 3, 4, 6};
	for (std::size_t k = 0; k < levels.size(); ++k)
		CHECK_EQ(levels[k].ordinal, ordinals[k]);
	CHECK(near(parse::moyaOrdinal(levels, 1), 5.0));
	CHECK(near(parse::moyaOrdinal(levels, 2), 7.0));
	CHECK_EQ(parse::storyOfOrdinal(levels, 5.0), std::size_t(1)); // 母屋のレベルはその階
	CHECK_EQ(parse::storyOfOrdinal(levels, 0.5), std::size_t(0)); // いちばん下より下
	// 標準の天端（2FL は 3490）を含むものは段数 0・印なし、ほかは標準からの段数の印。
	CHECK(levels[1].story == 1 && !levels[1].standard);
	CHECK_EQ(levels[1].step, -1);
	CHECK_EQ(levels[1].tag, std::string("(-1)"));
	CHECK(levels[2].standard && levels[2].tag.empty() && levels[2].step == 0);
	CHECK(levels[0].standard && levels[3].standard);
	// レイヤ名は階の接頭辞＋段数の符号（"2-1-横架材" / "2±0-FL"）。
	CHECK_EQ(parse::planLevelBeamLayer(levels[1], stories[1]), std::string("2-1-横架材"));
	CHECK_EQ(parse::planLevelLayer(levels[2], stories[1], "FL"), std::string("2±0-FL"));
	CHECK_EQ(parse::planLevelBeamLayer(levels[3], stories[2]), std::string("R±0-横架材"));
	CHECK_EQ(parse::planLevelType(levels[1], "横架材天端"), std::string("横架材天端(-1)"));
	// レベルのずらし量は代表の高さ − 標準の天端。
	CHECK(near(planLevelShift(levels[1], stories[1]), 2700.0 - 3490.0));
	CHECK(near(planLevelShift(levels[2], stories[1]), 0.0));
}

// 水平な横架材が 1 本も無い階にも標準の伏図レベルが必ず 1 つでき、登り梁はどの階でも
// 伏図に映る span レイヤ（"2±0_2+1-登り梁"）へ入る。
TEST(every_story_has_one_standard_level_so_noboribari_always_reach_a_framing_plan)
{
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, collectBeamHeights(stories, {}), ImportOptions{});
	for (std::size_t i = 0; i < stories.size(); ++i)
	{
		std::size_t standards = 0;
		for (const PlanLevel& level : levels)
		{
			if (level.story == i && level.standard)
				++standards;
		}
		CHECK_EQ(standards, std::size_t(1));
	}

	std::vector<MemberCommand> members = {beam("2-登り梁", 3900.0, 2710.0),
										  beam("R-登り梁", 6300.0, 7500.0)};
	assignMemberPlanLevels(members, stories, levels);
	for (const MemberCommand& member : members)
	{
		bool onPlan = false;
		for (const PlanLevel& level : levels)
		{
			double from = 0.0;
			double to = 0.0;
			// 範囲は映す伏図のレベル（柱梁伏図＝伏図レベル・母屋伏図＝母屋のレベル）。
			const double moya = parse::moyaOrdinal(levels, level.story);
			if (parse::parseSpanOrdinals(levels, member.layer, "登り梁", from, to) &&
				((from <= level.ordinal && level.ordinal <= to) || (from <= moya && moya <= to)))
				onPlan = true;
		}
		CHECK(onPlan);
	}
}

TEST(merged_heights_join_the_previous_level_of_the_same_story_only)
{
	const std::vector<StoryInfo> stories = twoStories();
	ImportOptions options;
	options.setMergeWithPrevious(PlanLevelKey{1, 3490}, true);
	// 階の最初の高さは寄せる先が無いので、設定に挙がっていても新しく立てる（階をまたいで
	// まとめない）。
	options.setMergeWithPrevious(PlanLevelKey{1, 2700}, true);
	options.setMergeWithPrevious(PlanLevelKey{2, 6300}, true);
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, options);
	CHECK_EQ(levels.size(), std::size_t(3));
	if (levels.size() != 3)
		return;
	CHECK((levels[1].heights == std::vector<long long>{2700, 3490}));
	// 標準を含むので名前は変えない。
	CHECK(levels[1].standard && levels[1].tag.empty());
	// 通し番号は 1 / (2) / 3 / (4) / 5（各階のあとに母屋のレベル）。
	CHECK_EQ(levels[2].ordinal, 5);
	CHECK_EQ(levels[2].story, std::size_t(2));
}

TEST(lookups_pick_the_nearest_and_the_reached_level)
{
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	// 通し番号は 2700 が 3・3490 が 4（1FL が 1、その母屋のレベルが 2）。
	// いちばん近い天端（同じ近さなら低い方）。
	CHECK_EQ(nearestPlanLevel(levels, 1, 2600.0)->ordinal, 3);
	CHECK_EQ(nearestPlanLevel(levels, 1, 3200.0)->ordinal, 4);
	CHECK_EQ(nearestPlanLevel(levels, 1, 3095.0)->ordinal, 3);
	CHECK(nearestPlanLevel(levels, 5, 0.0) == nullptr);
	// 柱の上端が届く伏図レベル（天端が上端 − 1mm 以上の最も低いもの。無ければ最も高いもの）。
	CHECK_EQ(planLevelAbove(levels, 1, 2550.0)->ordinal, 3);
	CHECK_EQ(planLevelAbove(levels, 1, 2701.0)->ordinal, 3);
	CHECK_EQ(planLevelAbove(levels, 1, 2702.0)->ordinal, 4);
	CHECK_EQ(planLevelAbove(levels, 1, 3600.0)->ordinal, 4);
	// 通し番号 → 階（母屋のレベル 5 は 2 階、6 は最上階）。
	CHECK_EQ(storyOfOrdinal(levels, 3.0), std::size_t(1));
	CHECK_EQ(storyOfOrdinal(levels, 5.0), std::size_t(1));
	CHECK_EQ(storyOfOrdinal(levels, 6.0), std::size_t(2));
	// 伏図レベルが無ければ階 s の標準を 2s+1 とみなす。
	CHECK_EQ(storyOfOrdinal({}, 3.0), std::size_t(1));
}

TEST(title_suffix_only_when_a_story_has_several_plans)
{
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> split =
		buildPlanLevels(stories, {{590}, {2700, 2800, 3490}, {6300}}, ImportOptions{});
	CHECK(planLevelTitleSuffix(split, split[0]).empty());
	CHECK_EQ(planLevelTitleSuffix(split, split[1]), std::string("（FL-800）"));

	ImportOptions options;
	options.setMergeWithPrevious(PlanLevelKey{1, 2800}, true);
	const std::vector<PlanLevel> merged =
		buildPlanLevels(stories, {{590}, {2700, 2800, 3490}, {6300}}, options);
	CHECK_EQ(planLevelTitleSuffix(merged, merged[1]), std::string("（FL-800・FL-700）"));
}

TEST(ordinal_labels_round_trip_through_the_plan_levels)
{
	// 通し番号 ⇔ 符号（"2-1" / "2+1" / "R+1"）。段数はその階の標準の通し番号との差。
	// 通し番号は 1FL=1・2FL=3, 4（標準 4）・RFL=6、母屋のレベルは 2 / 5 / 7。
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	CHECK_EQ(parse::ordinalLabel(levels, 1, 3.0), std::string("2-1"));
	CHECK_EQ(parse::ordinalLabel(levels, 1, 5.0), std::string("2+1")); // 2 階の母屋のレベル
	CHECK_EQ(parse::spanLevelLabel(levels, 5.0), std::string("2+1"));  // 番号の階で書く
	CHECK_EQ(parse::spanLevelLabel(levels, 6.0), std::string("R±0"));
	CHECK_EQ(parse::spanLevelLabel(levels, 7.0), std::string("R+1"));
	double ordinal = 0.0;
	CHECK(parse::labelOrdinal(levels, "2-1", ordinal) && near(ordinal, 3.0));
	CHECK(parse::labelOrdinal(levels, "2+1", ordinal) && near(ordinal, 5.0));
	CHECK(parse::labelOrdinal(levels, "R+1", ordinal) && near(ordinal, 7.0));
	// 符号として読めないものは引き直せない。
	CHECK(!parse::labelOrdinal(levels, "2", ordinal));
	CHECK(!parse::labelOrdinal(levels, "共通", ordinal));
}

TEST(ordinal_labels_without_plan_levels_use_the_story_number)
{
	// 伏図レベルが無い（単体テストで階だけを扱う）ときは、階 s の標準を 2s+1・母屋の
	// レベルを 2s+2 とみなす（standardOrdinalWithoutLevels）。最上階 "R" は番号が分からない
	// ので引き直せない。
	CHECK_EQ(parse::ordinalLabel({}, 1, 3.0), std::string("2±0"));
	CHECK_EQ(parse::ordinalLabel({}, 1, 4.0), std::string("2+1"));
	CHECK_EQ(parse::storyOfOrdinal({}, 4.0), std::size_t(1));
	CHECK_EQ(parse::storyOfOrdinal({}, 0.0), std::size_t(0));
	CHECK(near(parse::moyaOrdinal({}, 1), 4.0));
	double ordinal = 0.0;
	CHECK(parse::labelOrdinal({}, "2+1", ordinal) && near(ordinal, 4.0));
	CHECK(!parse::labelOrdinal({}, "R±0", ordinal));
	double from = 0.0;
	double to = 0.0;
	CHECK(!parse::parseSpanOrdinals({}, "2±0_R±0-柱", "柱", from, to));
	CHECK(parse::parseSpanOrdinals({}, "1±0_3±0-柱", "柱", from, to));
	CHECK(near(from, 1.0) && near(to, 5.0));
}

TEST(span_layers_are_collected_for_any_suffix)
{
	// 柱と登り梁で同じ集め方（parse/Column の collectSpanLayers）。span でない名前と
	// 接尾辞の違う名前は読み飛ばし、重複を除いて (from, to) 昇順。
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	const std::vector<parse::ColumnSpan> spans = parse::collectSpanLayers(
		{"2±0_2+1-登り梁", "2-1_2-1-登り梁", "2±0_2+1-登り梁", "2-登り梁", "1±0_2-1-柱"}, "登り梁",
		levels);
	CHECK_EQ(spans.size(), std::size_t(2));
	if (spans.size() != 2)
		return;
	CHECK_EQ(spans[0].layer, std::string("2-1_2-1-登り梁"));
	CHECK(near(spans[0].from, 3.0) && near(spans[0].to, 3.0));
	CHECK_EQ(spans[0].toLabel, std::string("2-1"));
	CHECK_EQ(spans[1].layer, std::string("2±0_2+1-登り梁"));
	CHECK(near(spans[1].from, 4.0) && near(spans[1].to, 5.0));
}

// ---------------------------------------------------------------------------
// 横架材の振り分け
// ---------------------------------------------------------------------------

TEST(members_move_to_their_plan_level_layer)
{
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	std::vector<MemberCommand> members = {
		beam("2±0-横架材", 2700.0, 2700.0), beam("2±0-横架材", 3490.0, 3490.0),
		// 傾いた材は**水下側**（低い側の端）で切り分ける（ご要望）。
		beam("2±0-横架材", 3490.0, 2750.0), beam("2±0-横架材", 3300.0, 4000.0),
		// 母屋レイヤの材は振り分けない（母屋伏図が別にある）。
		beam("2-母屋", 2700.0, 2700.0),
		// 登り梁は映す伏図の範囲の span レイヤへ（noboribariSpan。下のテスト）。2700（通し
		// 番号 3）と 3490（4）を跨ぎ母屋伏図にも届くもの、3490 だけを跨ぎ母屋伏図にも届くもの。
		beam("2-登り梁", 3900.0, 2710.0), beam("2-登り梁", 3480.0, 4200.0)};
	assignMemberPlanLevels(members, stories, levels);
	CHECK_EQ(members[0].layer, std::string("2-1-横架材"));
	CHECK_EQ(members[1].layer, std::string("2±0-横架材"));
	CHECK_EQ(members[2].layer, std::string("2-1-横架材"));
	CHECK_EQ(members[3].layer, std::string("2±0-横架材"));
	CHECK_EQ(members[4].layer, std::string("2-母屋"));
	// 登り梁の span の端は材の階の符号で書く。母屋伏図まで映す材の上端は、2 階の母屋の
	// レベル "2+1"（いちばん上の伏図レベル 2±0 の 1 段上。通し番号 5）。
	CHECK_EQ(members[5].layer, std::string("2-1_2+1-登り梁"));
	CHECK_EQ(members[6].layer, std::string("2±0_2+1-登り梁"));
	// 高さ基準もその span のレベルへ。
	CHECK_EQ(members[6].startBound.level, std::string("2±0_2+1-登り梁"));
	CHECK_EQ(members[6].endBound.level, std::string("2±0_2+1-登り梁"));
	// 符号から通し番号へ引き直せる（伏図の切断と比べるため）。
	double from = 0.0;
	double to = 0.0;
	CHECK(parse::parseSpanOrdinals(levels, members[5].layer, "登り梁", from, to));
	CHECK(near(from, 3.0) && near(to, 5.0));
}

TEST(noboribari_take_the_span_of_the_plans_they_appear_on)
{
	// 2 階の伏図レベルは 2700（通し番号 3）と 3490（4。いちばん上）、母屋のレベルは 5。
	// 柱梁伏図は高さを跨ぐ（下端 ≤ 高さ ≤ 上端）登り梁を、母屋伏図は上端が 3490 以上の
	// 登り梁を映す（ご要望）。範囲は映す伏図のレベルの範囲（両端を含む・整数）。
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	const auto span = [&](double startZ, double endZ)
	{
		double from = 0.0;
		double to = 0.0;
		CHECK(noboribariSpan(beam("2-登り梁", startZ, endZ), levels, 1, from, to));
		return std::pair<double, double>{from, to};
	};
	// 3490 より上にだけある（下端 3550）: 母屋伏図だけ。
	CHECK((span(3700.0, 4500.0) == std::pair<double, double>{5.0, 5.0}));
	// 2700 を跨ぐが上端が 3490 に届かない（実機の軒高 -1570〜-302 の登り梁）: 2700 の柱梁
	// 伏図だけ。
	CHECK((span(2690.0, 3300.0) == std::pair<double, double>{3.0, 3.0}));
	// 3490 を跨ぐ: 3490 の柱梁伏図と母屋伏図。
	CHECK((span(3480.0, 4200.0) == std::pair<double, double>{4.0, 5.0}));
	// どれも跨がず母屋伏図にも届かない: 低い側の端に近い伏図レベル（3300 → 3490）の
	// 柱梁伏図。
	CHECK((span(3480.0, 3300.0) == std::pair<double, double>{4.0, 4.0}));
	// 端がちょうど伏図レベルの高さなら跨ぐとみなす（上端 2700）。
	CHECK((span(2600.0, 2700.0) == std::pair<double, double>{3.0, 3.0}));
}

TEST(eaves_girders_move_to_their_own_layer)
{
	// 軒桁（クラスで判別）は母屋伏図に薄く重ねるため、横架材レイヤから軒桁の専用レイヤへ
	// 分ける。伏図レベルの分け方は横架材と同じ（標準でない高さは印が付く）。高さ基準の
	// レベルは元のまま（横架材天端）。
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	std::vector<MemberCommand> members = {beam("2±0-横架材", 3490.0, 3490.0),
										  beam("2±0-横架材", 2700.0, 2700.0),
										  beam("R±0-横架材", 6300.0, 6300.0)};
	for (MemberCommand& member : members)
		member.drawClass = parse::CLASS_NOKIGETA;
	assignMemberPlanLevels(members, stories, levels);
	CHECK_EQ(members[0].layer, std::string("2±0-軒桁"));
	CHECK_EQ(members[1].layer, std::string("2-1-軒桁"));
	CHECK_EQ(members[2].layer, std::string("R±0-軒桁"));
	// 取り合いを判定するときは同じ伏図レベルの横架材レイヤとして読み替える。
	CHECK_EQ(parse::beamGroupLayer(members[1].layer), std::string("2-1-横架材"));
	CHECK_EQ(parse::beamGroupLayer(members[2].layer), std::string("R±0-横架材"));
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(only_the_skip_floor_fixture_has_several_heights_per_story)
{
	// ほかのフィクスチャは階ごとに高さが 1 つ＝伏図もレイヤも従来と同じになる。
	forEachFixture(failures,
				   [&](const std::string& name, const Model& model)
				   {
					   Context context(model);
					   const std::vector<PlanLevel>& levels = context.planLevels();
					   const bool skip = name == "スキップフロア_サンプル.ifc";
					   if (!skip)
						   CHECK_EQ(levels.size(), context.stories().size());
					   for (const PlanLevel& level : levels)
					   {
						   if (!skip)
							   CHECK(level.standard && level.tag.empty());
						   CHECK(!level.heights.empty());
					   }
				   });
}

TEST(skip_floor_fixture_levels)
{
	bool ok = false;
	const Model& model = fixture("スキップフロア_サンプル.ifc", ok);
	CHECK(ok);
	Context context(model);
	const std::vector<PlanLevel>& levels = context.planLevels();
	const std::vector<long long> expected = {572, 2699, 3531, 5542, 6010, 6374};
	CHECK_EQ(levels.size(), expected.size());
	if (levels.size() != expected.size())
		return;
	// 通し番号は各階のあとに母屋のレベルを 1 つ空ける（1 / (2) / 3, 4 / (5) / 6, 7, 8 / (9)）。
	const std::vector<int> ordinals = {1, 3, 4, 6, 7, 8};
	for (std::size_t k = 0; k < expected.size(); ++k)
	{
		CHECK(levels[k].heights == std::vector<long long>{expected[k]});
		CHECK_EQ(levels[k].ordinal, ordinals[k]);
	}
	// 標準は 2FL の横架材天端（3531）と RFL の軒高（6374）。
	CHECK(levels[2].standard && levels[5].standard);
	CHECK(!levels[1].standard && !levels[3].standard && !levels[4].standard);

	// 横架材はすべて自分の高さの伏図レベルのレイヤに載る（水平な材は天端がその高さ）。
	for (const MemberCommand& member : context.members())
	{
		if (member.layer == "2-1-横架材")
			CHECK(near(member.elevation, 2699.0, 0.5));
		if (member.layer == "2±0-横架材")
			CHECK(near(member.elevation, 3531.0, 0.5));
		if (member.layer == "R-1-横架材")
			CHECK(near(member.elevation, 6010.0, 0.5));
	}

	// 柱の span は伏図レベルの符号（通し番号へ引き直して比べる）: GL+2699 の横架材に立つ
	// 柱は from=3（"2-1"）、GL+3531 は from=4（"2±0"）。1 階の管柱は受ける横架材の高さで
	// to が分かれる（"1±0_2-1" と "1±0_2±0"）。
	//
	// **from は柱が属する IFC の階の中で選ぶ**——このモデルには RFL に属しながら GL+2699 の
	// 横架材に立つ小屋束が 1 本あり、それは RFL の伏図レベル（from=6）に置く。柱の高さ
	// 基準はレイヤの階から数えるので（StoryBoundCommand::storyOffset）、別の階の番号に
	// すると基準のレベルを失う。
	bool from2 = false;
	bool from3 = false;
	for (const core::ColumnCommand& column : context.columns())
	{
		double from = 0.0;
		double to = 0.0;
		CHECK(parse::parseSpanOrdinals(context.planLevels(), column.layer, "柱", from, to));
		CHECK(to > from);
		if (near(column.elevation, 2699.0, 0.5) && near(column.height, 7362.0 - 2699.0, 0.5))
		{
			// RFL の伏図レベルのうち 2699 にいちばん近い 5542（軒高の 2 段下）。
			CHECK(near(from, 6.0));
			CHECK(column.layer.starts_with("R-2_"));
			continue;
		}
		if (near(column.elevation, 2699.0, 0.5))
		{
			// 2FL の標準（3531）の 1 段下に立つ: "2-1_…"。
			CHECK(near(from, 3.0));
			CHECK(column.layer.starts_with("2-1_"));
			from2 = true;
		}
		if (near(column.elevation, 3531.0, 0.5))
		{
			CHECK(near(from, 4.0));
			CHECK(column.layer.starts_with("2±0_"));
			from3 = true;
		}
	}
	CHECK(from2 && from3);
}

TEST(plan_level_choices_list_every_height_before_merging)
{
	// 設定ダイアログの候補は、まとめる前の高さ 1 つずつ。「前のレベルとまとめる」は同じ階の
	// 2 つ目以降だけに問える。
	const std::vector<core::PlanLevelChoice> choices =
		parse::scanPlanLevelChoices(HomeskzIfcTests::fixturePath("スキップフロア_サンプル.ifc"));
	CHECK_EQ(choices.size(), std::size_t(6));
	if (choices.size() != 6)
		return;
	CHECK(choices[1].key == (PlanLevelKey{1, 2699}));
	CHECK_EQ(choices[1].planTitle, std::string("2階床伏図"));
	// 高さは FL（最上階は軒高）から書く（ご要望）。
	CHECK_EQ(choices[1].heightText, std::string("FL-872"));
	CHECK_EQ(choices[2].heightText, std::string("FL-40"));
	CHECK_EQ(choices[5].heightText, std::string("軒高±0"));
	CHECK(!choices[1].canMerge);
	CHECK(choices[2].key == (PlanLevelKey{1, 3531}));
	CHECK(choices[2].canMerge);
	CHECK_EQ(choices[5].planTitle, std::string("2階小屋伏図"));
	CHECK(choices[5].canMerge);
	// 読めないファイルは空（取り込みは止めない）。
	CHECK(parse::scanPlanLevelChoices("/no/such/file.ifc").empty());
}

TEST_MAIN();
