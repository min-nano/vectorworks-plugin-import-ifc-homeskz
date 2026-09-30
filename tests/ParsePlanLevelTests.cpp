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

TEST(plan_level_tag_measures_from_fl_or_eaves)
{
	// 高さは GL ではなくその階の FL（最上階は軒高）から測る（ご要望）。0 は "±"。
	CHECK_EQ(core::planLevelTag(2699, 3571, false), std::string("(FL-872)"));
	CHECK_EQ(core::planLevelTag(3700, 3571, false), std::string("(FL+129)"));
	CHECK_EQ(core::planLevelTag(6374, 6374, true), std::string("(軒高±0)"));
	CHECK_EQ(core::planLevelHeightText(5542, 6374, true), std::string("軒高-832"));
}

TEST(strip_plan_level_tag_restores_the_base_name)
{
	CHECK_EQ(core::stripPlanLevelTag("2-横架材天端(FL-872)"), std::string("2-横架材天端"));
	CHECK_EQ(core::stripPlanLevelTag("R-軒高(軒高-832)"), std::string("R-軒高"));
	CHECK_EQ(core::stripPlanLevelTag("R-軒高(軒高±0)"), std::string("R-軒高"));
	CHECK_EQ(core::stripPlanLevelTag("FL(FL+100)"), std::string("FL"));
	// 印の無い名前・印に見えない括弧はそのまま。
	CHECK_EQ(core::stripPlanLevelTag("2-横架材天端"), std::string("2-横架材天端"));
	CHECK_EQ(core::stripPlanLevelTag("柱(通し)"), std::string("柱(通し)"));
	CHECK_EQ(core::stripPlanLevelTag("柱(FL)"), std::string("柱(FL)"));
	CHECK_EQ(core::stripPlanLevelTag(""), std::string());
}

TEST(layer_order_treats_tagged_levels_like_their_base_type)
{
	// 床は背面・耐力壁は前面へ回す決まり（core::desiredStoryLayerOrder）が、伏図レベルの
	// 印の付いたレベルにも効く。
	core::StoryCommand story;
	story.name = "2階";
	story.suffix = "2";
	story.levels = {core::LevelCommand{"FL", 0.0, "2-FL"},
					core::LevelCommand{"FL(FL-872)", -832.0, "2-FL(FL-872)"},
					core::LevelCommand{"耐力壁(FL-872)", -872.0, "2-耐力壁(FL-872)"},
					core::LevelCommand{"横架材天端", -40.0, "2-横架材天端"}};
	const std::vector<std::string> order = core::desiredStoryLayerOrder({story}, {});
	CHECK_EQ(order.size(), std::size_t(5));
	if (order.size() != 5)
		return;
	CHECK_EQ(order[0], std::string(core::kGridLayer));
	CHECK_EQ(order[1], std::string("2-耐力壁(FL-872)"));
	CHECK_EQ(order[2], std::string("2-横架材天端"));
	CHECK_EQ(order[3], std::string("2-FL"));
	CHECK_EQ(order[4], std::string("2-FL(FL-872)"));
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
		beam("2-横架材天端", 3490.0, 3490.0), beam("2-横架材天端", 3490.4, 3490.4),
		beam("2-横架材天端", 3489.0, 3489.0), beam("2-横架材天端", 2700.0, 2700.0),
		beam("2-横架材天端", 3000.0, 3400.0), beam("2-母屋", 3900.0, 3900.0),
		beam("1-横架材天端", 590.0, 590.0)};
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
	// 通し番号は下から 1, 2, 3 …（柱の span の番号になる）。
	for (std::size_t k = 0; k < levels.size(); ++k)
		CHECK_EQ(levels[k].ordinal, static_cast<int>(k) + 1);
	// 標準の天端（2FL は 3490）を含むものは名前を変えず、ほかは高さの印。
	CHECK(levels[1].story == 1 && !levels[1].standard);
	CHECK_EQ(levels[1].tag, std::string("(FL-800)"));
	CHECK(levels[2].standard && levels[2].tag.empty());
	CHECK(levels[0].standard && levels[3].standard);
	// レベルのずらし量は代表の高さ − 標準の天端。
	CHECK(near(planLevelShift(levels[1], stories[1]), 2700.0 - 3490.0));
	CHECK(near(planLevelShift(levels[2], stories[1]), 0.0));
}

// 水平な横架材が 1 本も無い階にも標準の伏図レベルが必ず 1 つでき、登り梁はどの階でも
// いずれかの伏図レベルのレイヤ（＝柱梁伏図に映るレイヤ）へ入る。母屋伏図が登り梁を
// 映さない（parse/Sheet）のは、これで登り梁がどの伏図からも消えないと言えるから。
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
			if (member.layer == planLevelLayer(level, stories[level.story], "登り梁"))
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
	CHECK_EQ(levels[2].ordinal, 3);
	CHECK_EQ(levels[2].story, std::size_t(2));
}

TEST(lookups_pick_the_nearest_and_the_reached_level)
{
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	// いちばん近い天端（同じ近さなら低い方）。
	CHECK_EQ(nearestPlanLevel(levels, 1, 2600.0)->ordinal, 2);
	CHECK_EQ(nearestPlanLevel(levels, 1, 3200.0)->ordinal, 3);
	CHECK_EQ(nearestPlanLevel(levels, 1, 3095.0)->ordinal, 2);
	CHECK(nearestPlanLevel(levels, 5, 0.0) == nullptr);
	// 柱の上端が届く伏図レベル（天端が上端 − 1mm 以上の最も低いもの。無ければ最も高いもの）。
	CHECK_EQ(planLevelAbove(levels, 1, 2550.0)->ordinal, 2);
	CHECK_EQ(planLevelAbove(levels, 1, 2701.0)->ordinal, 2);
	CHECK_EQ(planLevelAbove(levels, 1, 2702.0)->ordinal, 3);
	CHECK_EQ(planLevelAbove(levels, 1, 3600.0)->ordinal, 3);
	// 通し番号 → 階。
	CHECK_EQ(storyOfOrdinal(levels, 3.0), std::size_t(1));
	CHECK_EQ(storyOfOrdinal(levels, 4.5), std::size_t(2));
	// 伏図レベルが無ければ従来の対応（番号 − 1）。
	CHECK_EQ(storyOfOrdinal({}, 3.0), std::size_t(2));
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

// ---------------------------------------------------------------------------
// 横架材の振り分け
// ---------------------------------------------------------------------------

TEST(members_move_to_their_plan_level_layer)
{
	const std::vector<StoryInfo> stories = twoStories();
	const std::vector<PlanLevel> levels =
		buildPlanLevels(stories, {{590}, {2700, 3490}, {6300}}, ImportOptions{});
	std::vector<MemberCommand> members = {
		beam("2-横架材天端", 2700.0, 2700.0), beam("2-横架材天端", 3490.0, 3490.0),
		// 傾いた材は**水下側**（低い側の端）で切り分ける（ご要望）。
		beam("2-横架材天端", 3490.0, 2750.0), beam("2-横架材天端", 3300.0, 4000.0),
		// 母屋レイヤの材は振り分けない（母屋伏図が別にある）。
		beam("2-母屋", 2700.0, 2700.0),
		// 登り梁の専用レイヤの材も水下側で切り分ける（その高さの柱梁伏図に映すため）。
		beam("2-登り梁", 3900.0, 2710.0), beam("2-登り梁", 3480.0, 4200.0)};
	assignMemberPlanLevels(members, stories, levels);
	CHECK_EQ(members[0].layer, std::string("2-横架材天端(FL-800)"));
	CHECK_EQ(members[1].layer, std::string("2-横架材天端"));
	CHECK_EQ(members[2].layer, std::string("2-横架材天端(FL-800)"));
	CHECK_EQ(members[3].layer, std::string("2-横架材天端"));
	CHECK_EQ(members[4].layer, std::string("2-母屋"));
	CHECK_EQ(members[5].layer, std::string("2-登り梁(FL-800)"));
	CHECK_EQ(members[6].layer, std::string("2-登り梁"));
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
	for (std::size_t k = 0; k < expected.size(); ++k)
	{
		CHECK(levels[k].heights == std::vector<long long>{expected[k]});
		CHECK_EQ(levels[k].ordinal, static_cast<int>(k) + 1);
	}
	// 標準は 2FL の横架材天端（3531）と RFL の軒高（6374）。
	CHECK(levels[2].standard && levels[5].standard);
	CHECK(!levels[1].standard && !levels[3].standard && !levels[4].standard);

	// 横架材はすべて自分の高さの伏図レベルのレイヤに載る（水平な材は天端がその高さ）。
	for (const MemberCommand& member : context.members())
	{
		if (member.layer == "2-横架材天端(FL-872)")
			CHECK(near(member.elevation, 2699.0, 0.5));
		if (member.layer == "2-横架材天端")
			CHECK(near(member.elevation, 3531.0, 0.5));
		if (member.layer == "R-軒高(軒高-364)")
			CHECK(near(member.elevation, 6010.0, 0.5));
	}

	// 柱の span は伏図レベルの通し番号: GL+2699 の横架材に立つ柱は from=2、GL+3531 は
	// from=3。1 階の管柱は受ける横架材の高さで to が分かれる（"1to2" と "1to3"）。
	//
	// **from は柱が属する IFC の階の中で選ぶ**——このモデルには RFL に属しながら GL+2699 の
	// 横架材に立つ小屋束が 1 本あり、それは RFL の伏図レベル（from=4）に置く。柱の高さ
	// 基準はレイヤの階から数えるので（StoryBoundCommand::storyOffset）、別の階の番号に
	// すると基準のレベルを失う。
	bool from2 = false;
	bool from3 = false;
	for (const core::ColumnCommand& column : context.columns())
	{
		double from = 0.0;
		double to = 0.0;
		CHECK(parse::parseSpanLayer(column.layer, from, to));
		CHECK(to > from);
		if (near(column.elevation, 2699.0, 0.5) && near(column.height, 7362.0 - 2699.0, 0.5))
		{
			CHECK(near(from, 4.0));
			continue;
		}
		if (near(column.elevation, 2699.0, 0.5))
		{
			CHECK(near(from, 2.0));
			from2 = true;
		}
		if (near(column.elevation, 3531.0, 0.5))
		{
			CHECK(near(from, 3.0));
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
