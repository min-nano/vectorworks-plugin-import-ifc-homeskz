//
//	ParseTagTests.cpp
//
//	断面寸法データタグの解析（src/parse/Tag）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M13）: 文字角度の (-90, 90] 正規化・タグを寄せる側（上または左）・
//	伏図では表示レイヤに乗る横架材だけが対象で位置が部材の辺の中央になること・軸組図では
//	**切断面に乗る横架材だけ**が対象で位置が断面の注釈空間（切断線に沿った距離, 天端 Z）に
//	なること・memberIndex が members の添字を指すこと・実フィクスチャでの通し（全タグが
//	妥当＝validateDocument を通ること）・並び順に依存しない決定性。実フィクスチャのパスは
//	CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Context.h"
#include "parse/Loader.h"
#include "parse/Story.h"
#include "parse/Tag.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::SectionCommand;
using HomeskzIfcImport::core::SectionDirection;
using HomeskzIfcImport::core::TagCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::ViewportCommand;
using HomeskzIfcImport::parse::attachTagCommands;
using HomeskzIfcImport::parse::buildPlanTagCommands;
using HomeskzIfcImport::parse::buildSectionTagCommands;
using HomeskzIfcImport::parse::memberLevelNote;
using HomeskzIfcImport::parse::memberLevelNoteParts;
using HomeskzIfcImport::parse::StoryInfo;
using HomeskzIfcImport::parse::tagAngle;
using HomeskzIfcImport::parse::tagOffsetSide;
using HomeskzIfcImport::parse::upwardNormal;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::forEachFixtureDocument;
using HomeskzIfcTests::near;

namespace
{
	// 試験用の横架材 1 本。タグの組み立てが見るのは配置先レイヤ・天端中央線・断面幅・
	// 両端の天端 Z だけなので、そこだけを埋める。
	MemberCommand makeMember(const std::string& layer, Vec2 start, Vec2 end, double width,
							 double elevation, double endElevation)
	{
		MemberCommand member;
		member.layer = layer;
		member.memberId = "120×180";
		member.drawClass = "04構造-02木造-04床組-03床梁";
		member.start = start;
		member.end = end;
		member.width = width;
		member.height = 180.0;
		member.elevation = elevation;
		member.endElevation = endElevation;
		return member;
	}

	// 表示レイヤだけを持つビューポート命令。
	ViewportCommand makeViewport(const std::vector<std::string>& layers)
	{
		ViewportCommand viewport;
		viewport.drawingTitle = "1階床伏図";
		viewport.drawingNumber = "2";
		viewport.layers = layers;
		return viewport;
	}

	// 定 X（または定 Y）で切る軸組図の命令。切断位置と向きだけを埋める。
	SectionCommand makeSection(SectionDirection direction, double cut)
	{
		SectionCommand section;
		section.direction = direction;
		section.lineStart =
			direction == SectionDirection::X ? Vec2{cut, -10000.0} : Vec2{-10000.0, cut};
		section.lineEnd =
			direction == SectionDirection::X ? Vec2{cut, 10000.0} : Vec2{10000.0, cut};
		section.viewport.drawingNumber = "X1";
		section.viewport.drawingTitle = "X1通り";
		section.viewport.layers = {"1-横架材天端"};
		return section;
	}
} // namespace

TEST(TagAngleIsNormalisedToReadableRange)
{
	// 東西材は 0 度、北向きの材は +90 度、南向きの材は反転せず +90 度のまま
	// （(-90, 90] へ正規化するので文字が上下逆さにならない）。
	CHECK(near(tagAngle(1000.0, 0.0), 0.0, 1e-9));
	CHECK(near(tagAngle(0.0, 1000.0), 90.0, 1e-9));
	CHECK(near(tagAngle(0.0, -1000.0), 90.0, 1e-9));
	CHECK(near(tagAngle(-1000.0, 0.0), 0.0, 1e-9));
	// 斜材は素直な傾き。西向きに 45 度上がる材は −45 度へ折り返す。
	CHECK(near(tagAngle(1000.0, 1000.0), 45.0, 1e-9));
	CHECK(near(tagAngle(-1000.0, 1000.0), -45.0, 1e-9));
}

TEST(TagOffsetSidePicksUpOrLeft)
{
	// 東西材（+X 方向）は上（+Y）へ寄せる。逆向き（−X）でも同じ側を選ぶ。
	const Vec2 east = tagOffsetSide(1000.0, 0.0);
	CHECK(near(east.x, 0.0, 1e-9) && near(east.y, 1.0, 1e-9));
	const Vec2 west = tagOffsetSide(-1000.0, 0.0);
	CHECK(near(west.x, 0.0, 1e-9) && near(west.y, 1.0, 1e-9));

	// 南北材は左（−X）へ寄せる。どちら向きでも同じ側。
	const Vec2 north = tagOffsetSide(0.0, 1000.0);
	CHECK(near(north.x, -1.0, 1e-9) && near(north.y, 0.0, 1e-9));
	const Vec2 south = tagOffsetSide(0.0, -1000.0);
	CHECK(near(south.x, -1.0, 1e-9) && near(south.y, 0.0, 1e-9));

	// 長さ 0 の軸は向きを決められないので既定（上）。
	const Vec2 degenerate = tagOffsetSide(0.0, 0.0);
	CHECK(near(degenerate.x, 0.0, 1e-9) && near(degenerate.y, 1.0, 1e-9));
}

TEST(PlanTagsSitOnTheMemberEdge)
{
	const std::vector<MemberCommand> members = {
		// 表示レイヤに乗る東西材（上辺の中央にタグ）。
		makeMember("1-横架材天端", Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}, 120.0, 3000.0, 3000.0),
		// 表示レイヤに乗る南北材（左辺の中央にタグ）。
		makeMember("1-横架材天端", Vec2{500.0, 0.0}, Vec2{500.0, 1820.0}, 105.0, 3000.0, 3000.0),
		// 別レイヤ（この伏図には映らない）。
		makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{910.0, 0.0}, 120.0, 6000.0, 6000.0),
	};

	const std::vector<TagCommand> tags =
		buildPlanTagCommands(members, makeViewport({"1-横架材天端", "共通"}));
	CHECK(tags.size() == 2);
	if (tags.size() < 2)
		return;

	// 1 本目: 東西材。軸中央 (1000, 0) から上へ 幅/2 = 60（＝上辺の中央）。逃がす向きは上。
	CHECK(tags[0].memberIndex == 0);
	CHECK(near(tags[0].position.x, 1000.0, 1e-9));
	CHECK(near(tags[0].position.y, 60.0, 1e-9));
	CHECK(near(tags[0].offset.x, 0.0, 1e-9));
	CHECK(near(tags[0].offset.y, 1.0, 1e-9));
	CHECK(near(tags[0].angle, 0.0, 1e-9));

	// 2 本目: 南北材。軸中央 (500, 910) から左へ 幅/2 = 52.5（＝左辺の中央）。逃がす向きは左。
	CHECK(tags[1].memberIndex == 1);
	CHECK(near(tags[1].position.x, 500.0 - 52.5, 1e-9));
	CHECK(near(tags[1].position.y, 910.0, 1e-9));
	CHECK(near(tags[1].offset.x, -1.0, 1e-9));
	CHECK(near(tags[1].offset.y, 0.0, 1e-9));
	CHECK(near(tags[1].angle, 90.0, 1e-9));
}

TEST(SectionTagsOnlyCoverMembersOnTheCutPlane)
{
	const std::vector<MemberCommand> members = {
		// 切断面（X = 1000）に乗る南北材。断面には立面として写る。
		makeMember("1-横架材天端", Vec2{1000.0, 0.0}, Vec2{1000.0, 3640.0}, 120.0, 3000.0, 3000.0),
		// 同じ通りだが東西に走る材（切断面に直交して交わるだけ）。
		makeMember("1-横架材天端", Vec2{0.0, 910.0}, Vec2{2000.0, 910.0}, 120.0, 3000.0, 3000.0),
		// 別の通り（X = 2000）の南北材。
		makeMember("1-横架材天端", Vec2{2000.0, 0.0}, Vec2{2000.0, 3640.0}, 120.0, 3000.0, 3000.0),
		// 切断面に乗る傾斜材（登り梁）。角度が天端線の勾配になる。
		makeMember("2-登り梁", Vec2{1000.0, 0.0}, Vec2{1000.0, 1000.0}, 120.0, 6000.0, 7000.0),
	};

	const std::vector<TagCommand> tags =
		buildSectionTagCommands(members, makeSection(SectionDirection::X, 1000.0));
	CHECK(tags.size() == 2);
	if (tags.size() < 2)
		return;

	// 1 本目: 水平材。注釈座標は (Y の中点 − 横原点, 天端 Z)。逃がす向きは真上。
	// 横原点は断面線の終点（この試験用の断面では Y=10000）。
	CHECK(tags[0].memberIndex == 0);
	CHECK(near(tags[0].position.x, 1820.0 - 10000.0, 1e-9));
	CHECK(near(tags[0].position.y, 3000.0, 1e-9));
	CHECK(near(tags[0].offset.x, 0.0, 1e-9));
	CHECK(near(tags[0].offset.y, 1.0, 1e-9));
	CHECK(near(tags[0].angle, 0.0, 1e-9));

	// 2 本目: 傾斜材。1000 進んで 1000 上がるので立面では 45 度。逃がす向きは天端線の
	// 法線のうち上を向く側＝(-1, 1)/√2。
	CHECK(tags[1].memberIndex == 3);
	CHECK(near(tags[1].position.x, 500.0 - 10000.0, 1e-9));
	CHECK(near(tags[1].position.y, 6500.0, 1e-9));
	CHECK(near(tags[1].offset.x, -std::sqrt(0.5), 1e-9));
	CHECK(near(tags[1].offset.y, std::sqrt(0.5), 1e-9));
	CHECK(near(tags[1].angle, 45.0, 1e-9));
}

TEST(UpwardNormalAlwaysPointsUp)
{
	// 水平な天端線は真上。向きを反転しても同じ側を返す（部材の始終端の取り方に依らない）。
	const Vec2 east = upwardNormal(1000.0, 0.0);
	CHECK(near(east.x, 0.0, 1e-9) && near(east.y, 1.0, 1e-9));
	const Vec2 west = upwardNormal(-1000.0, 0.0);
	CHECK(near(west.x, 0.0, 1e-9) && near(west.y, 1.0, 1e-9));

	// 登り勾配は法線が棟側へ倒れる（上向きは保つ）。
	const Vec2 up = upwardNormal(1000.0, 1000.0);
	CHECK(near(up.x, -std::sqrt(0.5), 1e-9) && near(up.y, std::sqrt(0.5), 1e-9));

	// 長さ 0 は既定（真上）。
	const Vec2 degenerate = upwardNormal(0.0, 0.0);
	CHECK(near(degenerate.x, 0.0, 1e-9) && near(degenerate.y, 1.0, 1e-9));
}

TEST(SectionTagsFollowTheViewDirection)
{
	// Y通り（定 Y）は東西に走る材が切断面に乗る。注釈座標の x は材の X −横原点
	// （この試験用の断面は終点が X=10000）。
	const std::vector<MemberCommand> members = {
		makeMember("1-横架材天端", Vec2{0.0, 500.0}, Vec2{3640.0, 500.0}, 120.0, 3000.0, 3000.0),
		makeMember("1-横架材天端", Vec2{910.0, 0.0}, Vec2{910.0, 2000.0}, 120.0, 3000.0, 3000.0),
	};

	const std::vector<TagCommand> tags =
		buildSectionTagCommands(members, makeSection(SectionDirection::Y, 500.0));
	CHECK(tags.size() == 1);
	if (tags.empty())
		return;
	CHECK(tags[0].memberIndex == 0);
	CHECK(near(tags[0].position.x, 1820.0 - 10000.0, 1e-9));
	CHECK(near(tags[0].position.y, 3000.0, 1e-9));
}

TEST(AttachedTagsAreValidOnRealFixtures)
{
	// 実フィクスチャを通して、伏図・軸組図の両方にタグが載り、命令セットが検証を
	// 通ること（memberIndex が members の範囲内・スタイル名が非空）を確かめる。
	std::size_t withPlanTags = 0;
	std::size_t withSectionTags = 0;
	// 命令セットは 1 プロセス 1 回だけ組み立てて共有する（tests/Fixtures.h の
	// fixtureDocument）。ここは読むだけなので参照のまま使える。
	forEachFixtureDocument(
		[&](const std::string&, const Document& document)
		{
			CHECK(core::validateDocument(document));

			for (const auto& sheet : document.sheets)
			{
				if (!sheet.viewport.tags.empty())
					++withPlanTags;
				for (const TagCommand& tag : sheet.viewport.tags)
					CHECK(tag.memberIndex < document.members.size());
			}
			for (const auto& section : document.sections)
			{
				if (!section.viewport.tags.empty())
					++withSectionTags;
				for (const TagCommand& tag : section.viewport.tags)
				{
					CHECK(tag.memberIndex < document.members.size());
					// 断面に載る材はその切断面に乗る（＝通りに沿って走る）はず。
					const MemberCommand& member = document.members[tag.memberIndex];
					const double along = section.direction == SectionDirection::X
											 ? std::abs(member.end.y - member.start.y)
											 : std::abs(member.end.x - member.start.x);
					const double across = section.direction == SectionDirection::X
											  ? std::abs(member.end.x - member.start.x)
											  : std::abs(member.end.y - member.start.y);
					CHECK(across < along);
				}
			}
		});
	// どのフィクスチャにも横架材と伏図があるので、伏図のタグは必ず出る。
	CHECK(withPlanTags > 0);
	CHECK(withSectionTags > 0);
}

TEST(TagCommandsAreDeterministic)
{
	// 同じ入力から 2 度割り当てても同じ並び・同じ値（CLAUDE.md「決定性を守る」）。
	// 原本は共有のキャッシュ（fixtureDocument）から借り、**書き換えるほうだけ**自分の
	// コピーにする。原本を書き換えると同じフィクスチャを見る他のケースに漏れる。
	forEachFixtureDocument(
		[&](const std::string&, const Document& a)
		{
			Document b = a;
			// もう一度割り当てても結果が変わらない（べき等かつ決定的）。
			attachTagCommands(b);
			CHECK(a.sheets.size() == b.sheets.size());
			for (std::size_t i = 0; i < a.sheets.size() && i < b.sheets.size(); ++i)
			{
				const std::vector<TagCommand>& lhs = a.sheets[i].viewport.tags;
				const std::vector<TagCommand>& rhs = b.sheets[i].viewport.tags;
				CHECK(lhs.size() == rhs.size());
				for (std::size_t k = 0; k < lhs.size() && k < rhs.size(); ++k)
				{
					CHECK(lhs[k].memberIndex == rhs[k].memberIndex);
					CHECK(near(lhs[k].position.x, rhs[k].position.x, 1e-9));
					CHECK(near(lhs[k].position.y, rhs[k].position.y, 1e-9));
					CHECK(near(lhs[k].angle, rhs[k].angle, 1e-9));
				}
			}
			CHECK(a.sections.size() == b.sections.size());
			for (std::size_t i = 0; i < a.sections.size() && i < b.sections.size(); ++i)
			{
				const std::vector<TagCommand>& lhs = a.sections[i].viewport.tags;
				const std::vector<TagCommand>& rhs = b.sections[i].viewport.tags;
				CHECK(lhs.size() == rhs.size());
				for (std::size_t k = 0; k < lhs.size() && k < rhs.size(); ++k)
				{
					CHECK(lhs[k].memberIndex == rhs[k].memberIndex);
					CHECK(near(lhs[k].position.x, rhs[k].position.x, 1e-9));
					CHECK(near(lhs[k].position.y, rhs[k].position.y, 1e-9));
				}
			}
		});
}

// ---------------------------------------------------------------------------
// 高さの注記（memberLevelNote）
// ---------------------------------------------------------------------------

TEST(SignedMillimetresGroupThousands)
{
	// 符号は必ず付け（0 は ±）、1,000 以上は 3 桁ごとにコンマ（ご要望）。
	CHECK_EQ(core::signedMillimetreText(-872), std::string("-872"));
	CHECK_EQ(core::signedMillimetreText(40), std::string("+40"));
	CHECK_EQ(core::signedMillimetreText(0), std::string("±0"));
	CHECK_EQ(core::signedMillimetreText(1000), std::string("+1,000"));
	CHECK_EQ(core::signedMillimetreText(-1234567), std::string("-1,234,567"));
	CHECK_EQ(core::planLevelHeightText(2000, 3571, false), std::string("FL-1,571"));
}

namespace
{
	// 2 階建て相当（2FL=3571・横架材天端 3531 / RFL=6374）。
	std::vector<StoryInfo> noteStories()
	{
		return {StoryInfo{1, 612.0, -40.0, false, "1FL"}, StoryInfo{2, 3571.0, -40.0, false, "2FL"},
				StoryInfo{3, 6374.0, 0.0, true, "RFL"}};
	}

	MemberCommand noteMember(const std::string& layer, double start, double end)
	{
		MemberCommand member;
		member.layer = layer;
		member.elevation = start;
		member.endElevation = end;
		return member;
	}
} // namespace

TEST(LevelNoteMeasuresFromTheStoreyFl)
{
	const std::vector<StoryInfo> stories = noteStories();
	// 各階の標準の横架材の高さ（parse/PlanLevel の standardBeamHeights）。
	const std::vector<long long> standard = {572, 3531, 6374};
	// 標準の横架材の高さ（2FL−40）と違う水平な材だけ、階名付きで FL から。
	CHECK_EQ(memberLevelNote(noteMember("2-横架材天端(FL-872)", 2699.0, 2699.0), stories, standard),
			 std::string("(2FL -872)"));
	CHECK(memberLevelNote(noteMember("2-横架材天端", 3531.0, 3531.0), stories, standard).empty());
	CHECK_EQ(memberLevelNote(noteMember("2-横架材天端", 3571.0, 3571.0), stories, standard),
			 std::string("(2FL ±0)"));
	CHECK_EQ(memberLevelNote(noteMember("1-横架材天端", 1612.4, 1612.4), stories, standard),
			 std::string("(1FL +1,000)"));
	// 標準は推した値（横架材天端）ではなく実在する高さ。推した値ちょうどの材でも、標準の
	// 高さと違えば添える。
	CHECK_EQ(memberLevelNote(noteMember("1-横架材天端", 572.0, 572.0), stories, {612, 3531, 6374}),
			 std::string("(1FL -40)"));
	// 標準を渡さない階は推した値で比べる。
	CHECK(memberLevelNote(noteMember("2-横架材天端", 3531.0, 3531.0), stories, {}).empty());
	// 最上階の標準は軒高（RFL そのもの）。母屋は軒高より上なので必ず添える。最上階は
	// "RFL" ではなく "軒高" と書く（RFL は図面で使わない）。
	CHECK(memberLevelNote(noteMember("R-軒高", 6374.0, 6374.0), stories, standard).empty());
	CHECK_EQ(memberLevelNote(noteMember("R-母屋", 6738.0, 6738.0), stories, standard),
			 std::string("(軒高 +364)"));
	// 傾斜材は低い端〜高い端（向きに依らない）。
	CHECK_EQ(memberLevelNote(noteMember("2-登り梁", 3531.0, 2699.0), stories, standard),
			 std::string("(2FL -872~-40)"));
	// 隅木・谷木は添えない。階を特定できない材も添えない。
	MemberCommand hip = noteMember("2-横架材天端", 3283.0, 4524.0);
	hip.hipOrValley = true;
	CHECK(memberLevelNote(hip, stories, standard).empty());
	CHECK(memberLevelNote(noteMember("共通", 0.0, 0.0), stories, standard).empty());
	CHECK(memberLevelNote(noteMember("3-横架材天端", 100.0, 100.0), stories, standard).empty());
	// 二重引用符を含む階名は番号で呼ぶ（注記はタグの式に "…" で囲んで埋め込むので、
	// 引用符が混ざると式全体が評価されなくなる）。名前が無い階も番号で呼ぶ。
	std::vector<StoryInfo> quoted = stories;
	quoted[1].name = "2\"FL";
	CHECK_EQ(memberLevelNote(noteMember("2-横架材天端", 2699.0, 2699.0), quoted, standard),
			 std::string("(2FL -872)"));
	quoted[1].name.clear();
	CHECK_EQ(memberLevelNote(noteMember("2-横架材天端", 2699.0, 2699.0), quoted, standard),
			 std::string("(2FL -872)"));
}

TEST(LevelNoteCarriesTheLinkedDatum)
{
	// 注記は描画側で部材の高さに連動させるので、基準の名前（最上階は軒高）を持つ。
	// 傾斜材も同じ（低い端〜高い端を式で読む）。
	const std::vector<StoryInfo> stories = noteStories();
	const std::vector<long long> standard = {572, 3531, 6374};
	const auto flat =
		memberLevelNoteParts(noteMember("2-横架材天端(FL-872)", 2699.0, 2699.0), stories, standard);
	CHECK_EQ(flat.text, std::string("(2FL -872)"));
	CHECK_EQ(flat.datum, std::string("2FL"));
	CHECK_EQ(memberLevelNoteParts(noteMember("R-母屋", 6738.0, 6738.0), stories, standard).datum,
			 std::string("軒高"));
	const auto sloped =
		memberLevelNoteParts(noteMember("2-登り梁", 3531.0, 2699.0), stories, standard);
	CHECK_EQ(sloped.text, std::string("(2FL -872~-40)"));
	CHECK_EQ(sloped.datum, std::string("2FL"));
	// 注記を添えない材は基準も持たない。
	const auto none =
		memberLevelNoteParts(noteMember("2-横架材天端", 3531.0, 3531.0), stories, standard);
	CHECK(none.text.empty());
	CHECK(none.datum.empty());
	// 引用符を含む階名は番号で呼ぶ（基準名も同じ）。
	std::vector<StoryInfo> quoted = stories;
	quoted[1].name = "2\"FL";
	CHECK_EQ(
		memberLevelNoteParts(noteMember("2-横架材天端", 2699.0, 2699.0), quoted, standard).datum,
		std::string("2FL"));
}

TEST(FixtureTagsCarryLevelNotes)
{
	// スキップフロア: GL+2699 の横架材（2FL−872）には注記があり、標準（2FL−40）には無い。
	// 実フィクスチャの隅木・谷木（傾いた材はすべてそれ）には添えない。
	for (const char* name : {"スキップフロア_サンプル.ifc", "サンプル1 (住木邸新築工事).ifc"})
	{
		const Document& document = HomeskzIfcTests::fixtureDocument(name);
		bool skipNote = false;
		for (const core::SheetCommand& sheet : document.sheets)
		{
			for (const TagCommand& tag : sheet.viewport.tags)
			{
				const MemberCommand& member = document.members[tag.memberIndex];
				if (member.hipOrValley)
					CHECK(tag.note.empty());
				if (member.layer == "2-横架材天端(FL-872)")
				{
					CHECK_EQ(tag.note, std::string("(2FL -872)"));
					CHECK_EQ(tag.noteDatum, std::string("2FL"));
					skipNote = true;
				}
				if (member.layer == "2-横架材天端" && member.elevation == member.endElevation &&
					std::llround(member.elevation) == 3531)
					CHECK(tag.note.empty());
			}
		}
		if (std::string(name) == "スキップフロア_サンプル.ifc")
			CHECK(skipNote);
	}
	// グレー本モデルプラン1 は横架材が FL ちょうどで、推した横架材天端（FL−100 など）と
	// ずれる。標準は実在する高さなので、床伏図の梁には注記が付かない（付くのは母屋だけ）。
	const Document& grey = HomeskzIfcTests::fixtureDocument("グレー本モデルプラン1【3階】.ifc");
	for (const core::SheetCommand& sheet : grey.sheets)
	{
		if (sheet.kind != core::PlanKind::Framing)
			continue;
		for (const TagCommand& tag : sheet.viewport.tags)
			CHECK(tag.note.empty());
	}
	// サンプル1 の傾いた材はすべて IFC 名で隅木・谷木と分かる（クラスは床梁・母屋に推定される）。
	const Document& sample = HomeskzIfcTests::fixtureDocument("サンプル1 (住木邸新築工事).ifc");
	std::size_t hips = 0;
	for (const MemberCommand& member : sample.members)
	{
		if (std::abs(member.elevation - member.endElevation) > 1.0)
		{
			CHECK(member.hipOrValley);
			++hips;
		}
	}
	CHECK(hips > 0);
}

TEST_MAIN();
