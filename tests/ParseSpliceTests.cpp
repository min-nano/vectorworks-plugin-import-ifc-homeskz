//
//	ParseSpliceTests.cpp
//
//	継手解析（src/parse/Splice）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。**期待値は手書きで持
//	つ**。
//
//	検証項目（docs/DEV-NOTES.md M33）: 同一直線上で材の端が突き付く箇所に 1 つだけ置くこと・
//	**直交材を挟んで材の端が離れている箇所には置かない**こと（端部オフセットを戻した材の端で
//	見る）・重なる材／直交する材／レイヤ違い／Z 分離の除外・基準点（材の端の一致点）・
//	**向き（+X が女木。土台は M12 アンカーボルトの反対側、それ以外は支点＝下階の柱・小屋束・
//	床束の側。決まらなければ (−90°, 90°] に正規化した材軸）**・高さ（2 本の端の offset の
//	大きい方）・設定（シンボル名・取り込まない）・並び順に依存しない決定性・実フィクスチャの通し。
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

using HomeskzIfcImport::core::ColumnCommand;
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
using HomeskzIfcImport::parse::CLASS_OOBIKI;
using HomeskzIfcImport::parse::SpliceCues;
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

	// 梁（土台以外）の命令。天端 3000 / せい 180 ＝下端 2820。
	constexpr double kBeamTop = 3000.0;
	constexpr double kBeamBottom = 2820.0;
	MemberCommand beam(Vec2 start, Vec2 end, const char* drawClass = "04構造-02木造-04梁桁-03床梁")
	{
		MemberCommand command = member("2-横架材天端", start, end, kBeamTop);
		command.drawClass = drawClass;
		return command;
	}

	// 柱命令（105 角）。材が実際に占める範囲は [bottom, top]。
	ColumnCommand column(Vec2 position, double bottom, double top)
	{
		ColumnCommand command;
		command.layer = "1to2-柱";
		command.memberId = "x";
		command.position = position;
		command.width = 105.0;
		command.depth = 105.0;
		command.elevation = bottom;
		command.height = top - bottom;
		return command;
	}

	// x 軸上の 2 本（0〜2000 と 2000〜4000）の継手 1 つの角度。
	double spliceAngle(const std::vector<MemberCommand>& members, const SpliceCues& cues)
	{
		const std::vector<SymbolCommand> splices = buildSpliceCommands(members, cues);
		return splices.size() == 1 ? splices[0].angle : 999.0;
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
	const std::vector<SymbolCommand> renamed = buildSpliceCommands(members, SpliceCues{}, options);
	CHECK_EQ(renamed.size(), std::size_t{1});
	if (!renamed.empty())
		CHECK_EQ(renamed[0].symbol, std::string("継手_腰掛け鎌"));

	options.setEnabled(SymbolRole::Splice, false);
	CHECK(buildSpliceCommands(members, SpliceCues{}, options).empty());
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
			}
		});
}

TEST(splice_dodai_female_is_opposite_the_m12_anchor)
{
	// 土台: M12 アンカーボルトが付く側が男木。女木はその反対で、シンボルの +X が女木を向く。
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
		member("1-横架材天端", Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0}),
	};
	SpliceCues left;
	left.anchorsM12 = {Vec2{1850.0, 0.0}, Vec2{3000.0, 0.0}}; // 左の 150mm が近い → 左が男木
	CHECK(near(spliceAngle(members, left), 0.0));			  // 女木は右（+X）

	SpliceCues right;
	right.anchorsM12 = {Vec2{2150.0, 0.0}, Vec2{500.0, 0.0}};  // 右が男木
	CHECK(near(std::abs(spliceAngle(members, right)), 180.0)); // 女木は左（−X）

	// 材の幅の外・継手の真上のアンカーボルトは手掛かりにしない → 正規化した材軸。
	SpliceCues off;
	off.anchorsM12 = {Vec2{1850.0, 300.0}, Vec2{2000.0, 0.0}};
	CHECK(near(spliceAngle(members, off), 0.0));
	SpliceCues offRight;
	offRight.anchorsM12 = {Vec2{2150.0, 300.0}};
	CHECK(near(spliceAngle(members, offRight), 0.0));

	// 土台は柱では決めない（柱を渡しても向きは M12 だけで決まる）。
	SpliceCues columnsOnly;
	columnsOnly.columns = {column(Vec2{2150.0, 0.0}, 0.0, 245.0)};
	CHECK(near(spliceAngle(members, columnsOnly), 0.0));
}

TEST(splice_beam_female_is_on_the_supported_side)
{
	// 梁: 下階の柱（上端が梁の下端）がある側が女木。
	const std::vector<MemberCommand> members = {beam(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
												beam(Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0})};
	SpliceCues supportLeft;
	supportLeft.columns = {column(Vec2{1850.0, 0.0}, 0.0, kBeamBottom),
						   column(Vec2{4000.0, 0.0}, 0.0, kBeamBottom)};
	CHECK(near(std::abs(spliceAngle(members, supportLeft)), 180.0)); // 女木は左

	SpliceCues supportRight;
	supportRight.columns = {column(Vec2{2150.0, 0.0}, 0.0, kBeamBottom),
							column(Vec2{0.0, 0.0}, 0.0, kBeamBottom)};
	CHECK(near(spliceAngle(members, supportRight), 0.0)); // 女木は右

	// 梁を越えて伸びる通し柱・梁の上に立つ上階の柱は支点ではない（向きは正規化した材軸）。
	// 左側にだけ置いて、支点と誤認すれば 180 度になることを確かめる形にする。
	SpliceCues notSupports;
	notSupports.columns = {column(Vec2{1850.0, 0.0}, 0.0, 5000.0),
						   column(Vec2{1700.0, 0.0}, kBeamTop, 5700.0)};
	CHECK(near(spliceAngle(members, notSupports), 0.0));

	// 両側が同じ距離なら決めない。
	SpliceCues tie;
	tie.columns = {column(Vec2{1850.0, 0.0}, 0.0, kBeamBottom),
				   column(Vec2{2150.0, 0.0}, 0.0, kBeamBottom)};
	CHECK(near(spliceAngle(members, tie), 0.0));
}

TEST(splice_ohbiki_uses_floor_posts_and_beams_do_not)
{
	// 大引: 床束も支点。梁は床束を見ない（平面で重なっても 2 階の梁は床束に載らない）。
	const std::vector<MemberCommand> ohbiki = {
		beam(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}, CLASS_OOBIKI),
		beam(Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0}, CLASS_OOBIKI)};
	SpliceCues posts;
	posts.floorPosts = {Vec2{1800.0, 0.0}, Vec2{2700.0, 0.0}};
	CHECK(near(std::abs(spliceAngle(ohbiki, posts)), 180.0)); // 女木は床束の近い左

	const std::vector<MemberCommand> beams = {beam(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
											  beam(Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0})};
	CHECK(near(spliceAngle(beams, posts), 0.0)); // 手掛かり無し → 正規化した材軸
}

TEST(splice_orientation_follows_the_y_axis_too)
{
	// Y 方向の梁: 支点が上（+Y）側なら +X は +Y（90 度）を向く。
	const std::vector<MemberCommand> members = {beam(Vec2{0.0, 0.0}, Vec2{0.0, 2000.0}),
												beam(Vec2{0.0, 2000.0}, Vec2{0.0, 4000.0})};
	SpliceCues up;
	up.columns = {column(Vec2{0.0, 2150.0}, 0.0, kBeamBottom)};
	const std::vector<SymbolCommand> splices = buildSpliceCommands(members, up);
	CHECK_EQ(splices.size(), std::size_t{1});
	if (!splices.empty())
		CHECK(near(splices[0].angle, 90.0));
}

TEST(splice_short_span_uses_the_support_distance_seen_elsewhere)
{
	// 短いスパン（継手をはさむ 2 支点の間隔 ≤ 500）は「近い方」ではなく、スパンの長い継手で
	// 確かめた「継手から支点までの距離」と同じ距離にある側を女木とする。
	// 継手 1（x=2000）: 長いスパン。左の支点 1800（距離 200）・右の支点 3000（距離 1000）
	//                   → 女木は左、距離 200 が代表値になる。
	// 継手 2（x=10000）: 短いスパン。左の支点 9880（距離 120）・右の支点 10200（距離 200）
	//                   → 近いのは左だが、代表値 200 と同じ距離の右を女木とする。
	std::vector<MemberCommand> members = {
		beam(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}),
		beam(Vec2{2000.0, 0.0}, Vec2{4000.0, 0.0}),
		beam(Vec2{8000.0, 0.0}, Vec2{10000.0, 0.0}),
		beam(Vec2{10000.0, 0.0}, Vec2{12000.0, 0.0}),
	};
	SpliceCues cues;
	cues.columns = {
		column(Vec2{1800.0, 0.0}, 0.0, kBeamBottom), column(Vec2{3000.0, 0.0}, 0.0, kBeamBottom),
		column(Vec2{9880.0, 0.0}, 0.0, kBeamBottom), column(Vec2{10200.0, 0.0}, 0.0, kBeamBottom)};
	const std::vector<SymbolCommand> splices = buildSpliceCommands(members, cues);
	CHECK_EQ(splices.size(), std::size_t{2});
	if (splices.size() != 2)
		return;
	CHECK(near(std::abs(splices[0].angle), 180.0)); // 継手 1: 女木は左
	CHECK(near(splices[1].angle, 0.0)); // 継手 2: 女木は右（代表値 200 の側）

	// 代表値が無い（長いスパンの継手が無い）ときは従来どおり近い方。
	const std::vector<MemberCommand> onlyShort = {members[2], members[3]};
	const std::vector<SymbolCommand> alone = buildSpliceCommands(onlyShort, cues);
	CHECK_EQ(alone.size(), std::size_t{1});
	if (!alone.empty())
		CHECK(near(std::abs(alone[0].angle), 180.0));

	// 代表値は**レイヤごと**。別レイヤの継手の距離は、そのレイヤに代表値があれば使わない。
	std::vector<MemberCommand> otherLayer = members;
	otherLayer[2].layer = "3-横架材天端";
	otherLayer[3].layer = "3-横架材天端";
	// 3 階にも長いスパンの継手（距離 120 の支点）を足す → 3 階の代表値は 120 → 左が女木。
	MemberCommand longLeft = beam(Vec2{20000.0, 0.0}, Vec2{22000.0, 0.0});
	MemberCommand longRight = beam(Vec2{22000.0, 0.0}, Vec2{24000.0, 0.0});
	longLeft.layer = "3-横架材天端";
	longRight.layer = "3-横架材天端";
	otherLayer.push_back(longLeft);
	otherLayer.push_back(longRight);
	SpliceCues layered = cues;
	layered.columns.push_back(column(Vec2{21880.0, 0.0}, 0.0, kBeamBottom));
	layered.columns.push_back(column(Vec2{23000.0, 0.0}, 0.0, kBeamBottom));
	const std::vector<SymbolCommand> perLayer = buildSpliceCommands(otherLayer, layered);
	CHECK_EQ(perLayer.size(), std::size_t{3});
	if (perLayer.size() == 3)
		CHECK(near(std::abs(perLayer[1].angle), 180.0)); // 3 階の短いスパン: 代表値 120 → 左
}

TEST(splice_fixtures_orient_most_splices_from_the_cues)
{
	// 実データ: 継手の向きはおおむね手掛かりから決まる（0 件なら向きの規則が何も効いて
	// いない）。どの継手も材軸に沿う（角度は材軸と平行）。
	std::size_t total = 0;
	std::size_t oriented = 0;
	forEachFixtureDocument(
		[&](const std::string&, const Document& document)
		{
			for (const SymbolCommand& splice : document.splices)
			{
				++total;
				// 正規化の範囲 (−90, 90] の外を向いていれば、手掛かりで決まったもの。
				// 範囲の中でも手掛かりで決まったものはあるので、これは下限の数え方。
				if (splice.angle > 90.0 + 1e-9 || splice.angle <= -90.0 + 1e-9)
					++oriented;
			}
		});
	CHECK(total > 0);
	CHECK(oriented > 0);
}

TEST_MAIN();
