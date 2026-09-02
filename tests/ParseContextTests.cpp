//
//	ParseContextTests.cpp
//
//	解析中の共有コンテキスト（src/parse/Context）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネスで走る（CLAUDE.md「テスト方針」）。
//
//	コンテキストは「同じ Model に対する同じ前処理を 1 回で済ませる」ためのもので、
//	**振る舞いを変えないこと**が最も重要な性質。したがってここでは
//	  1. 何度呼んでも同じ結果を返す（キャッシュが効いており、かつ内容が壊れない）
//	  2. コンテキスト経由の結果が、コンテキストを使わない従来の関数と一致する
//	の 2 点を確かめる。2 が崩れればキャッシュがどこかで嘘をついている。
//
//	実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"
#include "Fixtures.h"

#include "parse/AnchorBolt.h"
#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/FireBrace.h"
#include "parse/Floor.h"
#include "parse/FloorPost.h"
#include "parse/Footing.h"
#include "parse/Grid.h"
#include "parse/IfcGeometry.h"
#include "parse/Loader.h"
#include "parse/Member.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"
#include "parse/Story.h"

#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::Context;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::StoryInfo;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// 屋根階に床版が無く、床梁からロフト床を合成する経路を持つフィクスチャ
	// （ParseFloorTests のロフト床ケースと同じもの）。
	constexpr const char* kLoftFixture = "グレー本モデルプラン1【3階】.ifc";
	// 下屋根（中間階の屋根版）を含み、屋根面が複数あるフィクスチャ。
	constexpr const char* kRoofFixture = "伏図次郎【2階】.ifc";
} // namespace

// ---------------------------------------------------------------------------
// ストーリ・通り芯中心・階の要素: 2 度目も同じで、非コンテキスト版とも一致する
// ---------------------------------------------------------------------------

TEST(stories_are_cached_and_match_the_plain_call)
{
	bool ok = false;
	const Model& model = fixture(kLoftFixture, ok);
	CHECK(ok);

	Context context(model);
	const std::vector<StoryInfo> first = context.stories();
	const std::vector<StoryInfo> second = context.stories(); // キャッシュヒット
	const std::vector<StoryInfo> plain = parse::collectStories(model);

	CHECK(!first.empty());
	CHECK_EQ(first.size(), second.size());
	CHECK_EQ(first.size(), plain.size());
	for (std::size_t i = 0; i < first.size(); ++i)
	{
		CHECK_EQ(first[i].id, second[i].id);
		CHECK_EQ(first[i].id, plain[i].id);
		CHECK(near(first[i].elevation, plain[i].elevation));
		CHECK(near(first[i].beamOffset, plain[i].beamOffset));
		CHECK_EQ(first[i].isTop, plain[i].isTop);
	}
}

TEST(grid_centre_is_cached_and_matches_the_plain_call)
{
	bool ok = false;
	const Model& model = fixture(kLoftFixture, ok);
	CHECK(ok);

	Context context(model);
	const Vec2 first = context.gridCenter();
	const Vec2 second = context.gridCenter(); // キャッシュヒット

	Vec2 plain{0.0, 0.0};
	CHECK(parse::gridCenterOf(parse::collectGridLines(model), plain));

	CHECK(near(first.x, second.x));
	CHECK(near(first.y, second.y));
	CHECK(near(first.x, plain.x));
	CHECK(near(first.y, plain.y));
}

TEST(grid_centre_is_origin_when_there_are_no_grid_axes)
{
	// 通り芯を 1 本も取れないモデルでは補正なし＝(0,0)（各要素の従来の挙動と同じ）。
	Model const model =
		parse::loadIfcFromText("ISO-10303-21;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n");
	Context context(model);
	CHECK(near(context.gridCenter().x, 0.0));
	CHECK(near(context.gridCenter().y, 0.0));
}

TEST(story_elements_are_cached_and_match_the_plain_call)
{
	bool ok = false;
	const Model& model = fixture(kLoftFixture, ok);
	CHECK(ok);

	Context context(model);
	const std::vector<StoryInfo> stories = context.stories();
	CHECK(!stories.empty());

	for (const StoryInfo& story : stories)
	{
		const std::vector<int>& first = context.storyElements(story.id);
		const std::vector<int>& second = context.storyElements(story.id); // キャッシュヒット
		CHECK_EQ(&first, &second); // 同じ実体＝走査をやり直していない
		CHECK(first == parse::collectStoryElements(model, story.id));
	}
}

TEST(unknown_storey_id_yields_no_elements)
{
	bool ok = false;
	const Model& model = fixture(kLoftFixture, ok);
	CHECK(ok);

	// 存在しない #id でも落ちず、空を返して覚える（1 要素の欠損で全体を止めない）。
	Context context(model);
	CHECK(context.storyElements(-1).empty());
	CHECK(context.storyElements(-1).empty());
}

// ---------------------------------------------------------------------------
// ロフト床: 2 度目はキャッシュから返る（領域合成をやり直さない）
// ---------------------------------------------------------------------------

TEST(loft_floor_regions_are_cached_and_match_the_plain_call)
{
	bool ok = false;
	const Model& model = fixture(kLoftFixture, ok);
	CHECK(ok);

	Context context(model);
	const std::vector<StoryInfo> stories = context.stories();
	CHECK(!stories.empty());
	const int topId = stories.back().id;

	// 1 回目は合成が走り、2 回目はキャッシュヒット（本番では storyHasLoftFloor と
	// buildFloorCommands が同じ屋根階について続けて呼ぶ経路にあたる）。**同じ実体を
	// 返すこと**がキャッシュが効いている証拠で、セル格子の flood fill をやり直していない。
	const std::vector<parse::LoftFloorRegion>& first = context.loftFloorRegions(topId);
	const std::vector<parse::LoftFloorRegion>& second = context.loftFloorRegions(topId);
	CHECK_EQ(&first, &second);

	// 内容はコンテキストを使わない従来の関数と一致する（実フィクスチャの屋根階は床梁が
	// 領域を囲まないため空になる——合成そのものの中身は ParseFloorTests の合成モデルで
	// 検証しており、ここで見たいのは「キャッシュが答えを変えないこと」）。
	const std::vector<parse::LoftFloorRegion> plain = parse::loftFloorRegions(model, topId);
	CHECK_EQ(first.size(), plain.size());
	for (std::size_t i = 0; i < first.size(); ++i)
	{
		CHECK_EQ(first[i].boundary.size(), plain[i].boundary.size());
		CHECK(near(first[i].beamTopOffset, plain[i].beamTopOffset));
		for (std::size_t j = 0; j < first[i].boundary.size(); ++j)
		{
			CHECK(near(first[i].boundary[j].x, plain[i].boundary[j].x));
			CHECK(near(first[i].boundary[j].y, plain[i].boundary[j].y));
		}
	}
}

// ---------------------------------------------------------------------------
// 屋根面: 垂木と野地板が同じ面を共有する（解決は 1 回で済む）
// ---------------------------------------------------------------------------

TEST(roof_planes_are_cached_and_match_the_plain_call)
{
	bool ok = false;
	const Model& model = fixture(kRoofFixture, ok);
	CHECK(ok);

	Context context(model);
	std::size_t resolved = 0;
	for (const StoryInfo& story : context.stories())
	{
		for (const int elementId : context.storyElements(story.id))
		{
			const parse::Entity* element = model.entity(elementId);
			if (element == nullptr || !parse::isRoofSlab(*element))
				continue;

			const parse::RoofPlane* first = context.roofPlane(elementId);
			const parse::RoofPlane* second = context.roofPlane(elementId); // キャッシュヒット
			// 2 度目は同じ実体を返す（＝解決をやり直していない）。
			CHECK_EQ(first, second);

			parse::RoofPlane plain;
			const bool plainOk = parse::roofPlane(model, element, plain);
			CHECK_EQ(first != nullptr, plainOk);
			if (first == nullptr)
				continue;

			++resolved;
			CHECK_EQ(first->vertices.size(), plain.vertices.size());
			CHECK(near(first->normal.z, plain.normal.z));
			for (std::size_t i = 0; i < first->vertices.size(); ++i)
			{
				CHECK(near(first->vertices[i].x, plain.vertices[i].x));
				CHECK(near(first->vertices[i].z, plain.vertices[i].z));
			}
		}
	}
	CHECK(resolved > 0); // このフィクスチャは主屋根＋下屋根の屋根版を持つ
}

TEST(unresolvable_roof_plane_is_remembered_as_absent)
{
	bool ok = false;
	const Model& model = fixture(kRoofFixture, ok);
	CHECK(ok);

	// 存在しない #id は解決できない。**その事実もキャッシュする**ので、2 度目も
	// nullptr が返り、解決をやり直さない。
	Context context(model);
	CHECK(context.roofPlane(-1) == nullptr);
	CHECK(context.roofPlane(-1) == nullptr);
}

// ---------------------------------------------------------------------------
// 共有コンテキストを通した Document が、通さない場合と一致する（本命の回帰）
// ---------------------------------------------------------------------------

TEST(members_are_cached_and_match_the_plain_call)
{
	// 横架材の解析はストーリ・垂木・登り梁の補正が共有するので、コンテキストは 1 回だけ
	// 走らせて覚える。2 度目が同じ結果（同じ実体）を返し、キャッシュを通さない呼び出しとも
	// 一致すること。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	if (!ok)
		return;

	Context context(model);
	const std::vector<core::MemberCommand>& first = context.members();
	const std::vector<core::MemberCommand>& second = context.members();
	CHECK(&first == &second);

	const std::vector<core::MemberCommand> plain = parse::buildMemberCommands(model);
	CHECK_EQ(first.size(), plain.size());
	CHECK(!plain.empty());
	for (std::size_t i = 0; i < first.size() && i < plain.size(); ++i)
	{
		CHECK_EQ(first[i].layer, plain[i].layer);
		CHECK_EQ(first[i].memberId, plain[i].memberId);
		CHECK(near(first[i].elevation, plain[i].elevation));
		CHECK(near(first[i].start.x, plain[i].start.x));
	}
}

TEST(columns_are_cached_and_match_the_plain_call)
{
	// 柱の解析はストーリ（span 柱レベル）・Document の columns・登り梁の端部詰めが共有する
	// ので、コンテキストは 1 回だけ走らせて覚える。2 度目が同じ結果（同じ実体）を返し、
	// キャッシュを通さない呼び出しとも一致すること。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	if (!ok)
		return;

	Context context(model);
	const std::vector<core::ColumnCommand>& first = context.columns();
	const std::vector<core::ColumnCommand>& second = context.columns();
	CHECK(&first == &second);

	const std::vector<core::ColumnCommand> plain = parse::buildColumnCommands(model);
	CHECK_EQ(first.size(), plain.size());
	CHECK(!plain.empty());
	for (std::size_t i = 0; i < first.size() && i < plain.size(); ++i)
	{
		CHECK_EQ(first[i].layer, plain[i].layer);
		CHECK_EQ(first[i].memberId, plain[i].memberId);
		CHECK(near(first[i].elevation, plain[i].elevation));
		CHECK(near(first[i].position.x, plain[i].position.x));
	}
}

TEST(walls_are_cached_and_match_the_plain_call)
{
	// 立上りの解析は基礎命令の組み立てと床束の配置が共有するので、コンテキストは
	// 1 回だけ走らせて覚える。2 度目が同じ結果（同じ実体）を返し、キャッシュを通さない
	// 呼び出しとも一致すること。
	bool ok = false;
	const Model& model = fixture(kRoofFixture, ok);
	CHECK(ok);
	if (!ok)
		return;

	Context context(model);
	const std::vector<parse::RiserPiece>& first = context.walls();
	const std::vector<parse::RiserPiece>& second = context.walls();
	CHECK(&first == &second);

	const std::vector<parse::RiserPiece> plain = parse::buildWallCommands(model);
	CHECK_EQ(first.size(), plain.size());
	CHECK(!plain.empty());
	for (std::size_t i = 0; i < first.size() && i < plain.size(); ++i)
	{
		CHECK(near(first[i].thickness, plain[i].thickness));
		CHECK(near(first[i].start.x, plain[i].start.x));
		CHECK(near(first[i].top, plain[i].top));
	}
}

TEST(context_backed_commands_match_the_plain_commands)
{
	HomeskzIfcTests::forEachFixture(
		failures,
		[&](const std::string&, const Model& model)
		{
			// 1 つのコンテキストを全要素で共有した結果（＝buildDocument と同じ経路）と、
			// 要素ごとに作り直した結果（＝従来の経路）が一致すること。
			Context shared(model);
			const std::vector<core::StoryCommand> stories = parse::buildStoryCommands(shared);
			const std::vector<core::GridCommand> grids = parse::buildGridCommands(shared);
			const std::vector<core::FloorCommand> floors = parse::buildFloorCommands(shared);
			const std::vector<core::MemberCommand> members = parse::buildMemberCommands(shared);
			const std::vector<core::RafterCommand> rafters = parse::buildRafterCommands(shared);
			const std::vector<core::RoofCommand> roofs = parse::buildRoofCommands(shared);
			const std::vector<parse::SlabPiece> slabs =
				parse::buildSlabCommands(shared, shared.walls());
			// M11 シンボル置換系（仕口は命令から導出するのでコンテキストを取らない）。
			const std::vector<core::SymbolCommand> bolts = parse::buildAnchorBoltCommands(shared);
			const std::vector<core::SymbolCommand> posts = parse::buildFloorPostCommands(shared);
			const std::vector<core::SymbolCommand> braces = parse::buildFireBraceCommands(shared);

			CHECK_EQ(stories.size(), parse::buildStoryCommands(model).size());
			CHECK_EQ(slabs.size(), parse::buildSlabCommands(model).size());
			CHECK_EQ(bolts.size(), parse::buildAnchorBoltCommands(model).size());
			CHECK_EQ(posts.size(), parse::buildFloorPostCommands(model).size());
			CHECK_EQ(braces.size(), parse::buildFireBraceCommands(model).size());
			CHECK_EQ(grids.size(), parse::buildGridCommands(model).size());
			CHECK_EQ(members.size(), parse::buildMemberCommands(model).size());
			CHECK_EQ(rafters.size(), parse::buildRafterCommands(model).size());
			CHECK_EQ(roofs.size(), parse::buildRoofCommands(model).size());

			const std::vector<core::FloorCommand> plainFloors = parse::buildFloorCommands(model);
			CHECK_EQ(floors.size(), plainFloors.size());
			for (std::size_t i = 0; i < floors.size(); ++i)
			{
				CHECK_EQ(floors[i].layer, plainFloors[i].layer);
				CHECK_EQ(floors[i].boundary.size(), plainFloors[i].boundary.size());
				CHECK(near(floors[i].elevation, plainFloors[i].elevation));
				CHECK(near(floors[i].bound.offset, plainFloors[i].bound.offset));
			}
		});
}

TEST_MAIN();
