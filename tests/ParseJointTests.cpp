//
//	ParseJointTests.cpp
//
//	仕口解析（src/parse/Joint）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	Python 版 test_ifc_joint.py のケースを 1 対 1 で写している（期待値は手書き。
//	docs/DEV-NOTES.md「Python 版出力との比較はしない」）。
//
//	検証項目（docs/DEV-NOTES.md M11）: 端点が相手材の footprint に入るかの判定・平行（継ぎ手・
//	側並び）とレイヤ違いと Z 分離の除外・登り梁だけレイヤ一致を外すこと・柱に受けられる
//	端部・退化した材のスキップ・基準点（梁端の中央上端）と回転角（端部から内側へ）・
//	**高さ（zOffset＝その端部のバウンド offset）**・
//	配置先レイヤ（横架材と同じ）・並び順に依存しない決定性・実フィクスチャの通し。
//	実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Column.h"
#include "parse/Joint.h"
#include "parse/Loader.h"
#include "parse/Member.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::ColumnCommand;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::SymbolCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildColumnCommands;
using HomeskzIfcImport::parse::buildJointCommands;
using HomeskzIfcImport::parse::buildMemberCommands;
using HomeskzIfcImport::parse::CLASS_DODAI;
using HomeskzIfcImport::parse::CLASS_KUDABASHIRA;
using HomeskzIfcImport::parse::CLASS_NOBORIBARI;
using HomeskzIfcImport::parse::ColumnGeom;
using HomeskzIfcImport::parse::columnGeom;
using HomeskzIfcImport::parse::endHasReceiver;
using HomeskzIfcImport::parse::kSymbolJoint;
using HomeskzIfcImport::parse::MemberGeom;
using HomeskzIfcImport::parse::memberGeom;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::pointInColumn;
using HomeskzIfcImport::parse::pointInMember;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// 横架材命令（Python 版 _member）。既定は幅 120 / せい 180 / 天端 425 の水平材。
	// バウンド offset（レベルの絶対 Z から天端 Z までの距離）は既定 0＝レイヤ平面ちょうど。
	// 仕口の高さはこの offset をそのまま写すので、高さを見るテストだけ明示的に入れる。
	MemberCommand member(const std::string& layer, Vec2 start, Vec2 end, double width = 120.0,
						 double height = 180.0, double elevation = 425.0,
						 double endElevation = 425.0, double startOffset = 0.0,
						 double endOffset = 0.0)
	{
		MemberCommand command;
		command.layer = layer;
		command.memberId = "x";
		command.drawClass = CLASS_DODAI;
		command.start = start;
		command.end = end;
		command.width = width;
		command.height = height;
		command.elevation = elevation;
		command.endElevation = endElevation;
		command.startBound.level = "横架材天端";
		command.endBound.level = "横架材天端";
		command.startBound.offset = startOffset;
		command.endBound.offset = endOffset;
		return command;
	}

	// 柱命令（Python 版 _column）。既定は 105 角・下端 245・高さ 2844。
	ColumnCommand column(Vec2 position, double elevation = 245.0, double height = 2844.0,
						 double width = 105.0, double depth = 105.0)
	{
		ColumnCommand command;
		command.layer = "1to2-柱";
		command.memberId = "x";
		command.drawClass = CLASS_KUDABASHIRA;
		command.structuralUse = "4";
		command.position = position;
		command.width = width;
		command.depth = depth;
		command.height = height;
		command.elevation = elevation;
		command.bottomBound.level = "横架材天端";
		command.topBound.level = "横架材天端";
		return command;
	}

	std::vector<MemberGeom> geomsOf(const std::vector<MemberCommand>& members)
	{
		std::vector<MemberGeom> geoms;
		geoms.reserve(members.size());
		for (const MemberCommand& command : members)
			geoms.push_back(memberGeom(command));
		return geoms;
	}

	std::vector<ColumnGeom> columnGeomsOf(const std::vector<ColumnCommand>& columns)
	{
		std::vector<ColumnGeom> geoms;
		geoms.reserve(columns.size());
		for (const ColumnCommand& command : columns)
			geoms.push_back(columnGeom(command));
		return geoms;
	}

	// 仕口の同一性キー（レイヤ＋位置。0.001mm 単位へ丸めて浮動小数の最下位ビット差を吸収）。
	using JointKey = std::tuple<std::string, long long, long long>;

	JointKey keyOf(const SymbolCommand& command)
	{
		return JointKey{command.layer, std::llround(command.position.x * 1000.0),
						std::llround(command.position.y * 1000.0)};
	}

	std::multiset<JointKey> keysOf(const std::vector<SymbolCommand>& commands)
	{
		std::multiset<JointKey> keys;
		for (const SymbolCommand& command : commands)
			keys.insert(keyOf(command));
		return keys;
	}
} // namespace

// --- 端点が相手材の footprint に入るか（Python 版 TestPointInMember）---------

TEST(joint_point_in_member)
{
	// 中心線 x=0..3000・y=0、半幅 60 の相手材。
	const MemberGeom other = memberGeom(member("L", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}));
	CHECK(other.valid);
	CHECK(near(other.halfWidth, 60.0));

	CHECK(pointInMember(Vec2{1500.0, 60.0}, other));   // 天端面ちょうどに載る端点
	CHECK(pointInMember(Vec2{1500.0, 0.0}, other));	   // 内部
	CHECK(pointInMember(Vec2{0.0, 0.0}, other));	   // 相手の端（コーナー）
	CHECK(!pointInMember(Vec2{1500.0, 100.0}, other)); // 半幅＋余裕より遠い
	CHECK(!pointInMember(Vec2{4000.0, 0.0}, other));   // 軸方向に範囲外
}

// --- 受ける材の有無（Python 版 TestEndHasReceiver）---------------------------

TEST(joint_t_junction_stem_end_is_received)
{
	// A＝通し材（X 方向）、B＝A の側面に突き当たる材（Y 方向）。B の始端は A の天端面に
	// 載るので受ける材がある。
	const std::vector<MemberCommand> members = {
		member("L", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("L", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0}),
	};
	CHECK(endHasReceiver(1, Vec2{1500.0, 60.0}, geomsOf(members), members, {}));
}

TEST(joint_through_member_free_ends_are_not_received)
{
	// A の端点は B（A の中間に突き当たる材）に取り付かない＝自由端。
	const std::vector<MemberCommand> members = {
		member("L", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("L", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0}),
	};
	CHECK(!endHasReceiver(0, Vec2{0.0, 0.0}, geomsOf(members), members, {}));
	CHECK(!endHasReceiver(0, Vec2{3000.0, 0.0}, geomsOf(members), members, {}));
}

TEST(joint_parallel_splice_is_not_received)
{
	// 同一直線上の継ぎ手（平行）は受ける材にしない。
	const std::vector<MemberCommand> members = {
		member("L", Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}),
		member("L", Vec2{1000.0, 0.0}, Vec2{2000.0, 0.0}),
	};
	CHECK(!endHasReceiver(0, Vec2{1000.0, 0.0}, geomsOf(members), members, {}));
	CHECK(!endHasReceiver(1, Vec2{1000.0, 0.0}, geomsOf(members), members, {}));
}

TEST(joint_different_layer_is_not_received)
{
	const std::vector<MemberCommand> members = {
		member("L1", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("L2", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0}),
	};
	CHECK(!endHasReceiver(1, Vec2{1500.0, 60.0}, geomsOf(members), members, {}));
}

TEST(joint_separated_z_is_not_received)
{
	// 段差で Z 範囲が離れた相手は受ける材にしない。
	const std::vector<MemberCommand> members = {
		member("L", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("L", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0}, 120.0, 180.0, 2000.0, 2000.0),
	};
	CHECK(!endHasReceiver(1, Vec2{1500.0, 60.0}, geomsOf(members), members, {}));
}

TEST(joint_noboribari_receives_across_layers)
{
	// 登り梁（R-登り梁）の端部が別レイヤの母屋（R-母屋）に取り付く。登り梁はレイヤ一致の
	// 制約を外すので、別レイヤでも受ける材とみなす。
	std::vector<MemberCommand> members = {
		member("R-母屋", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("R-登り梁", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0}),
	};
	members[1].drawClass = CLASS_NOBORIBARI;
	CHECK(endHasReceiver(1, Vec2{1500.0, 60.0}, geomsOf(members), members, {}));
}

TEST(joint_non_noboribari_still_layer_restricted)
{
	// 通常の横架材は別レイヤの相手を受ける材にしない（従来どおり）。
	const std::vector<MemberCommand> members = {
		member("R-母屋", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("R-軒高", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0}),
	};
	CHECK(!endHasReceiver(1, Vec2{1500.0, 60.0}, geomsOf(members), members, {}));
}

TEST(joint_noboribari_parallel_and_z_separation_still_excluded)
{
	// 登り梁でも平行（同一直線上・側並び）／Z 範囲が離れた相手は受ける材にしない。
	std::vector<MemberCommand> parallelCase = {
		member("R-母屋", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("R-登り梁", Vec2{3000.0, 0.0}, Vec2{5000.0, 0.0}),
	};
	parallelCase[1].drawClass = CLASS_NOBORIBARI;
	CHECK(!endHasReceiver(1, Vec2{3000.0, 0.0}, geomsOf(parallelCase), parallelCase, {}));

	std::vector<MemberCommand> zCase = {
		member("R-母屋", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
		member("R-登り梁", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0}, 120.0, 180.0, 4000.0, 4000.0),
	};
	zCase[1].drawClass = CLASS_NOBORIBARI;
	CHECK(!endHasReceiver(1, Vec2{1500.0, 60.0}, geomsOf(zCase), zCase, {}));
}

// --- 柱に受けられる端部（Python 版 TestColumnReceiver）-----------------------

TEST(joint_beam_end_on_column_is_received)
{
	// 梁: 天端 425・背 180 → Z 範囲 [245, 425]。柱: 下端 −2575・高さ 3000 → [−2575, 425]。
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{1500.0, 0.0})};
	const std::vector<ColumnGeom> columns =
		columnGeomsOf({column(Vec2{1500.0, 0.0}, -2575.0, 3000.0)});
	CHECK(endHasReceiver(0, Vec2{1500.0, 0.0}, geomsOf(members), members, columns));
}

TEST(joint_beam_end_on_column_face_is_received)
{
	// 柱の側面（x = 1500 − 52.5）ちょうどに載る梁端も取り付きとみなす。
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{1447.5, 0.0})};
	const std::vector<ColumnGeom> columns =
		columnGeomsOf({column(Vec2{1500.0, 0.0}, -2575.0, 3000.0)});
	CHECK(endHasReceiver(0, Vec2{1447.5, 0.0}, geomsOf(members), members, columns));
}

TEST(joint_beam_end_away_from_column_is_not_received)
{
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{1500.0, 0.0})};
	const std::vector<ColumnGeom> columns =
		columnGeomsOf({column(Vec2{5000.0, 5000.0}, -2575.0, 3000.0)});
	CHECK(!endHasReceiver(0, Vec2{1500.0, 0.0}, geomsOf(members), members, columns));
}

TEST(joint_column_z_separated_is_not_received)
{
	// 柱の上端が梁の Z 範囲より下（梁が柱に乗って面で触れるだけ）は取り付きとみなさない。
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{1500.0, 0.0})};
	const std::vector<ColumnGeom> columns =
		columnGeomsOf({column(Vec2{1500.0, 0.0}, 425.0, 3000.0)});
	CHECK(!endHasReceiver(0, Vec2{1500.0, 0.0}, geomsOf(members), members, columns));
}

TEST(joint_point_in_column_uses_axis_aligned_rectangle)
{
	const ColumnGeom geom = columnGeom(column(Vec2{1000.0, 2000.0}));
	CHECK(near(geom.halfWidth, 52.5));
	CHECK(near(geom.halfDepth, 52.5));
	CHECK(pointInColumn(Vec2{1000.0, 2000.0}, geom));
	CHECK(pointInColumn(Vec2{1052.5, 2000.0}, geom)); // 側面ちょうど
	CHECK(!pointInColumn(Vec2{1100.0, 2000.0}, geom));
}

TEST(joint_build_places_joint_at_column_supported_end)
{
	// 柱に受けられる梁端に仕口が付く（横架材同士では受け材が無いケース）。
	const MemberCommand beam = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{1500.0, 0.0});
	const ColumnCommand post = column(Vec2{1500.0, 0.0}, -2575.0, 3000.0);

	// 柱を渡さなければ受ける材が無いので仕口は 0。
	CHECK(buildJointCommands({beam}).empty());

	const std::vector<SymbolCommand> commands = buildJointCommands({beam}, {post});
	CHECK_EQ(commands.size(), std::size_t{1});
	CHECK_EQ(commands.front().symbol, std::string(kSymbolJoint));
	CHECK_EQ(commands.front().layer, std::string("1-横架材天端"));
	CHECK(near(commands.front().position.x, 1500.0));
	CHECK(near(commands.front().position.y, 0.0));
	// 終端の内側方向は −軸（+X の梁なので 180 度）。
	CHECK(near(std::abs(commands.front().angle), 180.0));
}

// --- 退化した材（Python 版 TestDegenerateMembers）----------------------------

TEST(joint_degenerate_member_is_skipped)
{
	const MemberCommand degenerate = member("1-横架材天端", Vec2{500.0, 500.0}, Vec2{500.0, 500.0});
	CHECK(!memberGeom(degenerate).valid);

	const MemberCommand other = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0});
	const std::vector<MemberCommand> members = {degenerate, other};
	CHECK(!endHasReceiver(0, Vec2{500.0, 500.0}, geomsOf(members), members, {}));
	CHECK(buildJointCommands({degenerate, other}).empty());
}

TEST(joint_end_has_receiver_rejects_out_of_range_index)
{
	// 範囲外のインデックスは受ける材なしとして扱う（呼び出し側は範囲内しか渡さないが、
	// 添字で落ちないことを守る）。
	const std::vector<MemberCommand> members = {
		member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})};
	CHECK(!endHasReceiver(5, Vec2{0.0, 0.0}, geomsOf(members), members, {}));
}

// --- 命令の組み立て（Python 版 TestBuildJointCommands）-----------------------

TEST(joint_t_junction_places_single_joint_at_stem_end)
{
	const MemberCommand a = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0});
	const MemberCommand b = member("1-横架材天端", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0});

	const std::vector<SymbolCommand> commands = buildJointCommands({a, b});
	CHECK_EQ(commands.size(), std::size_t{1});
	CHECK_EQ(commands.front().symbol, std::string(kSymbolJoint));
	CHECK_EQ(commands.front().layer, std::string("1-横架材天端"));
	CHECK(near(commands.front().position.x, 1500.0));
	CHECK(near(commands.front().position.y, 60.0));
	// 内側方向（+Y。B の始端から終端へ）＝ 90 度。
	CHECK(near(commands.front().angle, 90.0));
}

TEST(joint_free_member_has_no_joints)
{
	CHECK(buildJointCommands({member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})}).empty());
}

TEST(joint_both_ends_received_places_two_joints)
{
	// 2 本の桁の間に架かる梁は両端に仕口が付く。
	const MemberCommand left = member("1-横架材天端", Vec2{0.0, -2000.0}, Vec2{0.0, 2000.0});
	const MemberCommand right = member("1-横架材天端", Vec2{3000.0, -2000.0}, Vec2{3000.0, 2000.0});
	const MemberCommand span = member("1-横架材天端", Vec2{60.0, 0.0}, Vec2{2940.0, 0.0});

	const std::vector<SymbolCommand> commands = buildJointCommands({left, right, span});
	const auto onSpan =
		std::ranges::count_if(commands,
							  [](const SymbolCommand& c) {
								  return near(c.position.y, 0.0) &&
										 (near(c.position.x, 60.0) || near(c.position.x, 2940.0));
							  });
	CHECK_EQ(onSpan, 2);
}

TEST(joint_noboribari_end_on_moya_places_joint)
{
	// 登り梁（R-登り梁）の上端が別レイヤの棟木（R-母屋）に取り付くと、登り梁の端部に
	// 仕口が付く。仕口は登り梁と同じレイヤに描かれる。
	const MemberCommand munagi =
		member("R-母屋", Vec2{-2000.0, 3000.0}, Vec2{2000.0, 3000.0}, 120.0, 180.0, 6000.0, 6000.0);
	MemberCommand nobori =
		member("R-登り梁", Vec2{0.0, 0.0}, Vec2{0.0, 2940.0}, 120.0, 180.0, 425.0, 6000.0);
	nobori.drawClass = CLASS_NOBORIBARI;

	const std::vector<SymbolCommand> commands = buildJointCommands({munagi, nobori});
	const auto onNobori = std::ranges::count_if(commands, [](const SymbolCommand& c)
												{ return c.layer == "R-登り梁"; });
	CHECK_EQ(onNobori, 1);
	for (const SymbolCommand& c : commands)
	{
		if (c.layer != "R-登り梁")
			continue;
		CHECK(near(c.position.x, 0.0));
		CHECK(near(c.position.y, 2940.0));
		CHECK_EQ(c.symbol, std::string(kSymbolJoint));
	}
}

TEST(joint_result_is_order_independent)
{
	// 入力の並びを変えても仕口の集合は同じ（判定がジオメトリだけで決まる）。
	const MemberCommand a = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0});
	const MemberCommand b = member("1-横架材天端", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0});
	const MemberCommand c = member("1-横架材天端", Vec2{5000.0, 5000.0}, Vec2{7000.0, 5000.0});

	CHECK(keysOf(buildJointCommands({a, b, c})) == keysOf(buildJointCommands({c, b, a})));
}

// --- 高さ（zOffset）------------------------------------------------------

TEST(joint_flat_member_keeps_layer_plane_height)
{
	// レベルちょうどに載る平らな梁（バウンド offset = 0）は高さ調整をしない＝従来どおり
	// レイヤ平面に載る。
	const MemberCommand a = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0});
	const MemberCommand b = member("1-横架材天端", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0});

	const std::vector<SymbolCommand> commands = buildJointCommands({a, b});
	CHECK_EQ(commands.size(), std::size_t{1});
	CHECK(near(commands.front().zOffset, 0.0));
}

TEST(joint_height_follows_end_bound_offset)
{
	// 段差梁（天端がレベルより 150mm 下）の仕口はレイヤ平面から 150mm 下がる。
	const MemberCommand girder = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 120.0,
										180.0, 275.0, 275.0, -150.0, -150.0);
	const MemberCommand beam = member("1-横架材天端", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0},
									  120.0, 180.0, 275.0, 275.0, -150.0, -150.0);

	const std::vector<SymbolCommand> commands = buildJointCommands({girder, beam});
	CHECK_EQ(commands.size(), std::size_t{1});
	CHECK(near(commands.front().zOffset, -150.0));
}

TEST(joint_sloped_member_uses_the_offset_of_its_own_end)
{
	// 登り梁は両端で天端 Z が違う（軒桁 425 ↔ 棟木 6000）。両端に仕口が付くとき、始端の
	// 仕口は startBound、終端の仕口は endBound の offset を持つ——1 本の梁に一律の高さを
	// 与えるのではなく、**端部ごとに**梁の天端へ合わせる。
	MemberCommand eaves = member("R-軒高", Vec2{-2000.0, 0.0}, Vec2{2000.0, 0.0}, 120.0, 180.0,
								 425.0, 425.0, 0.0, 0.0);
	MemberCommand munagi = member("R-母屋", Vec2{-2000.0, 3000.0}, Vec2{2000.0, 3000.0}, 120.0,
								  180.0, 6000.0, 6000.0, 5575.0, 5575.0);
	MemberCommand nobori = member("R-登り梁", Vec2{0.0, 60.0}, Vec2{0.0, 2940.0}, 120.0, 180.0,
								  425.0, 6000.0, 0.0, 5575.0);
	nobori.drawClass = CLASS_NOBORIBARI;

	double startZ = 0.0;
	double endZ = 0.0;
	std::size_t onNobori = 0;
	for (const SymbolCommand& command : buildJointCommands({eaves, munagi, nobori}))
	{
		if (command.layer != "R-登り梁")
			continue;
		++onNobori;
		if (near(command.position.y, 60.0))
			startZ = command.zOffset;
		else
			endZ = command.zOffset;
	}
	CHECK_EQ(onNobori, std::size_t{2});
	CHECK(near(startZ, 0.0));
	CHECK(near(endZ, 5575.0));
}

TEST(joint_height_is_taken_from_the_receiving_member_independent_bound)
{
	// 高さは**仕口が載る梁自身**の端部から取る（受ける材の高さではない）。受ける材の
	// offset を変えても、仕口の高さは変わらない。
	const MemberCommand girder = member("1-横架材天端", Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 120.0,
										180.0, 425.0, 425.0, 0.0, 0.0);
	const MemberCommand beam = member("1-横架材天端", Vec2{1500.0, 60.0}, Vec2{1500.0, 2000.0},
									  120.0, 180.0, 425.0, 425.0, 120.0, 120.0);

	const std::vector<SymbolCommand> commands = buildJointCommands({girder, beam});
	CHECK_EQ(commands.size(), std::size_t{1});
	CHECK(near(commands.front().position.y, 60.0)); // 仕口は beam の始端
	CHECK(near(commands.front().zOffset, 120.0)); // beam の startBound（girder の 0 ではない）
}

// --- 実フィクスチャ（Python 版 TestBuildFromFixture / …WithColumns）----------

TEST(joint_fixture_shape_and_layers)
{
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<MemberCommand> members = buildMemberCommands(model);
	const std::vector<SymbolCommand> joints = buildJointCommands(members);
	CHECK(!joints.empty());

	std::set<std::string> memberLayers;
	for (const MemberCommand& command : members)
		memberLayers.insert(command.layer);
	double minX = joints.front().position.x;
	double maxX = joints.front().position.x;
	double minY = joints.front().position.y;
	double maxY = joints.front().position.y;
	for (const SymbolCommand& joint : joints)
	{
		CHECK_EQ(joint.symbol, std::string(kSymbolJoint));
		// 仕口は横架材と同じレイヤに置く。
		CHECK(memberLayers.contains(joint.layer));
		minX = std::min(minX, joint.position.x);
		maxX = std::max(maxX, joint.position.x);
		minY = std::min(minY, joint.position.y);
		maxY = std::max(maxY, joint.position.y);
	}
	// 基準点は member 命令の端点なので、横架材と同じセンタリングに乗っている。
	CHECK(minX < 0.0 && maxX > 0.0);
	CHECK(minY < 0.0 && maxY > 0.0);
}

TEST(joint_columns_only_add_joints)
{
	// 柱を渡しても横架材同士の仕口は消えない（受ける材判定は柱の有無に依らない）。かつ
	// 実データでは柱に受けられる梁端の仕口が少なくとも 1 件は増える。
	std::size_t totalAdded = 0;
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<MemberCommand> members = buildMemberCommands(model);
		const std::vector<ColumnCommand> columns = buildColumnCommands(model, members);
		const std::vector<SymbolCommand> without = buildJointCommands(members);
		const std::vector<SymbolCommand> withColumns = buildJointCommands(members, columns);

		const std::multiset<JointKey> base = keysOf(without);
		const std::multiset<JointKey> extended = keysOf(withColumns);
		// 柱なしの各仕口は柱ありにも必ず同数以上含まれる（上位集合）。
		for (const JointKey& key : base)
			CHECK(extended.count(key) >= base.count(key));
		CHECK(withColumns.size() >= without.size());
		totalAdded += withColumns.size() - without.size();
	}
	CHECK(totalAdded > 0);
}

TEST(joint_added_joints_land_on_columns)
{
	// 柱ありでのみ現れる仕口（＝柱に受けられた梁端）は、いずれかの柱の断面 footprint に
	// 載っている（実データでの幾何的な妥当性）。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<MemberCommand> members = buildMemberCommands(model);
		const std::vector<ColumnCommand> columns = buildColumnCommands(model, members);
		const std::vector<ColumnGeom> columnGeoms = columnGeomsOf(columns);
		const std::multiset<JointKey> without = keysOf(buildJointCommands(members));

		std::multiset<JointKey> seen;
		for (const SymbolCommand& joint : buildJointCommands(members, columns))
		{
			const JointKey key = keyOf(joint);
			seen.insert(key);
			if (seen.count(key) <= without.count(key))
				continue; // 柱なしでも出ていた仕口
			CHECK(std::ranges::any_of(columnGeoms, [&joint](const ColumnGeom& geom)
									  { return pointInColumn(joint.position, geom); }));
		}
	}
}

TEST(joint_fixture_height_matches_member_ends)
{
	// 実データでも、各仕口の高さはその位置・レイヤに端部を持つ横架材のバウンド offset と
	// 一致する。かつ**レイヤ平面から外れる仕口が実際に出る**（登り梁・母屋・段差梁）——
	// ここが 0 件なら、この高さ調整は何も直していないことになる。
	std::size_t raised = 0;
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<MemberCommand> members = buildMemberCommands(model);
		for (const SymbolCommand& joint : buildJointCommands(members))
		{
			// 同じレイヤに同じ端点を持つ横架材のうち、その端部の offset が仕口の高さと
			// 一致するものが必ずある。
			const bool matched = std::ranges::any_of(
				members,
				[&joint](const MemberCommand& m)
				{
					if (m.layer != joint.layer)
						return false;
					return (near(m.start.x, joint.position.x) &&
							near(m.start.y, joint.position.y) &&
							near(m.startBound.offset, joint.zOffset)) ||
						   (near(m.end.x, joint.position.x) && near(m.end.y, joint.position.y) &&
							near(m.endBound.offset, joint.zOffset));
				});
			CHECK(matched);
			if (std::abs(joint.zOffset) > 1.0)
				++raised;
		}
	}
	CHECK(raised > 0);
}

TEST(joint_all_fixtures_build)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<SymbolCommand> joints = buildJointCommands(buildMemberCommands(model));
		CHECK(!joints.empty());
		for (const SymbolCommand& joint : joints)
			CHECK_EQ(joint.symbol, std::string(kSymbolJoint));
	}
}

TEST(joint_fixture_result_is_order_independent_with_columns)
{
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	std::vector<MemberCommand> members = buildMemberCommands(model);
	std::vector<ColumnCommand> columns = buildColumnCommands(model, members);
	const std::multiset<JointKey> forward = keysOf(buildJointCommands(members, columns));

	std::ranges::reverse(members);
	std::ranges::reverse(columns);
	CHECK(forward == keysOf(buildJointCommands(members, columns)));
}

TEST_MAIN();
