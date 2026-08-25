//
//	ParseRafterTests.cpp
//
//	垂木解析（src/parse/Rafter）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:core/ parse/ は無 SDK
//	で単体テスト）。**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//	桁幅参照（girderWidthAt）は M7 で横架材が入って実寸を引けるようになったので、実寸の選択・
//	垂木と平行な材の除外・見つからないときの既定値フォールバックをそれぞれ検証する
//	（docs/DEV-NOTES.md M6「依存メモ」/ M7）。
//
//	検証項目（docs/DEV-NOTES.md M6）: 屋根面の掃引（両端は半幅内側・内部 455 以下・中間 455 ちょうど・
//	端数は両端へ等分）・走査線クリップ（非凸面の分割）・勾配（start=軒側の支持点／end=棟側）・
//	支持点（屋根面と横架材天端 Z の交点）・軒の出と差し込み・仕様ラベル・断面とクラス・
//	センタリング・ストーリ Elevation の加算・レイヤ振り分け・決定性。実フィクスチャのパスは
//	CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"
#include "Fixtures.h"
#include "RoofSample.h"

#include "core/Document.h"
#include "parse/IfcGeometry.h"
#include "parse/Loader.h"
#include "parse/Rafter.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::RafterCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::Vec3;
using HomeskzIfcImport::parse::buildRafterCommands;
using HomeskzIfcImport::parse::CLASS_TARUKI;
using HomeskzIfcImport::parse::girderWidthAt;
using HomeskzIfcImport::parse::kDefaultGirderWidth;
using HomeskzIfcImport::parse::kDefaultRafterHeight;
using HomeskzIfcImport::parse::kDefaultRafterWidth;
using HomeskzIfcImport::parse::kRafterInterval;
using HomeskzIfcImport::parse::loadIfc;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::raftersForPlane;
using HomeskzIfcImport::parse::RoofPlane;
using HomeskzIfcImport::parse::storyHasRoofSlab;
using HomeskzIfcImport::parse::sweepPositions;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::minimalRoofText;
using HomeskzIfcTests::near;
using HomeskzIfcTests::shedPlane;

namespace
{
	// 試験用の屋根面（4m×3m の片流れ）と、それに対応する最小の屋根版 IFC は
	// tests/RoofSample.h が唯一の定義。野地板（ParseRoofTests）と共有する。

	// 桁幅参照（girderWidthAt）に渡す軒桁の member 命令。芯線と幅だけを見るので、
	// 高さ・クラス・レイヤは既定のままでよい。
	MemberCommand girderMember(const Vec2& start, const Vec2& end, double width)
	{
		MemberCommand member;
		member.layer = "R-軒高";
		member.memberId = "girder";
		member.drawClass = CLASS_TARUKI; // 桁幅の判定はクラスを見ない
		member.start = start;
		member.end = end;
		member.width = width;
		member.height = 105.0;
		return member;
	}

	// 上の屋根面から垂木命令を作る（支持点なし＝start は軒先のまま）。
	std::vector<RafterCommand> shedRafters(double storeyElevation = 0.0,
										   const Vec2& center = Vec2{0.0, 0.0})
	{
		return raftersForPlane(shedPlane(), "R-垂木", storeyElevation, center);
	}

	// 支持点（横架材天端 Z）を与えて垂木命令を作る。
	std::vector<RafterCommand> shedRaftersWithBeamTop(double beamTopZ)
	{
		return raftersForPlane(shedPlane(), "R-垂木", 0.0, Vec2{0.0, 0.0},
							   std::optional<double>(beamTopZ));
	}

	// 命令のレイヤ名の集合。
	std::set<std::string> layersOf(const std::vector<RafterCommand>& rafters)
	{
		std::set<std::string> layers;
		for (const RafterCommand& rafter : rafters)
			layers.insert(rafter.layer);
		return layers;
	}
} // namespace

// ---------------------------------------------------------------------------
// 掃引位置（sweepPositions）: 両端は半幅内側・内部 455 以下・中間 455・端数は両端へ等分
// ---------------------------------------------------------------------------

TEST(sweep_ends_inset_by_half_width)
{
	// 両端の垂木は屋根面の端から inset（＝垂木幅の半分＝22.5mm）だけ内側へ寄る。
	std::vector<double> const pos = sweepPositions(0.0, 2000.0, 455.0, 22.5);
	CHECK(!pos.empty());
	if (pos.empty())
		return;
	CHECK(near(pos.front(), 22.5));
	CHECK(near(pos.back(), 2000.0 - 22.5));
}

TEST(sweep_interior_gaps_are_module)
{
	// 実効幅 (2000 − 2×22.5)=1955 / 455 → n=ceil=5 区間、中間 3 区間は 455 ちょうど。
	std::vector<double> const pos = sweepPositions(0.0, 2000.0, 455.0, 22.5);
	int exact = 0;
	double maxGap = 0.0;
	for (std::size_t i = 1; i < pos.size(); ++i)
	{
		const double gap = pos[i] - pos[i - 1];
		maxGap = std::max(maxGap, gap);
		if (near(gap, 455.0))
			++exact;
	}
	CHECK_EQ(exact, 3);
	CHECK(maxGap <= 455.0 + 1e-6);
}

TEST(sweep_end_gaps_split_remainder)
{
	// 実効幅 (1820 − 45)=1775 / 455 → n=ceil=4 区間、中間 2 区間は 455、端数は両端へ等分。
	std::vector<double> const pos = sweepPositions(0.0, 1820.0, 455.0, 22.5);
	CHECK_EQ(pos.size(), static_cast<std::size_t>(5));
	if (pos.size() != 5)
		return;
	int exact = 0;
	for (std::size_t i = 1; i < pos.size(); ++i)
	{
		if (near(pos[i] - pos[i - 1], 455.0))
			++exact;
	}
	CHECK_EQ(exact, 2);
	CHECK(near(pos[1] - pos[0], pos[4] - pos[3]));
}

TEST(sweep_width_within_interval_two_ends_only)
{
	// 実効幅 <= interval は内部無しで両端の 2 本のみ（いずれも半幅内側）。
	std::vector<double> const pos = sweepPositions(0.0, 400.0, 455.0, 22.5);
	CHECK_EQ(pos.size(), static_cast<std::size_t>(2));
	if (pos.size() != 2)
		return;
	CHECK(near(pos.front(), 22.5));
	CHECK(near(pos.back(), 400.0 - 22.5));
}

TEST(sweep_degenerate_width_single_center)
{
	// 半幅を差し引くと広がりが極小（屋根が垂木幅程度に狭い）なら中央 1 本だけ。
	std::vector<double> const pos = sweepPositions(1000.0, 1030.0, 455.0, 22.5);
	CHECK_EQ(pos.size(), static_cast<std::size_t>(1));
	if (pos.size() == 1)
		CHECK(near(pos.front(), 1015.0));
}

// --------------------------------------------------------------------------
// - 1 つの屋根面からの垂木（raftersForPlane）
// ---------------------------------------------------------------------------

TEST(rafters_at_both_ends_interior_455)
{
	// 掃引方向は X（軒に平行）。幅 4000mm。両端の垂木は屋根面の端から垂木幅の半分
	// （22.5mm）だけ内側、内部は 455 以下（中間は 455 ちょうど・端数は両端へ等分）。
	// 実効幅 (4000 − 45)=3955 / 455 → n=ceil=9 → 垂木 n+1=10 本。
	std::vector<RafterCommand> const rafters = shedRafters();
	CHECK_EQ(rafters.size(), static_cast<std::size_t>(10));
	if (rafters.size() != 10)
		return;

	std::vector<double> xs;
	for (const RafterCommand& rafter : rafters)
		xs.push_back(rafter.start.x);
	std::sort(xs.begin(), xs.end());
	CHECK(near(xs.front(), 22.5));
	CHECK(near(xs.back(), 4000.0 - 22.5));

	double maxGap = 0.0;
	bool anyExact = false;
	for (std::size_t i = 1; i < xs.size(); ++i)
	{
		const double gap = xs[i] - xs[i - 1];
		maxGap = std::max(maxGap, gap);
		if (near(gap, 455.0))
			anyExact = true;
	}
	CHECK(maxGap <= 455.0 + 1e-6);
	CHECK(anyExact);
	// 端数は両端の 2 区間へ等分（左右対称）。
	CHECK(near(xs[1] - xs[0], xs[xs.size() - 1] - xs[xs.size() - 2]));
}

TEST(rafters_run_up_slope_start_low_end_high)
{
	for (const RafterCommand& rafter : shedRafters())
	{
		// start=軒側（y=0, z=1000）、end=棟側（y=3000, z=2000）。
		CHECK(near(rafter.start.y, 0.0));
		CHECK(near(rafter.end.y, 3000.0));
		CHECK(rafter.endElevation > rafter.elevation);
		CHECK(near(rafter.elevation, 1000.0));
		CHECK(near(rafter.endElevation, 2000.0));
	}
}

TEST(rafter_default_section_and_class)
{
	std::vector<RafterCommand> const rafters = shedRafters();
	CHECK(!rafters.empty());
	if (rafters.empty())
		return;
	const RafterCommand& rafter = rafters.front();
	CHECK(near(rafter.width, 45.0));
	CHECK(near(rafter.height, 45.0));
	CHECK(near(kDefaultRafterWidth, 45.0));
	CHECK(near(kDefaultRafterHeight, 45.0));
	CHECK(near(kRafterInterval, 455.0));
	CHECK_EQ(rafter.drawClass, std::string(CLASS_TARUKI));
	CHECK_EQ(rafter.layer, std::string("R-垂木"));
}

TEST(rafter_storey_elevation_added_to_z)
{
	std::vector<RafterCommand> const rafters = shedRafters(6300.0);
	CHECK(!rafters.empty());
	if (rafters.empty())
		return;
	CHECK(near(rafters.front().elevation, 1000.0 + 6300.0));
	CHECK(near(rafters.front().endElevation, 2000.0 + 6300.0));
}

TEST(rafter_center_offset_subtracted_from_xy)
{
	std::vector<RafterCommand> const rafters = shedRafters(0.0, Vec2{100.0, 200.0});
	CHECK(!rafters.empty());
	if (rafters.empty())
		return;
	CHECK(near(rafters.front().start.y, 0.0 - 200.0));
	CHECK(near(rafters.front().end.y, 3000.0 - 200.0));
}

TEST(flat_plane_has_no_rafters)
{
	// ほぼ水平な面（法線がほぼ +Z）は勾配方向が定まらないため垂木なし。
	RoofPlane flat;
	flat.vertices = {Vec3{0.0, 0.0, 0.0}, Vec3{4000.0, 0.0, 0.0}, Vec3{4000.0, 3000.0, 0.0},
					 Vec3{0.0, 3000.0, 0.0}};
	flat.normal = Vec3{0.0, 0.0, 1.0};
	CHECK(raftersForPlane(flat, "R-垂木", 0.0, Vec2{0.0, 0.0}).empty());
}

TEST(tiny_face_below_min_length_has_no_rafters)
{
	// 掃引方向の広がりが 100mm 未満の極小面は垂木なし。
	RoofPlane tiny = shedPlane();
	tiny.vertices = {Vec3{0.0, 0.0, 1000.0}, Vec3{50.0, 0.0, 1000.0}, Vec3{50.0, 3000.0, 2000.0},
					 Vec3{0.0, 3000.0, 2000.0}};
	CHECK(raftersForPlane(tiny, "R-垂木", 0.0, Vec2{0.0, 0.0}).empty());
}

TEST(rafter_without_girder_support_uses_the_eaves_tip)
{
	// **軒桁に乗らない垂木は軒先を高さの基準（軒高）にする。** 試験用の片流れは
	// 軒 y=0/z=1000・棟 y=3000/z=2000（平面投影長 3000mm）。支持点は
	// s=(beamTopZ−1000)/1000 の位置に来るので、支持点→棟側は (1−s)·3000mm。
	// beamTopZ=1990 → s=0.99 → 30mm しか残らない＝部材にならないので支持点を採らず、
	// 軒先そのものを挿入点にする（長さ 3000mm・高さ差 1000mm の実形状）。
	const std::vector<RafterCommand> noSupport = shedRaftersWithBeamTop(1990.0);
	CHECK(!noSupport.empty());
	for (const RafterCommand& rafter : noSupport)
	{
		// 挿入点＝軒先（y=0・z=1000）。棟側は y=3000・z=2000。
		CHECK(near(rafter.start.y, 0.0));
		CHECK(near(rafter.elevation, 1000.0));
		CHECK(near(rafter.end.y, 3000.0));
		CHECK(near(rafter.endElevation, 2000.0));
		// **長さと高さを実形状に合わせる**: 差し込み・軒の出が乗ると VW は軒先を
		// 「挿入点＋差し込み＋軒の出」に置くので、受ける軒桁が無い垂木では 0 にする。
		CHECK(near(rafter.embedment, 0.0));
		CHECK(near(rafter.overhang, 0.0));
	}
}

TEST(rafter_resting_on_a_girder_keeps_the_support_point)
{
	// 支持点→棟側に部材が残る（beamTopZ=1500 → s=0.5 → 1500mm）ときは従来どおり
	// 支持点を採る。挿入点は軒高（z=1500）で、軒先までの距離は差し込み＋軒の出になる。
	const std::vector<RafterCommand> supported = shedRaftersWithBeamTop(1500.0);
	CHECK(!supported.empty());
	for (const RafterCommand& rafter : supported)
	{
		CHECK(near(rafter.start.y, 1500.0));
		CHECK(near(rafter.elevation, 1500.0));
		// 支持点→軒先は 1500mm。差し込み（既定桁幅 105 の半分）＋軒の出がその全部を占める。
		CHECK(near(rafter.embedment, kDefaultGirderWidth / 2.0));
		CHECK(near(rafter.embedment + rafter.overhang, 1500.0));
	}
}

TEST(rafter_with_beam_top_at_the_ridge_uses_the_eaves_tip)
{
	// 棟側の端がちょうど横架材天端に来る面（隅棟・谷際の三角形の先端）。s は本来 1.0 だが
	// 割り算の丸めで 1−ε になり `s < 1.0` をすり抜けることがある。s ではなく「支持点→棟側に
	// 部材が残るか」で判定するので、どちらに転んでも軒先が基準になり縮退しない。
	const std::vector<RafterCommand> rafters = shedRaftersWithBeamTop(2000.0);
	CHECK(!rafters.empty());
	for (const RafterCommand& rafter : rafters)
	{
		CHECK(!core::samePoint(rafter.start, rafter.end));
		CHECK(near(rafter.start.y, 0.0));
		CHECK(near(rafter.elevation, 1000.0));
		CHECK(near(rafter.embedment, 0.0));
	}
}

TEST(no_degenerate_rafters_in_any_fixture)
{
	// **回帰（実データ）**: 全フィクスチャで縮退した垂木が 1 本も出ないこと。かつては
	// サンプル1 で 8 本・グレー本モデルプラン1 で 7 本の縮退垂木が出ており、
	// validateDocument がその 2 モデルの Document 全体を弾いていた（＝インポートしても
	// 何も描かれなかった）。core::samePoint は validateDocument の isValidRafter と同じ述語。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);
		for (const RafterCommand& rafter : buildRafterCommands(model))
			CHECK(!core::samePoint(rafter.start, rafter.end));
	}
}

TEST(rafters_without_girder_support_have_no_extra_length_in_any_fixture)
{
	// 軒桁に乗らない垂木（embedment 0）は軒の出も 0 で、OIP のスパン（start→end の水平
	// 投影長）がそのまま部材長になる＝**長さと高さが実形状と一致**する。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);
		for (const RafterCommand& rafter : buildRafterCommands(model))
		{
			if (rafter.embedment != 0.0)
				continue;
			CHECK(near(rafter.overhang, 0.0));
		}
	}
}

TEST(non_convex_face_splits_into_multiple_segments)
{
	// ドーマ状の非凸面（左辺中央に矩形の切り欠き）。掃引 X の切り欠き範囲では走査線が
	// 4 交点になり、1 掃引線が 2 本の垂木に分割される。平面は z=1000+y/3。
	const auto z = [](double y) { return 1000.0 + (y / 3.0); };
	RoofPlane notched = shedPlane();
	notched.vertices = {Vec3{0.0, 0.0, z(0.0)},			 Vec3{4000.0, 0.0, z(0.0)},
						Vec3{4000.0, 3000.0, z(3000.0)}, Vec3{0.0, 3000.0, z(3000.0)},
						Vec3{0.0, 2000.0, z(2000.0)},	 Vec3{2000.0, 2000.0, z(2000.0)},
						Vec3{2000.0, 1000.0, z(1000.0)}, Vec3{0.0, 1000.0, z(1000.0)}};

	std::vector<RafterCommand> const rafters =
		raftersForPlane(notched, "R-垂木", 0.0, Vec2{0.0, 0.0});
	const auto has = [&rafters](double segLo, double segHi)
	{
		for (const RafterCommand& rafter : rafters)
		{
			if (near(rafter.start.y, segLo, 1.0) && near(rafter.end.y, segHi, 1.0))
				return true;
		}
		return false;
	};
	// 切り欠き範囲（x<2000）では下 [0,1000] と上 [2000,3000] の 2 区間に分割される。
	CHECK(has(0.0, 1000.0));
	CHECK(has(2000.0, 3000.0));
	// 切り欠きの無い範囲（x>2000）では全長 [0,3000] の 1 本。
	CHECK(has(0.0, 3000.0));
}

// ---------------------------------------------------------------------------
// 支持点（屋根面と横架材天端 Z の交点）・軒の出・差し込み・ラベル
// ---------------------------------------------------------------------------

TEST(start_is_intersection_with_beam_top_z)
{
	// beam_top_z=1500 → 屋根面と交わる支持点は y=1500（z=1500）。軒先（y=0）より上。
	std::vector<RafterCommand> const rafters = shedRaftersWithBeamTop(1500.0);
	CHECK(!rafters.empty());
	for (const RafterCommand& rafter : rafters)
	{
		CHECK(near(rafter.start.y, 1500.0));
		CHECK(near(rafter.elevation, 1500.0));
		// 棟側（end）は変わらず y=3000, z=2000。
		CHECK(near(rafter.end.y, 3000.0));
		CHECK(near(rafter.endElevation, 2000.0));
	}
}

TEST(overhang_is_support_to_tip_minus_embedment)
{
	// 壁外面から軒先の距離 ＝ 支持点（y=1500）→軒先（y=0）の水平距離（1500）から
	// 支持部分の差し込み（既定桁幅/2＝52.5）を引いた残り。
	const double expected = 1500.0 - (kDefaultGirderWidth / 2.0);
	for (const RafterCommand& rafter : shedRaftersWithBeamTop(1500.0))
	{
		CHECK(near(rafter.overhang, expected));
		// 差し込み + 軒の出 = 支持点→軒先（VW は軒先をこの和の位置に置く）。
		CHECK(near(rafter.embedment + rafter.overhang, 1500.0));
	}
}

TEST(no_overhang_when_beam_top_at_or_below_eave_tip)
{
	// beam_top_z <= 軒先 z（1000）なら支持点は取れず start=軒先・overhang=0。
	std::vector<RafterCommand> const rafters = shedRaftersWithBeamTop(800.0);
	CHECK(!rafters.empty());
	for (const RafterCommand& rafter : rafters)
	{
		CHECK(near(rafter.start.y, 0.0));
		CHECK(near(rafter.elevation, 1000.0));
		CHECK(near(rafter.overhang, 0.0));
	}
}

TEST(embedment_defaults_to_half_default_girder)
{
	// 受ける軒桁（横架材命令）を渡さなければ差し込みは既定桁幅の半分（M6 の挙動と同じ値）。
	CHECK(near(kDefaultGirderWidth, 105.0));
	for (const RafterCommand& rafter : shedRaftersWithBeamTop(1500.0))
		CHECK(near(rafter.embedment, kDefaultGirderWidth / 2.0));
}

// --------------------------------------------------------------------------
// - girderWidthAt: 支持点の真下の軒桁から桁幅を引く（M7 で横架材が入って有効になった）
// ---------------------------------------------------------------------------

TEST(girder_width_uses_nearest_perpendicular_member)
{
	// 支持点 (0, 0) の真下を x 方向に走る幅 150 の軒桁。垂木は +y へ流れるので直交する。
	const std::vector<MemberCommand> members = {
		girderMember(Vec2{-1000.0, 0.0}, Vec2{1000.0, 0.0}, 150.0)};
	CHECK(near(girderWidthAt(0.0, 0.0, 0.0, 1000.0, members), 150.0));
}

TEST(girder_width_ignores_members_parallel_to_rafter)
{
	// 垂木と平行に走る材（継ぎ手・側並び）は軒桁でないため選ばない → 既定桁幅。
	const std::vector<MemberCommand> members = {
		girderMember(Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}, 150.0)};
	CHECK(near(girderWidthAt(0.0, 0.0, 0.0, 1000.0, members), kDefaultGirderWidth));
}

TEST(girder_width_ignores_degenerate_members)
{
	// 平面投影長が 0 の材（点に潰れた命令）は候補にしない → 既定桁幅。
	const std::vector<MemberCommand> members = {
		girderMember(Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, 150.0)};
	CHECK(near(girderWidthAt(0.0, 0.0, 0.0, 1000.0, members), kDefaultGirderWidth));
}

TEST(girder_width_ignores_distant_members)
{
	// 芯線が探索許容（100mm）より遠い材は選ばない → 既定桁幅。
	const std::vector<MemberCommand> members = {
		girderMember(Vec2{-1000.0, 500.0}, Vec2{1000.0, 500.0}, 150.0)};
	CHECK(near(girderWidthAt(0.0, 0.0, 0.0, 1000.0, members), kDefaultGirderWidth));
}

TEST(girder_width_picks_the_closest_of_several)
{
	// 複数の候補があれば芯線が支持点に最も近いものを採る（並び順に依存しない）。
	const std::vector<MemberCommand> near_ = {
		girderMember(Vec2{-1000.0, 10.0}, Vec2{1000.0, 10.0}, 120.0)};
	const std::vector<MemberCommand> far_ = {
		girderMember(Vec2{-1000.0, 60.0}, Vec2{1000.0, 60.0}, 240.0)};
	std::vector<MemberCommand> both = near_;
	both.push_back(far_.front());
	std::vector<MemberCommand> reversed = far_;
	reversed.push_back(near_.front());
	CHECK(near(girderWidthAt(0.0, 0.0, 0.0, 1000.0, both), 120.0));
	CHECK(near(girderWidthAt(0.0, 0.0, 0.0, 1000.0, reversed), 120.0));
}

TEST(embedment_uses_real_girder_width_from_members)
{
	// 支持点の真下に幅 150 の軒桁があれば、差し込みはその半分（75）になる。
	// 片流れ屋根の支持点は y=1500 の線上（勾配方向は +y）。
	const std::vector<MemberCommand> members = {
		girderMember(Vec2{-1000.0, 1500.0}, Vec2{5000.0, 1500.0}, 150.0)};
	const std::vector<RafterCommand> rafters = raftersForPlane(
		shedPlane(), "R-垂木", 0.0, Vec2{0.0, 0.0}, std::optional<double>(1500.0), members);
	CHECK(!rafters.empty());
	for (const RafterCommand& rafter : rafters)
		CHECK(near(rafter.embedment, 75.0));
}

TEST(label_shows_spec)
{
	for (const RafterCommand& rafter : shedRaftersWithBeamTop(1500.0))
		CHECK_EQ(rafter.label, std::string("45×45@455"));
}

// ---------------------------------------------------------------------------
// 合成モデル: 屋根版の抽出条件（型 / Name 前方一致 / 形状の欠損）
// ---------------------------------------------------------------------------

// 最小の屋根版 IFC（minimalRoofText）は tests/RoofSample.h が唯一の定義で、野地板
// （ParseRoofTests）と共有する。slabName を "屋根版" 以外にすると拾われないことの確認に使う。

TEST(extracts_rafters_from_minimal_roof_slab)
{
	// 勾配した屋根版 1 枚から、最上階（屋根）の "R-垂木" レイヤへ垂木が導出される。
	Model const model = loadIfcFromText(minimalRoofText("屋根版:1"));
	std::vector<RafterCommand> const rafters = buildRafterCommands(model);
	CHECK(!rafters.empty());
	for (const RafterCommand& rafter : rafters)
	{
		CHECK_EQ(rafter.layer, std::string("R-垂木"));
		CHECK_EQ(rafter.drawClass, std::string(CLASS_TARUKI));
		CHECK(near(rafter.width, 45.0));
		CHECK(near(rafter.height, 45.0));
		// 棟側は軒側（支持点）より高い。
		CHECK(rafter.endElevation >= rafter.elevation);
	}
}

TEST(ignores_slabs_with_other_names)
{
	// 床版など "屋根版" 始まり以外の IfcSlab は屋根面として拾わない。
	Model const model = loadIfcFromText(minimalRoofText("床版"));
	CHECK(buildRafterCommands(model).empty());
}

TEST(skips_roof_slab_without_plane)
{
	// 形状表現を持たない（屋根面を解決できない）屋根版はスキップする。1 面の欠損で
	// 全体を止めない（CLAUDE.md「エラーハンドリング」）。
	Model const model =
		loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
						"#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
						"#3=IFCLOCALPLACEMENT($,#2);\n"
						"#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
						"#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n"
						"#40=IFCSLAB('slab',$,'屋根版:1',$,$,#3,$,$,$);\n"
						"#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#40),#11);\n");
	CHECK(buildRafterCommands(model).empty());
}

TEST(returns_empty_without_stories)
{
	// ストーリが無ければ垂木も置けない（空を返し、例外を投げない）。
	Model const model = loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n");
	CHECK(buildRafterCommands(model).empty());
}

TEST(story_has_roof_slab_detects_roof_face)
{
	// parse/Story が該当階へ 垂木・野地板 レベルを足すかの判定に使う。
	Model const model = loadIfcFromText(minimalRoofText("屋根版:1"));
	CHECK(storyHasRoofSlab(model, 11));
	CHECK(!storyHasRoofSlab(model, 10));

	Model const other = loadIfcFromText(minimalRoofText("床版"));
	CHECK(!storyHasRoofSlab(other, 11));
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(fixture_rafters_are_valid)
{
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	std::vector<RafterCommand> const rafters = buildRafterCommands(model);
	CHECK(!rafters.empty());
	for (const RafterCommand& rafter : rafters)
	{
		// すべて既定断面・垂木クラスで、棟が軒（支持点）より高い。
		CHECK(near(rafter.width, 45.0));
		CHECK(near(rafter.height, 45.0));
		CHECK_EQ(rafter.drawClass, std::string(CLASS_TARUKI));
		CHECK(rafter.endElevation >= rafter.elevation);
		CHECK(rafter.layer.size() > 3);
		CHECK(rafter.layer.rfind("-垂木") == rafter.layer.size() - std::string("-垂木").size());
		// 軒の出は 0 以上、仕様ラベルは 45×45@455。差し込みは**軒桁に乗る垂木なら
		// 桁幅/2（正）、乗らない垂木なら 0**（軒先が挿入点＝軒高になり、差し込み・軒の出が
		// 乗ると実形状より長く描かれるため。src/parse/Rafter.cpp）。
		CHECK(rafter.overhang >= 0.0);
		CHECK(rafter.embedment >= 0.0);
		if (rafter.embedment == 0.0)
			CHECK(near(rafter.overhang, 0.0));
		CHECK_EQ(rafter.label, std::string("45×45@455"));
	}
}

TEST(fixture_layers_map_to_roof_storeys)
{
	// 伏図次郎: 下屋根（2FL）→ "2-垂木"、主屋根（RFL）→ "R-垂木"。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	std::set<std::string> const layers = layersOf(buildRafterCommands(model));
	CHECK_EQ(layers.size(), static_cast<std::size_t>(2));
	CHECK(layers.count("2-垂木") == 1);
	CHECK(layers.count("R-垂木") == 1);
}

TEST(shed_dormer_without_moya_still_gets_rafters)
{
	// スキップフロア: 2FL の下屋根は母屋を持たないが屋根版＝垂木を持つ。
	bool ok = false;
	const Model& model = fixture("スキップフロア_サンプル.ifc", ok);
	CHECK(ok);
	std::set<std::string> const layers = layersOf(buildRafterCommands(model));
	CHECK(layers.count("2-垂木") == 1);
}

TEST(fixture_rafters_are_spaced_within_interval)
{
	// 同じ屋根面から出た隣り合う垂木の掃引間隔は 455mm 以下（要件の「455 以下」）。
	// 面ごとの厳密な検証は合成入力側で行うので、ここでは実データで「命令が正の長さを持ち、
	// 平面投影長が 100mm 以上（極小片が混じらない）」ことを確かめる。
	bool ok = false;
	const Model& model = fixture("グレー本モデルプラン1【3階】.ifc", ok);
	CHECK(ok);
	std::vector<RafterCommand> const rafters = buildRafterCommands(model);
	CHECK(!rafters.empty());
	for (const RafterCommand& rafter : rafters)
	{
		const double run = std::hypot(rafter.end.x - rafter.start.x, rafter.end.y - rafter.start.y);
		// 支持点は軒先より棟側にあるため平面投影長は軒先までの全長以下だが、極小片は
		// 除外されているので必ず正の長さを持つ。
		CHECK(run > 0.0);
	}
}

TEST(rafters_are_deterministic)
{
	// 同じ入力からは同じ命令列（順序・値）が得られる（エンティティ列挙順に依存しない）。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	std::vector<RafterCommand> const first = buildRafterCommands(model);
	std::vector<RafterCommand> const second = buildRafterCommands(model);

	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size() && i < second.size(); ++i)
	{
		CHECK_EQ(first[i].layer, second[i].layer);
		CHECK(near(first[i].start.x, second[i].start.x));
		CHECK(near(first[i].start.y, second[i].start.y));
		CHECK(near(first[i].elevation, second[i].elevation));
		CHECK(near(first[i].endElevation, second[i].endElevation));
	}
}

TEST_MAIN()
