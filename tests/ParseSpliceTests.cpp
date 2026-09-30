//
//	ParseSpliceTests.cpp
//
//	継手解析（src/parse/Splice）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。**期待値は手書きで持
//	つ**。
//
//	検証項目（docs/DEV-NOTES.md M33）: 同一直線上で材の端が突き付く箇所に 1 つだけ置くこと・
//	**直交材を挟んで材の端が離れている箇所には置かない**こと（端部オフセットを戻した材の端で
//	見る）・重なる材／直交する材／レイヤ違い／Z 分離の除外・基準点（材の端の一致点）と
//	回転角（(−90°, 90°] に正規化）・高さ（2 本の端の offset の大きい方）・設定（シンボル名・
//	取り込まない）・並び順に依存しない決定性・実フィクスチャの通し。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "core/ImportOptions.h"
#include "parse/Splice.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using HomeskzIfcImport::core::defaultSymbolName;
using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::ImportOptions;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::memberDrawnEnd;
using HomeskzIfcImport::core::memberDrawnStart;
using HomeskzIfcImport::core::SymbolCommand;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildSpliceCommands;
using HomeskzIfcImport::parse::CLASS_DODAI;
using HomeskzIfcTests::forEachFixtureDocument;
using HomeskzIfcTests::near;

namespace
{
	// 既定のシンボル名。**唯一の定義は役割の表**（core::symbolRoles()）から引く。
	const std::string kSymbolSplice = defaultSymbolName(SymbolRole::Splice);

	// 横架材命令。既定は幅 105 / せい 180 / 天端 425 の水平材で、端部オフセット・バウンド
	// offset は 0。
	MemberCommand member(const std::string& layer, Vec2 start, Vec2 end, double elevation = 425.0,
						 double height = 180.0)
	{
		MemberCommand command;
		command.layer = layer;
		command.memberId = "x";
		command.drawClass = CLASS_DODAI;
		command.start = start;
		command.end = end;
		command.width = 105.0;
		command.height = height;
		command.elevation = elevation;
		command.endElevation = elevation;
		command.startBound.level = "横架材天端";
		command.endBound.level = "横架材天端";
		return command;
	}
} // namespace

TEST(splice_collinear_butt_joint_places_one_symbol)
{
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
		member("1-横架材天端", Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0}),
	};
	const std::vector<SymbolCommand> splices = buildSpliceCommands(members);
	CHECK_EQ(splices.size(), std::size_t{1});
	if (splices.size() != 1)
		return;
	CHECK_EQ(splices[0].symbol, kSymbolSplice);
	CHECK_EQ(splices[0].layer, std::string("1-横架材天端"));
	CHECK(near(splices[0].position.x, 2000.0));
	CHECK(near(splices[0].position.y, 0.0));
	CHECK(near(splices[0].angle, 0.0));
	CHECK(near(splices[0].zOffset, 0.0));
}

TEST(splice_angle_is_normalized_regardless_of_member_direction)
{
	// Y 方向の材を −Y 向きに描いても、向きは +Y（90 度）にそろう。
	const std::vector<MemberCommand> members = {
		member("2-横架材天端", Vec2{500.0, 3000.0}, Vec2{500.0, 1000.0}),
		member("2-横架材天端", Vec2{500.0, 1000.0}, Vec2{500.0, -1000.0}),
	};
	const std::vector<SymbolCommand> splices = buildSpliceCommands(members);
	CHECK_EQ(splices.size(), std::size_t{1});
	if (splices.size() != 1)
		return;
	CHECK(near(splices[0].position.x, 500.0));
	CHECK(near(splices[0].position.y, 1000.0));
	CHECK(near(splices[0].angle, 90.0));

	// X 方向の材を −X 向きに描いても 0 度（180 度にはならない）。
	const std::vector<MemberCommand> reversed = {
		member("2-横架材天端", Vec2{4000.0, 0.0}, Vec2{2000.0, 0.0}),
		member("2-横架材天端", Vec2{2000.0, 0.0}, Vec2{0.0, 0.0}),
	};
	const std::vector<SymbolCommand> xs = buildSpliceCommands(reversed);
	CHECK_EQ(xs.size(), std::size_t{1});
	if (!xs.empty())
		CHECK(near(xs[0].angle, 0.0));
}

TEST(splice_ends_separated_by_a_through_member_are_not_spliced)
{
	// 直交材が間を通り抜ける箇所: 命令の端点は相手の芯線（x=2000）で一致するが、材の端は
	// 端部オフセット（−52.5）でその材の面まで戻っているので 105mm 離れる → 継手ではない。
	MemberCommand left = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{2000.0, 0.0});
	left.endOffset = -52.5;
	MemberCommand right = member("1-横架材天端", Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0});
	right.startOffset = -52.5;
	const std::vector<MemberCommand> members = {
		left, right, member("1-横架材天端", Vec2{2000.0, -1000.0}, Vec2{2000.0, 1000.0})};
	CHECK(buildSpliceCommands(members).empty());
}

TEST(splice_uses_the_drawn_end_not_the_command_end)
{
	// 命令の端点が柱芯へ送られていても（端部オフセットで材の端を戻している）、材の端どうしが
	// 一致すれば継手。置く位置も材の端。
	MemberCommand left = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{2100.0, 0.0});
	left.endOffset = -100.0; // 材の端は x=2000
	MemberCommand right = member("1-横架材天端", Vec2{2100.0, 0.0}, Vec2{4000.0, 0.0});
	right.startOffset = 100.0; // 材の端は x=2000
	const std::vector<SymbolCommand> splices = buildSpliceCommands({left, right});
	CHECK_EQ(splices.size(), std::size_t{1});
	if (!splices.empty())
		CHECK(near(splices[0].position.x, 2000.0));
}

TEST(splice_excludes_overlap_corner_layer_and_z)
{
	const std::string layer = "1-横架材天端";
	// 同じ側へ伸びる（重なる）材。
	CHECK(buildSpliceCommands({member(layer, Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
							   member(layer, Vec2{0.0, 0.0}, Vec2{1000.0, 0.0})})
			  .empty());
	// L 字の出隅（端は一致するが直交）。
	CHECK(buildSpliceCommands({member(layer, Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
							   member(layer, Vec2{2000.0, 0.0}, Vec2{2000.0, 2000.0})})
			  .empty());
	// レイヤ違い。
	CHECK(buildSpliceCommands({member(layer, Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
							   member("2-横架材天端", Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0})})
			  .empty());
	// Z が離れている（天端 425 せい 180 と 天端 1500）。
	CHECK(buildSpliceCommands({member(layer, Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
							   member(layer, Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0}, 1500.0)})
			  .empty());
	// 平面投影長が極小の材は判定から外す。
	CHECK(buildSpliceCommands({member(layer, Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
							   member(layer, Vec2{2000.0, 0.0}, Vec2{2000.5, 0.0})})
			  .empty());
}

TEST(splice_height_takes_the_higher_end_offset)
{
	MemberCommand left = member("R-母屋", Vec2{0.0, 0.0}, Vec2{2000.0, 0.0});
	left.endBound.offset = 30.0;
	MemberCommand right = member("R-母屋", Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0});
	right.startBound.offset = 45.0;
	const std::vector<SymbolCommand> splices = buildSpliceCommands({left, right});
	CHECK_EQ(splices.size(), std::size_t{1});
	if (!splices.empty())
		CHECK(near(splices[0].zOffset, 45.0));
}

TEST(splice_follows_import_options)
{
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
		member("1-横架材天端", Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0}),
	};
	ImportOptions options;
	options.setSymbol(SymbolRole::Splice, "継手_腰掛け鎌");
	const std::vector<SymbolCommand> renamed = buildSpliceCommands(members, options);
	CHECK_EQ(renamed.size(), std::size_t{1});
	if (!renamed.empty())
		CHECK_EQ(renamed[0].symbol, std::string("継手_腰掛け鎌"));

	options.setEnabled(SymbolRole::Splice, false);
	CHECK(buildSpliceCommands(members, options).empty());
}

TEST(splice_result_does_not_depend_on_member_order)
{
	std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
		member("1-横架材天端", Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0}),
		member("1-横架材天端", Vec2{0.0, 1000.0}, Vec2{0.0, 3000.0}),
		member("1-横架材天端", Vec2{0.0, 3000.0}, Vec2{0.0, 5000.0}),
	};
	const auto key = [](const std::vector<SymbolCommand>& splices)
	{
		std::vector<std::vector<double>> keys;
		for (const SymbolCommand& s : splices)
			keys.push_back({s.position.x, s.position.y, s.angle, s.zOffset});
		std::ranges::sort(keys);
		return keys;
	};
	const auto forward = key(buildSpliceCommands(members));
	std::ranges::reverse(members);
	CHECK(forward == key(buildSpliceCommands(members)));
	CHECK_EQ(forward.size(), std::size_t{2});
}

TEST(splice_fixtures_sit_where_two_member_ends_meet)
{
	// 実データ: どのフィクスチャにも継手があり、どの継手も**同じレイヤの 2 本の材の端**が
	// 一致する点にある（直交材を挟んだ突き付けを拾っていれば、そこには材の端が無い）。
	forEachFixtureDocument(
		[&](const std::string&, const Document& document)
		{
			CHECK(!document.splices.empty());
			CHECK(HomeskzIfcImport::core::validateDocument(document));
			for (const SymbolCommand& splice : document.splices)
			{
				CHECK_EQ(splice.symbol, kSymbolSplice);
				std::size_t endsHere = 0;
				for (const MemberCommand& m : document.members)
				{
					if (m.layer != splice.layer)
						continue;
					for (const Vec2& end : {memberDrawnStart(m), memberDrawnEnd(m)})
						if (HomeskzIfcImport::core::length(end - splice.position) <= 1.0)
							++endsHere;
				}
				CHECK(endsHere >= 2);
				CHECK(splice.angle > -90.0 - 1e-9 && splice.angle <= 90.0 + 1e-9);
			}
		});
}

TEST_MAIN();
