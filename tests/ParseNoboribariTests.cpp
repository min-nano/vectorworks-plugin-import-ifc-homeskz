//
//	ParseNoboribariTests.cpp
//
//	登り梁の位置補正（src/parse/Noboribari）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	Python 版 test_ifc_noboribari.py のケースを写している（柱を参照するケースは M8 で柱を
//	導入するときに足す。docs/DEV-NOTES.md M7）。
//
//	検証項目（docs/DEV-NOTES.md M7）: 屋根面の天端 Z と内包判定・受ける材への端部詰め（Z 範囲で
//	絞る・極小の食い込みは詰めない・詰めすぎになるなら詰めない）・登り梁の真上の屋根面の
//	選択（勾配方向が平行・外形が内包）・天端の屋根面スナップ（勾配・高さ・バインド offset）・
//	登り梁でない材の素通し・実フィクスチャからの屋根面収集。実フィクスチャのパスは CMake が
//	HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Context.h"
#include "parse/IfcGeometry.h"
#include "parse/Loader.h"
#include "parse/Member.h"
#include "parse/Noboribari.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::StoryBoundCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::Vec3;
using HomeskzIfcImport::parse::buildMemberCommands;
using HomeskzIfcImport::parse::CLASS_MOYA;
using HomeskzIfcImport::parse::CLASS_NOBORIBARI;
using HomeskzIfcImport::parse::collectRoofPlanes;
using HomeskzIfcImport::parse::Context;
using HomeskzIfcImport::parse::correctNoboribari;
using HomeskzIfcImport::parse::correctOneNoboribari;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::noboribariColumnPenetration;
using HomeskzIfcImport::parse::noboribariEndTrim;
using HomeskzIfcImport::parse::NoboribariRoofPlane;
using HomeskzIfcImport::parse::RoofPlane;
using HomeskzIfcImport::parse::roofPlaneFor;
using HomeskzIfcImport::parse::roofSlope;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// 平面外形が広い矩形の屋根面を作る（Python 版 _flat_roof_plane）。
	// zAt(x, y) = zAtOrigin + slope · x になるよう法線を定める（+x へ進むと slope ぶん上がる）。
	NoboribariRoofPlane flatRoofPlane(double slope, double zAtOrigin)
	{
		// 平面 z = slope·x（ストーリ相対）。法線は (−slope, 0, 1) を正規化したもの。
		const double norm = std::hypot(slope, 1.0);
		RoofPlane plane;
		plane.normal = Vec3{-slope / norm, 0.0, 1.0 / norm};
		constexpr double kSpan = 10000.0;
		plane.vertices = {Vec3{-kSpan, -kSpan, -slope * kSpan}, Vec3{kSpan, -kSpan, slope * kSpan},
						  Vec3{kSpan, kSpan, slope * kSpan}, Vec3{-kSpan, kSpan, -slope * kSpan}};

		NoboribariRoofPlane result;
		// 退化していない面なので必ず成功する（万一失敗すれば run=0 のまま zAt が破綻し、
		// 呼び出し側の期待値が合わずテストが落ちる）。
		roofSlope(plane, result.slope);
		result.plan = parse::RoofSlope::plan(plane);
		result.storeyElevation = zAtOrigin;
		return result;
	}

	// 勾配方向が +y の屋根面（登り梁（+x 方向）と直交＝選ばれてはいけない面）。
	NoboribariRoofPlane crossingRoofPlane()
	{
		const double slope = 0.25;
		const double norm = std::hypot(slope, 1.0);
		RoofPlane plane;
		plane.normal = Vec3{0.0, -slope / norm, 1.0 / norm};
		constexpr double kSpan = 10000.0;
		plane.vertices = {Vec3{-kSpan, -kSpan, -slope * kSpan}, Vec3{kSpan, -kSpan, -slope * kSpan},
						  Vec3{kSpan, kSpan, slope * kSpan}, Vec3{-kSpan, kSpan, slope * kSpan}};

		NoboribariRoofPlane result;
		roofSlope(plane, result.slope);
		result.plan = parse::RoofSlope::plan(plane);
		result.storeyElevation = 900.0;
		return result;
	}

	// 登り梁の member 命令（バインドは登り梁レベル。levelZ はそのレベルの絶対 Z）。
	MemberCommand noboribari(const Vec2& start, const Vec2& end, double elevation,
							 double endElevation, double levelZ = 800.0, double height = 105.0)
	{
		MemberCommand command;
		command.layer = "2-登り梁";
		command.memberId = "nobori";
		command.drawClass = CLASS_NOBORIBARI;
		command.start = start;
		command.end = end;
		command.width = 105.0;
		command.height = height;
		command.elevation = elevation;
		command.endElevation = endElevation;
		command.startBound = StoryBoundCommand{0, "登り梁", elevation - levelZ};
		command.endBound = StoryBoundCommand{0, "登り梁", endElevation - levelZ};
		return command;
	}

	// 受ける横架材（水平材）の member 命令。
	MemberCommand receiver(const Vec2& start, const Vec2& end, double elevation,
						   double width = 105.0, double height = 150.0)
	{
		MemberCommand command;
		command.layer = "2-母屋";
		command.memberId = "recv";
		command.drawClass = CLASS_MOYA;
		command.start = start;
		command.end = end;
		command.width = width;
		command.height = height;
		command.elevation = elevation;
		command.endElevation = elevation;
		command.startBound = StoryBoundCommand{0, "母屋", 0.0};
		command.endBound = StoryBoundCommand{0, "母屋", 0.0};
		return command;
	}

	// 受ける柱の column 命令（M8。登り梁の端部詰めは柱面まで見る）。
	core::ColumnCommand column(const Vec2& position, double elevation, double height = 500.0,
							   double width = 105.0, double depth = 105.0)
	{
		core::ColumnCommand command;
		command.layer = "2to2.5-柱";
		command.memberId = "col";
		command.drawClass = HomeskzIfcImport::parse::CLASS_KOYAZUKA;
		command.structuralUse = "5";
		command.position = position;
		command.width = width;
		command.depth = depth;
		command.height = height;
		command.elevation = elevation;
		command.bottomBound = StoryBoundCommand{0, "軒高", 0.0};
		command.topBound = StoryBoundCommand{0, "軒高", height};
		return command;
	}

	// 2 つの命令が（補正対象のフィールドについて）同じか。素通しの確認に使う。
	bool sameCommand(const MemberCommand& a, const MemberCommand& b)
	{
		return a.layer == b.layer && a.memberId == b.memberId && a.drawClass == b.drawClass &&
			   near(a.start.x, b.start.x) && near(a.start.y, b.start.y) && near(a.end.x, b.end.x) &&
			   near(a.end.y, b.end.y) && near(a.elevation, b.elevation) &&
			   near(a.endElevation, b.endElevation) &&
			   near(a.startBound.offset, b.startBound.offset) &&
			   near(a.endBound.offset, b.endBound.offset);
	}
} // namespace

// ---------------------------------------------------------------------------
// NoboribariRoofPlane
// ---------------------------------------------------------------------------

TEST(roof_plane_z_at_follows_slope)
{
	const NoboribariRoofPlane plane = flatRoofPlane(0.25, 900.0);
	CHECK(near(plane.zAt(0.0, 0.0), 900.0));
	CHECK(near(plane.zAt(1000.0, 0.0), 1150.0));
	CHECK(near(plane.zAt(-400.0, 0.0), 800.0));
}

TEST(roof_plane_contains_only_inside_footprint)
{
	const NoboribariRoofPlane plane = flatRoofPlane(0.25, 900.0);
	CHECK(plane.contains(0.0, 0.0));
	CHECK(!plane.contains(20000.0, 0.0));
}

TEST(roof_plane_without_footprint_contains_nothing)
{
	// 外形が面にならない（3 点未満）平面は何も内包しない。
	NoboribariRoofPlane degenerate = flatRoofPlane(0.25, 900.0);
	degenerate.plan.resize(2);
	CHECK(!degenerate.contains(0.0, 0.0));
}

// ---------------------------------------------------------------------------
// noboribariEndTrim
// ---------------------------------------------------------------------------

TEST(end_trim_trims_to_member_face)
{
	// 受ける母屋: y 方向に走る中心 x=1050・半幅 52.5 → 手前の面 x=997.5。
	// 登り梁の終端 (1000, 0)・外向き +x は面より 2.5mm 内側 → 2.5mm 詰める。
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{1050.0, -1000.0}, Vec2{1050.0, 1000.0}, 1000.0)};
	CHECK(near(noboribariEndTrim(Vec2{1000.0, 0.0}, Vec2{1.0, 0.0}, 900.0, 1050.0, receivers, {}),
			   2.5));
}

TEST(end_trim_is_gated_by_z_range)
{
	// Z 範囲が離れた受け材は対象外（食い込み 0）。
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{1050.0, -1000.0}, Vec2{1050.0, 1000.0}, 5000.0)};
	CHECK(near(noboribariEndTrim(Vec2{1000.0, 0.0}, Vec2{1.0, 0.0}, 900.0, 1050.0, receivers, {}),
			   0.0));
}

TEST(end_trim_ignores_degenerate_receiver)
{
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{1000.0, 0.0}, Vec2{1000.0, 0.0}, 1120.0)};
	CHECK(near(noboribariEndTrim(Vec2{1000.0, 0.0}, Vec2{1.0, 0.0}, 900.0, 1150.0, receivers, {}),
			   0.0));
}

// ---------------------------------------------------------------------------
// noboribariColumnPenetration / 柱を受け材にした端部詰め（M8）
// ---------------------------------------------------------------------------

TEST(column_penetration_trims_to_near_face)
{
	// 柱: 中心 (1000, 0)・105 角 → 面は x=947.5 / 1052.5、y=±52.5。端点が中心にあり
	// 外向き +x なら、内側（−x）へ 52.5mm 引き戻すと手前の面 947.5 に出る。
	const core::ColumnCommand receiver = column(Vec2{1000.0, 0.0}, 900.0);
	CHECK(near(noboribariColumnPenetration(Vec2{1000.0, 0.0}, Vec2{1.0, 0.0}, receiver), 52.5));
}

TEST(column_penetration_is_zero_outside_the_section)
{
	// 端点が柱の断面の外（x が半幅を超える）なら食い込んでいない。
	const core::ColumnCommand receiver = column(Vec2{1000.0, 0.0}, 900.0);
	CHECK(near(noboribariColumnPenetration(Vec2{1100.0, 0.0}, Vec2{1.0, 0.0}, receiver), 0.0));
	// y 方向に外れている場合も同じ。
	CHECK(near(noboribariColumnPenetration(Vec2{1000.0, 100.0}, Vec2{1.0, 0.0}, receiver), 0.0));
}

TEST(column_penetration_takes_the_nearest_face_for_a_diagonal)
{
	// 外向きが斜め（+x, +y の単位ベクトル）なら、X 面・Y 面のうち先に出る方までの距離。
	// 端点 (1030, 1010) は中心 (1000, 1000) から X へ 30・Y へ 10 入った位置で、内側方向は
	// (−1/√2, −1/√2)。X 面（947.5）までは (947.5−1030)/(−1/√2) ≈ 116.7、Y 面（947.5）までは
	// (947.5−1010)/(−1/√2) ≈ 88.4 → 小さい方を採る。
	const core::ColumnCommand receiver = column(Vec2{1000.0, 1000.0}, 900.0);
	const double diagonal = 1.0 / std::sqrt(2.0);
	CHECK(
		near(noboribariColumnPenetration(Vec2{1030.0, 1010.0}, Vec2{diagonal, diagonal}, receiver),
			 62.5 / diagonal));
}

TEST(column_penetration_is_zero_on_the_face)
{
	// 端点がちょうど手前の面（x=947.5）にあると引き戻す距離が 0 になり、詰めない。
	const core::ColumnCommand receiver = column(Vec2{1000.0, 0.0}, 900.0);
	CHECK(near(noboribariColumnPenetration(Vec2{947.5, 0.0}, Vec2{1.0, 0.0}, receiver), 0.0));
}

TEST(column_penetration_is_zero_when_direction_degenerates)
{
	// 外向きが 0 ベクトル（方向が定まらない）なら詰めない。
	const core::ColumnCommand receiver = column(Vec2{1000.0, 0.0}, 900.0);
	CHECK(near(noboribariColumnPenetration(Vec2{1000.0, 0.0}, Vec2{0.0, 0.0}, receiver), 0.0));
}

TEST(end_trim_trims_to_column_face)
{
	// 受け材が無くても、Z 範囲の重なる柱があれば端部を柱の手前の面まで詰める。
	const std::vector<core::ColumnCommand> columns = {column(Vec2{1000.0, 0.0}, 900.0)};
	CHECK(near(noboribariEndTrim(Vec2{1000.0, 0.0}, Vec2{1.0, 0.0}, 900.0, 1050.0, {}, columns),
			   52.5));
}

TEST(end_trim_column_is_gated_by_z_range)
{
	// Z 範囲が離れた柱（下端 5000・上端 5500）は取り合いでない。
	const std::vector<core::ColumnCommand> columns = {column(Vec2{1000.0, 0.0}, 5000.0)};
	CHECK(near(noboribariEndTrim(Vec2{1000.0, 0.0}, Vec2{1.0, 0.0}, 900.0, 1050.0, {}, columns),
			   0.0));
}

TEST(correct_one_trims_the_end_against_a_column)
{
	// 梁 (0,0)→(1000,0)。終端側に中心 (1000, 0)・105 角の柱があり、手前の面 947.5 まで詰める。
	const MemberCommand command =
		noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1150.0, 800.0);
	const std::vector<core::ColumnCommand> columns = {column(Vec2{1000.0, 0.0}, 900.0, 300.0)};

	const MemberCommand out = correctOneNoboribari(command, {}, {}, columns, Vec2{0.0, 0.0});
	CHECK(near(out.end.x, 947.5));
	CHECK(near(out.start.x, 0.0)); // 始端は受けるものが無く不変
}

// ---------------------------------------------------------------------------
// roofPlaneFor
// ---------------------------------------------------------------------------

TEST(roof_plane_for_selects_aligned_plane)
{
	const MemberCommand command = noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1300.0);
	const std::vector<NoboribariRoofPlane> planes = {crossingRoofPlane(),
													 flatRoofPlane(0.25, 900.0)};
	const NoboribariRoofPlane* found = roofPlaneFor(command, planes, Vec2{0.0, 0.0});
	CHECK(found == &planes[1]);
}

TEST(roof_plane_for_none_when_no_aligned_plane)
{
	const MemberCommand command = noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1300.0);
	const std::vector<NoboribariRoofPlane> planes = {crossingRoofPlane()};
	CHECK(roofPlaneFor(command, planes, Vec2{0.0, 0.0}) == nullptr);
}

TEST(roof_plane_for_none_for_degenerate_length)
{
	// 平面投影長が極小の登り梁は屋根面を求めない。
	const MemberCommand command = noboribari(Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, 1000.0, 1000.0);
	const std::vector<NoboribariRoofPlane> planes = {flatRoofPlane(0.25, 900.0)};
	CHECK(roofPlaneFor(command, planes, Vec2{0.0, 0.0}) == nullptr);
}

// ---------------------------------------------------------------------------
// correctOneNoboribari（端部詰め → 屋根スナップ）
// ---------------------------------------------------------------------------

TEST(snaps_pitch_and_height_to_roof)
{
	// 梁の勾配 0.3（1000→1300）に対し屋根は 0.25。レベル絶対 Z = 800。
	const MemberCommand command =
		noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1300.0, 800.0);
	const std::vector<NoboribariRoofPlane> planes = {flatRoofPlane(0.25, 900.0)};

	const MemberCommand out = correctOneNoboribari(command, planes, {}, {}, Vec2{0.0, 0.0});
	CHECK(near(out.elevation, 900.0));
	CHECK(near(out.endElevation, 1150.0));
	const double span = std::hypot(out.end.x - out.start.x, out.end.y - out.start.y);
	CHECK(near((out.endElevation - out.elevation) / span, 0.25));
	// バインド offset は新しい天端 − レベル絶対 Z（800）
	CHECK(near(out.startBound.offset, 100.0));
	CHECK(near(out.endBound.offset, 350.0));
	CHECK_EQ(out.startBound.level, "登り梁");
}

TEST(trims_penetrating_ends)
{
	// 梁 (0,0)→(1000,0)。終端側に x=1000 中心・半幅 52.5 の母屋（y 走り）。
	const MemberCommand command =
		noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1150.0, 800.0);
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{1000.0, -1000.0}, Vec2{1000.0, 1000.0}, 1120.0)};

	const MemberCommand out = correctOneNoboribari(command, {}, receivers, {}, Vec2{0.0, 0.0});
	CHECK(near(out.end.x, 947.5));
	CHECK(near(out.start.x, 0.0)); // 始端は受け材が無く不変
}

TEST(no_roof_keeps_height_but_trims)
{
	const MemberCommand command =
		noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1300.0, 800.0);
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{1000.0, -1000.0}, Vec2{1000.0, 1000.0}, 1250.0)};

	const MemberCommand out = correctOneNoboribari(command, {}, receivers, {}, Vec2{0.0, 0.0});
	CHECK(near(out.elevation, 1000.0)); // 天端はそのまま
	CHECK(near(out.endElevation, 1300.0));
	CHECK(near(out.end.x, 947.5)); // 食い込みは詰める
}

TEST(degenerate_length_returned_unchanged)
{
	const MemberCommand command = noboribari(Vec2{5.0, 5.0}, Vec2{5.0, 5.0}, 1000.0, 1000.0);
	const MemberCommand out = correctOneNoboribari(command, {}, {}, {}, Vec2{0.0, 0.0});
	CHECK(sameCommand(out, command));
}

TEST(over_trim_is_skipped)
{
	// 両端の直近に受け材を置き、各端 2.5mm 詰めると全長 4 → −1 になる → 詰めない。
	const MemberCommand command = noboribari(Vec2{0.0, 0.0}, Vec2{4.0, 0.0}, 1000.0, 1000.6);
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{-50.0, -1000.0}, Vec2{-50.0, 1000.0}, 1000.0),
		receiver(Vec2{54.0, -1000.0}, Vec2{54.0, 1000.0}, 1000.0)};

	const MemberCommand out = correctOneNoboribari(command, {}, receivers, {}, Vec2{0.0, 0.0});
	CHECK(near(out.start.x, 0.0));
	CHECK(near(out.end.x, 4.0));
}

TEST(tiny_penetration_not_trimmed)
{
	// 母屋の中心 x=1052.4・半幅 52.5 → 手前の面 999.9。端点 1000 は 0.1mm だけ内側で、
	// 詰める下限（0.5mm）未満なので触らない。
	const MemberCommand command =
		noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1150.0, 800.0);
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{1052.4, -1000.0}, Vec2{1052.4, 1000.0}, 1120.0)};

	const MemberCommand out = correctOneNoboribari(command, {}, receivers, {}, Vec2{0.0, 0.0});
	CHECK(near(out.end.x, 1000.0));
}

TEST(snap_uses_trimmed_end_position)
{
	// 端部を詰めてから屋根面へ落とすので、終端の天端は「詰めた後の XY」で決まる。
	const MemberCommand command =
		noboribari(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 1000.0, 1300.0, 800.0);
	const std::vector<NoboribariRoofPlane> planes = {flatRoofPlane(0.25, 900.0)};
	const std::vector<MemberCommand> receivers = {
		receiver(Vec2{1000.0, -1000.0}, Vec2{1000.0, 1000.0}, 1120.0)};

	const MemberCommand out = correctOneNoboribari(command, planes, receivers, {}, Vec2{0.0, 0.0});
	CHECK(near(out.end.x, 947.5));
	CHECK(near(out.endElevation, 900.0 + (0.25 * 947.5)));
}

// ---------------------------------------------------------------------------
// correctNoboribari / collectRoofPlanes（IFC 連携）
// ---------------------------------------------------------------------------

TEST(passes_non_noboribari_through_unchanged)
{
	bool ok = false;
	const Model& model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	if (!ok)
		return;

	const std::vector<MemberCommand> members = buildMemberCommands(model);
	CHECK(!members.empty());
	const bool anyNoboribari = std::ranges::any_of(members, [](const MemberCommand& m)
												   { return m.drawClass == CLASS_NOBORIBARI; });
	CHECK(!anyNoboribari);

	const std::vector<MemberCommand> out = correctNoboribari(model, members, {});
	CHECK_EQ(out.size(), members.size());
	for (std::size_t i = 0; i < out.size() && i < members.size(); ++i)
		CHECK(sameCommand(out[i], members[i]));
}

TEST(collect_roof_planes_skips_unresolvable_roof_slabs)
{
	// 形状表現を持たない屋根版（屋根面を解決できない）と、ほぼ水平な屋根版（勾配方向が
	// 定まらない）はどちらも集めない＝垂木・野地板と同じ関門を通る。
	Model const model =
		loadIfcFromText("#1=IFCBUILDINGSTOREY('s',$,'RFL',$,$,$,$,$,.ELEMENT.,3000.);\n"
						"#2=IFCSLAB('a',$,'屋根版:1',$,$,$,$,$,$);\n"
						"#3=IFCCARTESIANPOINT((0.,0.));\n"
						"#4=IFCCARTESIANPOINT((1000.,0.));\n"
						"#5=IFCCARTESIANPOINT((1000.,1000.));\n"
						"#6=IFCPOLYLINE((#3,#4,#5,#3));\n"
						"#7=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#6);\n"
						"#8=IFCDIRECTION((0.,0.,1.));\n"
						"#9=IFCEXTRUDEDAREASOLID(#7,$,#8,12.);\n"
						"#10=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#9));\n"
						"#11=IFCPRODUCTDEFINITIONSHAPE($,$,(#10));\n"
						"#12=IFCSLAB('b',$,'屋根版:2',$,$,$,#11,$,$);\n"
						"#13=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#2,#12),#1);\n");

	Context context(model);
	CHECK(collectRoofPlanes(context).empty());
}

TEST(collects_roof_planes_from_fixture)
{
	bool ok = false;
	const Model& model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	if (!ok)
		return;

	Context context(model);
	const std::vector<NoboribariRoofPlane> planes = collectRoofPlanes(context);
	CHECK(!planes.empty());
	for (const NoboribariRoofPlane& plane : planes)
	{
		// 勾配方向が定まる面だけを集める（退化した面は roofSlope が弾く）。
		CHECK(plane.slope.rise > 0.0);
		CHECK(plane.slope.run > 0.0);
		CHECK(plane.plan.size() >= 3);
	}
}

TEST(processes_injected_noboribari)
{
	// フィクスチャの屋根版と重ならない位置に合成登り梁＋受け材を置く（屋根スナップは
	// 効かないが、端部の食い込み詰めが働くことを検証する）。
	bool ok = false;
	const Model& model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	if (!ok)
		return;

	const MemberCommand nobori = noboribari(Vec2{50000.0, 0.0}, Vec2{51000.0, 0.0}, 1000.0, 1150.0);
	MemberCommand recv = receiver(Vec2{51050.0, -1000.0}, Vec2{51050.0, 1000.0}, 1120.0);
	recv.layer = "2-登り梁"; // Z 重なりのみで判定するためレイヤは不問

	const std::vector<MemberCommand> out = correctNoboribari(model, {nobori, recv}, {});
	CHECK_EQ(out.size(), std::size_t(2));
	if (out.size() == 2)
	{
		// 登り梁の終端が受け材の手前の面（51050 − 52.5 = 50997.5）まで詰められる。
		CHECK(near(out[0].end.x, 50997.5));
		// 受け材（登り梁でない）は不変。
		CHECK(sameCommand(out[1], recv));
	}
}

TEST_MAIN();
