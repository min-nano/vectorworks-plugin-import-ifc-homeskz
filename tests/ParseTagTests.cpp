//
//	ParseTagTests.cpp
//
//	断面寸法データタグの解析（src/parse/Tag）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネス（TestFramework.h）で走る
//	（CLAUDE.md「テスト方針」）。Python 版 test_ifc_tag.py が見ていた性質を写しつつ、
//	期待値は手書きする（ROADMAP.md「Python 版出力との比較はしない」）。
//
//	検証項目（ROADMAP.md M13）: 文字角度の (-90, 90] 正規化・タグを寄せる側（上または左）・
//	伏図では表示レイヤに乗る横架材だけが対象で位置が部材の辺の中央になること・軸組図では
//	**切断面に乗る横架材だけ**が対象で位置が断面の注釈空間（切断線に沿った距離, 天端 Z）に
//	なること・memberIndex が members の添字を指すこと・実フィクスチャでの通し（全タグが
//	妥当＝validateDocument を通ること）・並び順に依存しない決定性。実フィクスチャのパスは
//	CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/BuildDocument.h"
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
using HomeskzIfcImport::core::TagPlacement;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::ViewportCommand;
using HomeskzIfcImport::parse::attachTagCommands;
using HomeskzIfcImport::parse::buildPlanTagCommands;
using HomeskzIfcImport::parse::buildSectionTagCommands;
using HomeskzIfcImport::parse::kTagStyle;
using HomeskzIfcImport::parse::sectionAnnotationPoint;
using HomeskzIfcImport::parse::tagAngle;
using HomeskzIfcImport::parse::tagOffsetSide;
using HomeskzIfcImport::parse::upwardNormal;
using HomeskzIfcTests::allFixtures;
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
		section.number = "A";
		section.title = "軸組図";
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
	CHECK(tags[0].style == std::string(kTagStyle));
	CHECK(near(tags[0].position.x, 1000.0, 1e-9));
	CHECK(near(tags[0].position.y, 60.0, 1e-9));
	CHECK(near(tags[0].offset.x, 0.0, 1e-9));
	CHECK(near(tags[0].offset.y, 1.0, 1e-9));
	CHECK(tags[0].placement == TagPlacement::Absolute);
	CHECK(near(tags[0].angle, 0.0, 1e-9));

	// 2 本目: 南北材。軸中央 (500, 910) から左へ 幅/2 = 52.5（＝左辺の中央）。逃がす向きは左。
	CHECK(tags[1].memberIndex == 1);
	CHECK(near(tags[1].position.x, 500.0 - 52.5, 1e-9));
	CHECK(near(tags[1].position.y, 910.0, 1e-9));
	CHECK(near(tags[1].offset.x, -1.0, 1e-9));
	CHECK(near(tags[1].offset.y, 0.0, 1e-9));
	CHECK(near(tags[1].angle, 90.0, 1e-9));
}

TEST(SectionAnnotationPointProjectsToTheView)
{
	// X通り（−X 方向を見る）は画面右が +Y なので、注釈座標の x は材の Y。
	const Vec2 onX = sectionAnnotationPoint(Vec2{1500.0, -2000.0}, 3273.0, SectionDirection::X);
	CHECK(near(onX.x, -2000.0, 1e-9));
	CHECK(near(onX.y, 3273.0, 1e-9));

	// Y通り（+Y 方向を見る）は画面右が +X なので、注釈座標の x は材の X。
	const Vec2 onY = sectionAnnotationPoint(Vec2{1500.0, -2000.0}, 3273.0, SectionDirection::Y);
	CHECK(near(onY.x, 1500.0, 1e-9));
	CHECK(near(onY.y, 3273.0, 1e-9));
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

	// 1 本目: 水平材。注釈座標は (Y の中点, 天端 Z)。逃がす向きは真上。
	CHECK(tags[0].memberIndex == 0);
	CHECK(tags[0].style == std::string(kTagStyle));
	CHECK(near(tags[0].position.x, 1820.0, 1e-9));
	CHECK(near(tags[0].position.y, 3000.0, 1e-9));
	CHECK(near(tags[0].offset.x, 0.0, 1e-9));
	CHECK(near(tags[0].offset.y, 1.0, 1e-9));
	// 断面は原点が分からないので、VW が置いた位置からの相対で決める。基準点は横架材の
	// 挿入点（天端中央線の始端）を同じように投影したもの。
	CHECK(tags[0].placement == TagPlacement::RelativeToAnchor);
	CHECK(near(tags[0].anchor.x, 0.0, 1e-9));
	CHECK(near(tags[0].anchor.y, 3000.0, 1e-9));
	CHECK(near(tags[0].angle, 0.0, 1e-9));

	// 2 本目: 傾斜材。1000 進んで 1000 上がるので立面では 45 度。逃がす向きは天端線の
	// 法線のうち上を向く側＝(-1, 1)/√2。
	CHECK(tags[1].memberIndex == 3);
	CHECK(near(tags[1].position.x, 500.0, 1e-9));
	CHECK(near(tags[1].position.y, 6500.0, 1e-9));
	CHECK(near(tags[1].offset.x, -std::sqrt(0.5), 1e-9));
	CHECK(near(tags[1].offset.y, std::sqrt(0.5), 1e-9));
	// 基準点は始端の投影 (Y=0, 天端 Z=6000)。position との差が「材の始端から辺の中央まで」
	// のモデル上の変位になり、描画側はこの差だけタグを動かす。
	CHECK(near(tags[1].anchor.x, 0.0, 1e-9));
	CHECK(near(tags[1].anchor.y, 6000.0, 1e-9));
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
	// Y通り（定 Y）は東西に走る材が切断面に乗る。注釈座標の x は材の X。
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
	CHECK(near(tags[0].position.x, 1820.0, 1e-9));
	CHECK(near(tags[0].position.y, 3000.0, 1e-9));
}

TEST(AttachedTagsAreValidOnRealFixtures)
{
	// 実フィクスチャを通して、伏図・軸組図の両方にタグが載り、命令セットが検証を
	// 通ること（memberIndex が members の範囲内・スタイル名が非空）を確かめる。
	std::size_t withPlanTags = 0;
	std::size_t withSectionTags = 0;
	for (const std::string& name : allFixtures())
	{
		const Document document = parse::buildDocument(HomeskzIfcTests::fixturePath(name));
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
	}
	// どのフィクスチャにも横架材と伏図があるので、伏図のタグは必ず出る。
	CHECK(withPlanTags > 0);
	CHECK(withSectionTags > 0);
}

TEST(TagCommandsAreDeterministic)
{
	// 同じ入力から 2 度割り当てても同じ並び・同じ値（CLAUDE.md「決定性を守る」）。
	for (const std::string& name : allFixtures())
	{
		Document a = parse::buildDocument(HomeskzIfcTests::fixturePath(name));
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
	}
}

TEST_MAIN();
