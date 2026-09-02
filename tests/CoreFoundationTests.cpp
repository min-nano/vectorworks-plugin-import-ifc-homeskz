//
//	CoreFoundationTests.cpp
//
//	基礎を 1 つの立体オブジェクトとして描くための純計算（src/core/Foundation）の単体テスト。
//	VectorWorks SDK を一切 include せず、無 SDK のテストハーネス（TestFramework.h）で走る
//	（CLAUDE.md「テスト方針」）。**期待値は手書きで持つ**。
//
//	検証項目（docs/DEV-NOTES.md M20・M17）:
//	  * 地中梁断面のパラメータ化（FoundationBeam ⇄ BeamPrism の往復。実データの非対称な
//	    台形・矩形・鉛直部つきの断面・読めない断面の外接矩形）
//	  * 代表値（面積・長さで重み付けした最頻値）と、代表値の変更を部品へ配る規則
//	  * ソリッドの組み立て（底盤・砕石・立上り・地中梁の呑み込み・床付け。縮退した部品は
//	    落とす）と平面の外形
//	  * 地中梁の呑み込み（raiseBeamPrismTop）
//	  * 床付け（捨てコン・砕石）の断面（M17 の規則をそのまま引き継ぐ）と、実フィクスチャ全件で
//	    直交する地中梁へ食い込まないこと
//	  * PIO のレコードへ保存する直列化（往復・壊れた入力の拒否）
//	実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"
#include "Fixtures.h"

#include "core/Foundation.h"
#include "parse/Footing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::BeamPrism;
using HomeskzIfcImport::core::BeddingPrism;
using HomeskzIfcImport::core::FoundationBeam;
using HomeskzIfcImport::core::FoundationCommand;
using HomeskzIfcImport::core::FoundationParams;
using HomeskzIfcImport::core::FoundationRiser;
using HomeskzIfcImport::core::FoundationSlab;
using HomeskzIfcImport::core::FoundationSolid;
using HomeskzIfcImport::core::kBeddingPerimeterMargin;
using HomeskzIfcImport::core::kGroundBeamSlabBite;
using HomeskzIfcImport::core::kSlabBeddingThickness;
using HomeskzIfcImport::core::kSlabLeanConcreteThickness;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::Vec3;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::forEachFixture;
using HomeskzIfcTests::near;

namespace
{
	constexpr const char* kLean = "z構成要素-捨てコンクリート";
	constexpr const char* kGravel = "z構成要素-砕石";

	std::vector<Vec2> rect(double x1, double y1, double x2, double y2)
	{
		return {Vec2{x1, y1}, Vec2{x2, y1}, Vec2{x2, y2}, Vec2{x1, y2}};
	}

	double minX(const std::vector<Vec2>& pts)
	{
		return std::ranges::min_element(pts, {}, &Vec2::x)->x;
	}
	double maxX(const std::vector<Vec2>& pts)
	{
		return std::ranges::max_element(pts, {}, &Vec2::x)->x;
	}
	double minY(const std::vector<Vec2>& pts)
	{
		return std::ranges::min_element(pts, {}, &Vec2::y)->y;
	}
	double maxY(const std::vector<Vec2>& pts)
	{
		return std::ranges::max_element(pts, {}, &Vec2::y)->y;
	}
	double minZ(const std::vector<Vec3>& pts)
	{
		return std::ranges::min_element(pts, {}, &Vec3::z)->z;
	}
	double maxZ(const std::vector<Vec3>& pts)
	{
		return std::ranges::max_element(pts, {}, &Vec3::z)->z;
	}

	// 頂点列に (u, v) の点が（許容 0.01mm で）含まれるか。
	bool hasVertex(const std::vector<Vec2>& profile, double u, double v)
	{
		return std::ranges::any_of(profile, [u, v](const Vec2& p)
								   { return near(p.x, u, 0.01) && near(p.y, v, 0.01); });
	}

	// 実データに倣った合成の地中梁（下端 300・天端 700・せい 140。両側とも全高の斜め部）。
	FoundationBeam beam(Vec2 start, Vec2 end, double top = -100.0)
	{
		FoundationBeam cmd;
		cmd.start = start;
		cmd.end = end;
		cmd.bottomWidth = 300.0;
		cmd.haunchLeft = 200.0;
		cmd.haunchRight = 200.0;
		cmd.haunchHeight = 140.0;
		cmd.top = top;
		cmd.depth = 140.0;
		return cmd;
	}

	// 合成の基礎命令: 底盤 1 枚（天端 50・厚 150）・立上り 2 本（幅 120 と 150）・地中梁 1 本。
	FoundationCommand sample()
	{
		FoundationCommand cmd;
		cmd.layer = "F-基礎";
		cmd.drawClass = "04構造-01基礎-02基礎スラブ";
		cmd.slabClass = "04構造-01基礎-02基礎スラブ";
		cmd.riserClass = "04構造-01基礎-03立ち上がり";
		cmd.leanConcreteClass = kLean;
		cmd.gravelClass = kGravel;
		cmd.slabs.push_back(FoundationSlab{rect(0.0, 0.0, 3640.0, 2730.0), 50.0, 150.0});
		cmd.risers.push_back(
			FoundationRiser{Vec2{0.0, 0.0}, Vec2{3640.0, 0.0}, 120.0, -100.0, 400.0});
		cmd.risers.push_back(
			FoundationRiser{Vec2{0.0, 0.0}, Vec2{0.0, 1000.0}, 150.0, -100.0, 400.0});
		cmd.beams.push_back(beam(Vec2{0.0, 1365.0}, Vec2{3640.0, 1365.0}));
		cmd.params = core::foundationBaseParams(cmd);
		return cmd;
	}

	// 床付けの断面をテストする合成の地中梁。断面は 45 度の傾斜を持つ台形（下端が幅 200mm・
	// 天端が幅 400mm・せい 100mm）で、v=0 が梁下端。45 度にしてあるので、法線方向へ 130mm
	// オフセットした位置が「水平・鉛直とも 130/√2」で手計算できる。
	BeamPrism beddingBeam()
	{
		BeamPrism cmd;
		cmd.profile = {Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}, Vec2{200.0, 100.0},
					   Vec2{-200.0, 100.0}};
		cmd.depth = 1000.0;
		cmd.origin = Vec3{0.0, 0.0, -240.0};
		cmd.azimuth = 0.0;
		return cmd;
	}

	// 断面を切り上げない（天端より十分高い）topLimit。切り上げそのものは専用のケースで見る。
	constexpr double kNoTopLimit = 1e9;

	std::vector<BeddingPrism> bedding(const BeamPrism& prism, bool low, bool high, double top)
	{
		return core::groundBeamBedding(prism, low, high, top, kLean, kGravel);
	}

	// 断面座標 (u, v) の点が単純多角形の内部にあるか（水平レイキャスト）。
	bool inProfile(const std::vector<Vec2>& profile, double u, double v)
	{
		bool inside = false;
		const std::size_t n = profile.size();
		std::size_t j = n - 1;
		for (std::size_t i = 0; i < n; ++i)
		{
			if ((profile[i].y > v) != (profile[j].y > v))
			{
				const double crossing =
					profile[i].x + ((v - profile[i].y) * (profile[j].x - profile[i].x) /
									(profile[j].y - profile[i].y));
				if (u < crossing)
					inside = !inside;
			}
			j = i;
		}
		return inside;
	}

	// ワールド点が地中梁のコンクリートの中か。
	bool insideBeam(const BeamPrism& prism, double x, double y, double z)
	{
		Vec2 axis;
		Vec2 width;
		core::beamPrismAxes(prism, axis, width);
		const double dx = x - prism.origin.x;
		const double dy = y - prism.origin.y;
		const double along = (dx * axis.x) + (dy * axis.y);
		if (along < 0.0 || along > prism.depth)
			return false;
		return inProfile(prism.profile, (dx * width.x) + (dy * width.y), z - prism.origin.z);
	}

	// 床付け 1 区間を格子状に標本化し、他の地中梁のコンクリートへ入る点があるか。
	bool beddingBitesAnyBeam(const std::vector<BeamPrism>& beams, std::size_t owner,
							 const BeddingPrism& piece)
	{
		const BeamPrism& prism = beams[owner];
		Vec2 axis;
		Vec2 width;
		core::beamPrismAxes(prism, axis, width);
		const double uLo = minX(piece.profile);
		const double uHi = maxX(piece.profile);
		const double vLo = minY(piece.profile);
		const double vHi = maxY(piece.profile);
		constexpr int kAlong = 40;
		constexpr int kAcross = 20;
		constexpr int kUp = 12;
		for (int ia = 0; ia < kAlong; ++ia)
		{
			const double along = piece.start + (piece.depth * (ia + 0.5) / kAlong);
			for (int iu = 0; iu < kAcross; ++iu)
			{
				const double u = uLo + ((uHi - uLo) * (iu + 0.5) / kAcross);
				for (int iv = 0; iv < kUp; ++iv)
				{
					const double v = vLo + ((vHi - vLo) * (iv + 0.5) / kUp);
					if (!inProfile(piece.profile, u, v))
						continue;
					const double x = prism.origin.x + (axis.x * along) + (width.x * u);
					const double y = prism.origin.y + (axis.y * along) + (width.y * u);
					const double z = prism.origin.z + v;
					for (std::size_t i = 0; i < beams.size(); ++i)
					{
						if (i != owner && insideBeam(beams[i], x, y, z))
							return true;
					}
				}
			}
		}
		return false;
	}
} // namespace

// ---------------------------------------------------------------------------
// 地中梁断面のパラメータ化（beamPrism / fitFoundationBeam）
// ---------------------------------------------------------------------------

TEST(beam_prism_builds_the_trapezoid_from_the_parameters)
{
	// 方位角 0（+X へ走る）なら幅軸 u は +Y。下端の中心線が原点なので、断面は u=±150 の
	// 下端・u=±350 の天端（せい 140・両側とも全高の斜め部）で、原点の Z は天端 − せい。
	const BeamPrism prism = core::beamPrism(beam(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}));
	CHECK(near(prism.depth, 2000.0));
	CHECK(near(prism.azimuth, 0.0));
	CHECK(near(prism.origin.z, -240.0));
	CHECK_EQ(prism.profile.size(), std::size_t{4});
	CHECK(hasVertex(prism.profile, -150.0, 0.0));
	CHECK(hasVertex(prism.profile, 150.0, 0.0));
	CHECK(hasVertex(prism.profile, 350.0, 140.0));
	CHECK(hasVertex(prism.profile, -350.0, 140.0));

	// 鉛直部つき（斜め部の高さ 40 → 鉛直部 100）は 6 頂点になる。
	FoundationBeam kinked = beam(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0});
	kinked.haunchHeight = 40.0;
	const BeamPrism withKink = core::beamPrism(kinked);
	CHECK_EQ(withKink.profile.size(), std::size_t{6});
	CHECK(hasVertex(withKink.profile, 150.0, 100.0));
	CHECK(hasVertex(withKink.profile, -150.0, 100.0));

	// 張り出しの無い側は鉛直 1 本（中間頂点を作らない）。
	FoundationBeam oneSided = beam(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0});
	oneSided.haunchRight = 0.0;
	CHECK_EQ(core::beamPrism(oneSided).profile.size(), std::size_t{4});
	CHECK(hasVertex(core::beamPrism(oneSided).profile, -150.0, 140.0));

	// 長さ 0 の地中梁は断面を持たない。
	CHECK(core::beamPrism(beam(Vec2{5.0, 5.0}, Vec2{5.0, 5.0})).profile.empty());
}

TEST(fit_reads_the_parameters_back_from_the_prism)
{
	// 台形・非対称・鉛直部つき・矩形のどれも、プリズムにして当てはめ直すと同じ値に戻る。
	std::vector<FoundationBeam> cases;
	cases.push_back(beam(Vec2{100.0, 200.0}, Vec2{100.0, 2200.0}));
	FoundationBeam asymmetric = beam(Vec2{0.0, 0.0}, Vec2{3000.0, 1000.0});
	asymmetric.haunchRight = 0.0;
	cases.push_back(asymmetric);
	FoundationBeam kinked = beam(Vec2{0.0, 0.0}, Vec2{-2000.0, 0.0});
	kinked.haunchHeight = 40.0;
	cases.push_back(kinked);
	FoundationBeam rectangular = beam(Vec2{0.0, 0.0}, Vec2{0.0, -2000.0});
	rectangular.haunchLeft = 0.0;
	rectangular.haunchRight = 0.0;
	cases.push_back(rectangular);

	for (const FoundationBeam& original : cases)
	{
		FoundationBeam fitted;
		CHECK(core::fitFoundationBeam(core::beamPrism(original), fitted));
		CHECK(near(fitted.start.x, original.start.x, 1e-6) &&
			  near(fitted.start.y, original.start.y, 1e-6));
		CHECK(near(fitted.end.x, original.end.x, 1e-6) && near(fitted.end.y, original.end.y, 1e-6));
		CHECK(near(fitted.bottomWidth, original.bottomWidth, 1e-6));
		CHECK(near(fitted.haunchLeft, original.haunchLeft, 1e-6));
		CHECK(near(fitted.haunchRight, original.haunchRight, 1e-6));
		CHECK(near(fitted.top, original.top, 1e-6));
		CHECK(near(fitted.depth, original.depth, 1e-6));
		// 矩形は斜め部を持たないので高さは全高で返る（何でもよいが決定的）。
		const bool haunched = original.haunchLeft > 0.0 || original.haunchRight > 0.0;
		CHECK(near(fitted.haunchHeight, haunched ? original.haunchHeight : original.depth, 1e-6));
	}
}

TEST(fit_handles_the_real_data_profiles)
{
	// ホームズ君の外周の地中梁: 断面原点が外面（u=0）にあり、−u 側だけへ 150 → 290 と
	// 広がる（外面は鉛直）。方位角 90（+Y へ走る）なら幅軸は −X なので、下端の中心
	// （u=−75）はワールドで +X 側へ 75 動く。
	BeamPrism prism;
	prism.profile = {Vec2{0.0, 0.0}, Vec2{-150.0, 0.0}, Vec2{-290.0, 140.0}, Vec2{0.0, 140.0}};
	prism.depth = 3000.0;
	prism.origin = Vec3{1000.0, 2000.0, -240.0};
	prism.azimuth = 90.0;

	FoundationBeam fitted;
	CHECK(core::fitFoundationBeam(prism, fitted));
	CHECK(near(fitted.bottomWidth, 150.0, 1e-6));
	CHECK(near(fitted.haunchLeft, 0.0, 1e-6));	  // +u 側（外面）は鉛直
	CHECK(near(fitted.haunchRight, 140.0, 1e-6)); // −u 側（内側）が斜め
	CHECK(near(fitted.haunchHeight, 140.0, 1e-6));
	CHECK(near(fitted.depth, 140.0, 1e-6));
	CHECK(near(fitted.top, -100.0, 1e-6));
	CHECK(near(fitted.start.x, 1075.0, 1e-6) && near(fitted.start.y, 2000.0, 1e-6));
	CHECK(near(fitted.end.x, 1075.0, 1e-6) && near(fitted.end.y, 5000.0, 1e-6));

	// 戻したプリズムは断面原点が下端の中心（u=−75）へ移るだけで、外接矩形は元と同じ。
	const BeamPrism back = core::beamPrism(fitted);
	CHECK(near(minX(back.profile) - 75.0, minX(prism.profile), 1e-6));
	CHECK(near(maxX(back.profile) - 75.0, maxX(prism.profile), 1e-6));
}

TEST(fit_falls_back_to_the_bounding_box_and_rejects_degenerate_prisms)
{
	// 天端の辺が無い三角形は外接矩形（下端幅＝u の幅・張り出し 0・せい＝v の幅）で近似する。
	BeamPrism triangle;
	triangle.profile = {Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}, Vec2{0.0, 100.0}};
	triangle.depth = 1000.0;
	triangle.origin = Vec3{0.0, 0.0, -200.0};
	FoundationBeam fitted;
	CHECK(core::fitFoundationBeam(triangle, fitted));
	CHECK(near(fitted.bottomWidth, 200.0) && near(fitted.depth, 100.0));
	CHECK(near(fitted.haunchLeft, 0.0) && near(fitted.haunchRight, 0.0));
	CHECK(near(fitted.top, -100.0));

	// 面にならない断面・押し出し長 0 は当てはまらない。
	BeamPrism flat = triangle;
	flat.profile.resize(2);
	CHECK(!core::fitFoundationBeam(flat, fitted));
	BeamPrism zero = triangle;
	zero.depth = 0.0;
	CHECK(!core::fitFoundationBeam(zero, fitted));
}

// ---------------------------------------------------------------------------
// 代表値（foundationBaseParams）と適用（applyFoundationParams）
// ---------------------------------------------------------------------------

TEST(base_params_take_the_weighted_mode_of_the_parts)
{
	FoundationCommand cmd = sample();
	// 底盤は面積で重み付け: 小さい底盤（1×1m・厚 200・天端 −90）は大きい底盤に負ける。
	cmd.slabs.push_back(FoundationSlab{rect(5000.0, 0.0, 6000.0, 1000.0), -90.0, 200.0});
	// 立上りは長さで重み付け: 150 幅は 1m しかないので 120 が代表。
	const FoundationParams params = core::foundationBaseParams(cmd);
	CHECK(near(params.slabTop, 50.0));
	CHECK(near(params.slabThickness, 150.0));
	CHECK(near(params.riserWidth, 120.0));
	CHECK(near(params.riserTop, 400.0));
	CHECK(near(params.beamDepth, 140.0));
	CHECK(near(params.haunchWidth, 200.0));
	CHECK(near(params.haunchHeight, 140.0));

	// 張り出しの無い側（鉛直な面）は斜め部の幅に数えない。矩形断面だけなら幅・高さは 0。
	FoundationCommand rectangularOnly = sample();
	rectangularOnly.beams[0].haunchLeft = 0.0;
	rectangularOnly.beams[0].haunchRight = 0.0;
	const FoundationParams none = core::foundationBaseParams(rectangularOnly);
	CHECK(near(none.haunchWidth, 0.0) && near(none.haunchHeight, 0.0));
	CHECK(near(none.beamDepth, 140.0));

	// 部品の無い項目は 0。
	CHECK(near(core::foundationBaseParams(FoundationCommand{}).slabTop, 0.0));
}

TEST(apply_params_spreads_the_difference_over_the_parts)
{
	const FoundationCommand imported = sample();
	FoundationParams edited = imported.params;
	edited.slabTop += 20.0;		  // 底盤天端 50 → 70
	edited.slabThickness += 50.0; // 厚 150 → 200 → 底面は −100 → −130（−30）
	edited.riserWidth += 30.0;	  // 120 → 150（150 の立上りは 180 に）
	edited.riserTop += 100.0;	  // 400 → 500
	edited.beamDepth += 60.0;	  // 140 → 200
	edited.haunchWidth += 50.0;	  // 200 → 250
	edited.haunchHeight += 60.0;  // 140 → 200

	const FoundationCommand result = core::applyFoundationParams(imported, edited);
	CHECK(near(result.params.slabTop, 70.0));
	CHECK(near(result.slabs[0].top, 70.0) && near(result.slabs[0].thickness, 200.0));
	// 立上り: 幅は差を保ち、天端は Δ天端、下端は底盤の底面と一緒に動く。
	CHECK(near(result.risers[0].width, 150.0) && near(result.risers[1].width, 180.0));
	CHECK(near(result.risers[0].top, 500.0));
	CHECK(near(result.risers[0].bottom, -130.0));
	// 地中梁: 天端＝底盤の底面、せい・張り出し・斜め部の高さは差。
	CHECK(near(result.beams[0].top, -130.0));
	CHECK(near(result.beams[0].depth, 200.0));
	CHECK(near(result.beams[0].haunchLeft, 250.0) && near(result.beams[0].haunchRight, 250.0));
	CHECK(near(result.beams[0].haunchHeight, 200.0));
	// 部品の数と外形は変わらない。
	CHECK_EQ(result.slabs[0].boundary.size(), std::size_t{4});
	CHECK_EQ(result.risers.size(), std::size_t{2});
}

TEST(apply_params_keeps_vertical_faces_and_clamps_the_haunch_height)
{
	// 張り出し 0 の面（外周の外面）は張り出しを変えても鉛直のまま。斜め部の高さは
	// 0〜せい にクランプする。
	FoundationCommand imported = sample();
	imported.beams[0].haunchRight = 0.0;
	imported.params = core::foundationBaseParams(imported);
	FoundationParams edited = imported.params;
	edited.haunchWidth += 100.0;
	edited.haunchHeight += 1000.0;
	FoundationCommand result = core::applyFoundationParams(imported, edited);
	CHECK(near(result.beams[0].haunchLeft, 300.0));
	CHECK(near(result.beams[0].haunchRight, 0.0));
	CHECK(near(result.beams[0].haunchHeight, result.beams[0].depth));

	edited = imported.params;
	edited.haunchWidth -= 500.0; // 負にはならない
	edited.haunchHeight -= 500.0;
	result = core::applyFoundationParams(imported, edited);
	CHECK(near(result.beams[0].haunchLeft, 0.0));
	CHECK(near(result.beams[0].haunchHeight, 0.0));

	// 代表値を変えなければ部品はそのまま。
	const FoundationCommand same = core::applyFoundationParams(imported, imported.params);
	CHECK(near(same.risers[1].width, 150.0) && near(same.beams[0].top, -100.0));
}

// ---------------------------------------------------------------------------
// ソリッドの組み立て（foundationSolids / foundationPlanShapes）
// ---------------------------------------------------------------------------

TEST(solids_cover_every_part_in_a_fixed_order)
{
	const FoundationCommand cmd = sample();
	const std::vector<FoundationSolid> solids = core::foundationSolids(cmd);
	// 底盤（コンクリート＋砕石）2・立上り 2・地中梁 1・床付け（捨てコン＋砕石）2。
	CHECK_EQ(solids.size(), std::size_t{7});
	if (solids.size() != 7)
		return;

	// 底盤のコンクリート: 外形をそのまま底面（天端 − 厚）に置き、厚みだけ真上へ押し出す。
	CHECK(solids[0].kind == FoundationSolid::Kind::Slab);
	CHECK_EQ(solids[0].drawClass, cmd.slabClass);
	CHECK_EQ(solids[0].base.size(), std::size_t{4});
	CHECK(near(minZ(solids[0].base), -100.0) && near(maxZ(solids[0].base), -100.0));
	CHECK(near(solids[0].extent.z, 150.0) && near(solids[0].extent.x, 0.0));
	// その下の砕石（130）。
	CHECK(solids[1].kind == FoundationSolid::Kind::Bedding);
	CHECK_EQ(solids[1].drawClass, std::string(kGravel));
	CHECK(near(minZ(solids[1].base), -230.0) && near(solids[1].extent.z, kSlabBeddingThickness));

	// 立上り: 壁芯の両側へ半幅の矩形を下端から天端まで。
	CHECK(solids[2].kind == FoundationSolid::Kind::Riser);
	CHECK_EQ(solids[2].drawClass, cmd.riserClass);
	CHECK_EQ(solids[2].base.size(), std::size_t{4});
	double yLo = solids[2].base.front().y;
	double yHi = yLo;
	for (const Vec3& p : solids[2].base)
	{
		yLo = std::min(yLo, p.y);
		yHi = std::max(yHi, p.y);
	}
	CHECK(near(yLo, -60.0) && near(yHi, 60.0));
	CHECK(near(minZ(solids[2].base), -100.0) && near(solids[2].extent.z, 500.0));

	// 地中梁: 断面を始点に立て、軸方向へ長さだけ押し出す。天端は呑み込み（10）ぶん高い。
	CHECK(solids[4].kind == FoundationSolid::Kind::Beam);
	CHECK_EQ(solids[4].drawClass, cmd.slabClass);
	CHECK(near(solids[4].extent.x, 3640.0) && near(solids[4].extent.y, 0.0));
	CHECK(near(minZ(solids[4].base), -240.0));
	CHECK(near(maxZ(solids[4].base), -100.0 + kGroundBeamSlabBite));

	// 床付けは素材クラスで、捨てコン → 砕石の順。
	CHECK(solids[5].kind == FoundationSolid::Kind::Bedding);
	CHECK_EQ(solids[5].drawClass, std::string(kLean));
	CHECK_EQ(solids[6].drawClass, std::string(kGravel));
	CHECK(near(minZ(solids[6].base), -240.0 - kSlabBeddingThickness));
}

TEST(solids_skip_parts_that_have_no_body)
{
	FoundationCommand cmd = sample();
	cmd.slabs[0].thickness = 0.0;
	cmd.risers[0].width = 0.0;
	cmd.risers[1].top = cmd.risers[1].bottom;
	cmd.beams[0].end = cmd.beams[0].start;
	CHECK(core::foundationSolids(cmd).empty());
	// 平面の外形は高さを持たないので、底盤の外形と（幅のある）2 本目の立上りだけが残る。
	CHECK_EQ(core::foundationPlanShapes(cmd).size(), std::size_t{2});
}

TEST(plan_shapes_outline_slabs_risers_and_beam_tops)
{
	const FoundationCommand cmd = sample();
	const std::vector<core::FoundationPlanShape> shapes = core::foundationPlanShapes(cmd);
	CHECK_EQ(shapes.size(), std::size_t{4});
	if (shapes.size() != 4)
		return;
	CHECK(shapes[0].kind == FoundationSolid::Kind::Slab);
	CHECK_EQ(shapes[0].outline.size(), std::size_t{4});
	CHECK(shapes[1].kind == FoundationSolid::Kind::Riser);
	CHECK(near(minY(shapes[1].outline), -60.0) && near(maxY(shapes[1].outline), 60.0));
	// 地中梁は天端の幅（下端 300 ＋ 両側 200 ＝ 700）。
	CHECK(shapes[3].kind == FoundationSolid::Kind::Beam);
	CHECK(near(minY(shapes[3].outline), 1365.0 - 350.0) &&
		  near(maxY(shapes[3].outline), 1365.0 + 350.0));
}

// ---------------------------------------------------------------------------
// 地中梁の呑み込み（raiseBeamPrismTop）
// ---------------------------------------------------------------------------

TEST(raise_beam_prism_top_extends_along_the_slanted_side)
{
	// 台形の天端（最大 v）だけを bite ぶん上げる。側辺は斜めなので、u も勾配ぶんずらして
	// **側面が実形状の斜面の直線延長**になるようにする（真上へ上げると勾配が変わる）。
	// 下端 (±150, 0) → 天端 (±350, 140) の側辺は「v が 140 増える間に u が 200 増える」
	// ので、bite=10 なら u は 200/140 × 10 ≈ 14.2857 ずれる。
	const BeamPrism raised =
		core::raiseBeamPrismTop(core::beamPrism(beam(Vec2{0.0, 0.0}, Vec2{2730.0, 0.0})), 10.0);
	CHECK_EQ(raised.profile.size(), std::size_t{4});
	if (raised.profile.size() != 4)
		return;
	CHECK(hasVertex(raised.profile, -150.0, 0.0));
	CHECK(hasVertex(raised.profile, 150.0, 0.0));
	const double expected = 350.0 + (200.0 / 140.0 * 10.0);
	CHECK(hasVertex(raised.profile, expected, 150.0));
	CHECK(hasVertex(raised.profile, -expected, 150.0));
	CHECK(near(raised.depth, 2730.0));
	CHECK(near(raised.origin.z, -240.0));
}

TEST(raise_beam_prism_top_moves_vertical_sides_straight_up_and_is_a_no_op_without_bite)
{
	FoundationBeam rectangular = beam(Vec2{0.0, 0.0}, Vec2{2730.0, 0.0});
	rectangular.haunchLeft = 0.0;
	rectangular.haunchRight = 0.0;
	const BeamPrism raised = core::raiseBeamPrismTop(core::beamPrism(rectangular), 10.0);
	CHECK(hasVertex(raised.profile, 150.0, 150.0));
	CHECK(hasVertex(raised.profile, -150.0, 150.0));

	const BeamPrism same = core::raiseBeamPrismTop(core::beamPrism(rectangular), 0.0);
	CHECK(near(maxY(same.profile), 140.0));
}

// ---------------------------------------------------------------------------
// 床付け（groundBeamBedding / attachBeamsToSlabs / foundationBeddings）— M17 の規則
// ---------------------------------------------------------------------------

TEST(ground_beam_bedding_wraps_the_underside)
{
	// 外周部でない地中梁の床付け: 下端の下は 捨てコン 30 + 砕石 100、傾斜部は砕石のみで
	// 法線方向に 130。45 度の傾斜なので、オフセット後の頂点は 130/√2 ずつ斜めに動く。
	const std::vector<BeddingPrism> beddings = bedding(beddingBeam(), false, false, kNoTopLimit);
	CHECK_EQ(beddings.size(), std::size_t{2});
	if (beddings.size() != 2)
		return;

	// 1 枚目＝捨てコン（下端の平らな面の直下だけ。傾斜部には無い）。
	CHECK_EQ(beddings[0].drawClass, std::string(kLean));
	CHECK_EQ(beddings[0].profile.size(), std::size_t{4});
	CHECK(hasVertex(beddings[0].profile, -100.0, -kSlabLeanConcreteThickness));
	CHECK(hasVertex(beddings[0].profile, 100.0, -kSlabLeanConcreteThickness));
	CHECK(hasVertex(beddings[0].profile, -100.0, 0.0));
	CHECK(hasVertex(beddings[0].profile, 100.0, 0.0));
	CHECK(near(beddings[0].start, 0.0) && near(beddings[0].depth, 1000.0));

	// 2 枚目＝砕石。下端の下は 130 まで、傾斜部は法線方向に 130（＝斜辺方向へ 130√2 ぶん
	// 外へ出た位置で天端に達する）。
	const std::vector<Vec2>& gravel = beddings[1].profile;
	CHECK_EQ(beddings[1].drawClass, std::string(kGravel));
	CHECK(near(minY(gravel), -kSlabBeddingThickness));
	CHECK(near(maxY(gravel), 100.0)); // 天端＝地中梁の天端（底盤の底面）まで立ち上がる
	const double diagonal = kSlabBeddingThickness * std::sqrt(2.0); // 183.848…
	CHECK(hasVertex(gravel, -(200.0 + diagonal), 100.0));
	CHECK(hasVertex(gravel, 200.0 + diagonal, 100.0));
	CHECK(hasVertex(gravel, -(100.0 + diagonal - kSlabBeddingThickness), -kSlabBeddingThickness));
	CHECK(hasVertex(gravel, 100.0 + diagonal - kSlabBeddingThickness, -kSlabBeddingThickness));
	CHECK(hasVertex(gravel, -100.0, -kSlabLeanConcreteThickness));
	CHECK(hasVertex(gravel, 100.0, -kSlabLeanConcreteThickness));
	CHECK(hasVertex(gravel, -200.0, 100.0));
	CHECK(hasVertex(gravel, 200.0, 100.0));
}

TEST(ground_beam_bedding_spills_out_at_the_perimeter)
{
	// 外周部の側面では、床付けは側面を回り込まずに 50mm 張り出して終わる（建物の外には
	// コンクリートが無いので、回り込ませると外に砕石の壁が立つ）。
	const std::vector<BeddingPrism> beddings = bedding(beddingBeam(), false, true, kNoTopLimit);
	CHECK_EQ(beddings.size(), std::size_t{2});
	if (beddings.size() != 2)
		return;

	CHECK(hasVertex(beddings[0].profile, 100.0 + kBeddingPerimeterMargin,
					-kSlabLeanConcreteThickness));
	CHECK(hasVertex(beddings[0].profile, -100.0, -kSlabLeanConcreteThickness));

	const std::vector<Vec2>& gravel = beddings[1].profile;
	CHECK(hasVertex(gravel, 100.0 + kBeddingPerimeterMargin, -kSlabBeddingThickness));
	CHECK(hasVertex(gravel, 100.0 + kBeddingPerimeterMargin, -kSlabLeanConcreteThickness));
	const double diagonal = kSlabBeddingThickness * std::sqrt(2.0);
	CHECK(!hasVertex(gravel, 200.0 + diagonal, 100.0));
	CHECK(hasVertex(gravel, -(200.0 + diagonal), 100.0));
	CHECK(near(maxY(gravel), 100.0));
}

TEST(ground_beam_bedding_is_clipped_at_the_top_limit)
{
	// topLimit より上は落とす（傾斜部の帯が直交する地中梁へ食い込むのを防ぐ切り上げ）。
	// 45 度の傾斜なので、v=50 で切ると帯の外側の端は「傾斜の下端から 130√2 − 50」になる。
	const std::vector<BeddingPrism> beddings = bedding(beddingBeam(), false, false, 50.0);
	CHECK_EQ(beddings.size(), std::size_t{2});
	if (beddings.size() != 2)
		return;
	CHECK_EQ(beddings[0].profile.size(), std::size_t{4});
	CHECK(near(maxY(beddings[0].profile), 0.0));

	const std::vector<Vec2>& gravel = beddings[1].profile;
	CHECK(near(maxY(gravel), 50.0));
	CHECK(near(minY(gravel), -kSlabBeddingThickness));
	const double diagonal = kSlabBeddingThickness * std::sqrt(2.0);
	CHECK(hasVertex(gravel, -(100.0 + diagonal + 50.0), 50.0));
	CHECK(hasVertex(gravel, -150.0, 50.0));
	CHECK(!hasVertex(gravel, -(200.0 + diagonal), 100.0));

	// 下端より下まで切り下げると、捨てコンごと落ちる（相手のコンクリートが占める高さ）。
	const std::vector<BeddingPrism> deep = bedding(beddingBeam(), false, false, -60.0);
	CHECK_EQ(deep.size(), std::size_t{1});
	if (deep.size() == 1)
	{
		CHECK_EQ(deep[0].drawClass, std::string(kGravel));
		CHECK(near(maxY(deep[0].profile), -60.0));
	}
}

TEST(ground_beam_bedding_skips_sections_it_cannot_read_and_handles_awkward_ones)
{
	BeamPrism cmd = beddingBeam();
	// 下端の辺が無い（尖った下端）／天端の辺が無い／せいが無い／面にならない。
	cmd.profile = {Vec2{0.0, 0.0}, Vec2{200.0, 100.0}, Vec2{-200.0, 100.0}};
	CHECK(bedding(cmd, false, false, kNoTopLimit).empty());
	cmd.profile = {Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}, Vec2{0.0, 100.0}};
	CHECK(bedding(cmd, false, false, kNoTopLimit).empty());
	cmd.profile = {Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}, Vec2{100.0, 0.1}, Vec2{-100.0, 0.1}};
	CHECK(bedding(cmd, false, false, kNoTopLimit).empty());
	cmd.profile = {Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}, Vec2{100.0, 0.0}};
	CHECK(bedding(cmd, false, false, kNoTopLimit).empty());

	// 末尾が始点に戻る閉じた頂点列（重複は落として扱う）。
	cmd.profile = {Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}, Vec2{200.0, 100.0}, Vec2{-200.0, 100.0},
				   Vec2{-100.0, 0.0}};
	std::vector<BeddingPrism> beddings = bedding(cmd, false, false, kNoTopLimit);
	CHECK_EQ(beddings.size(), std::size_t{2});
	// 下端が同一直線上の 2 辺に割れている（オフセット線が平行になり、マイターが求まらない）。
	cmd.profile = {Vec2{-100.0, 0.0}, Vec2{0.0, 0.0}, Vec2{100.0, 0.0}, Vec2{200.0, 100.0},
				   Vec2{-200.0, 100.0}};
	beddings = bedding(cmd, false, false, kNoTopLimit);
	CHECK_EQ(beddings.size(), std::size_t{2});
	if (beddings.size() == 2)
	{
		const double diagonal = kSlabBeddingThickness * std::sqrt(2.0);
		CHECK(hasVertex(beddings[1].profile, 100.0 + diagonal - kSlabBeddingThickness,
						-kSlabBeddingThickness));
	}
	// 天端が水平に張り出している（その辺はオフセット線を天端まで伸ばせないので端点で代用）。
	cmd.profile = {Vec2{200.0, 100.0}, Vec2{-200.0, 100.0}, Vec2{-300.0, 100.0}, Vec2{-100.0, 0.0},
				   Vec2{100.0, 0.0}};
	beddings = bedding(cmd, false, false, kNoTopLimit);
	CHECK_EQ(beddings.size(), std::size_t{2});
}

TEST(attach_beams_to_the_overlapping_slab)
{
	// 平面外形が重なる底盤へ振り分ける（重なりが無ければ重心が最も近い底盤へ）。
	const std::vector<FoundationSlab> slabs = {
		FoundationSlab{rect(0.0, 0.0, 2000.0, 2000.0), 50.0, 150.0},
		FoundationSlab{rect(5000.0, 0.0, 7000.0, 2000.0), 50.0, 150.0}};
	const std::vector<BeamPrism> beams = {
		core::beamPrism(beam(Vec2{200.0, 1000.0}, Vec2{1800.0, 1000.0})),  // 1 枚目の中
		core::beamPrism(beam(Vec2{5200.0, 1000.0}, Vec2{6800.0, 1000.0})), // 2 枚目の中
		core::beamPrism(beam(Vec2{9000.0, 1000.0}, Vec2{9500.0, 1000.0}))}; // 外。近いのは 2 枚目
	const std::vector<std::size_t> attached = core::attachBeamsToSlabs(slabs, beams);
	CHECK_EQ(attached.size(), std::size_t{3});
	if (attached.size() != 3)
		return;
	CHECK_EQ(attached[0], std::size_t{0});
	CHECK_EQ(attached[1], std::size_t{1});
	CHECK_EQ(attached[2], std::size_t{1});
	// 底盤が 1 枚も無ければ付けようがない。
	const std::vector<std::size_t> none = core::attachBeamsToSlabs({}, beams);
	CHECK(std::ranges::all_of(none, [](std::size_t i)
							  { return i == std::numeric_limits<std::size_t>::max(); }));
}

TEST(foundation_beddings_use_the_slab_outline_for_the_perimeter)
{
	// 底盤の外形の縁に乗る地中梁（外面が外形と一致）は、その側だけが外周部になる。
	// 縁の側（−u＝外面）の捨てコンは 50 張り出し、内側は張り出さない。
	FoundationCommand cmd = sample();
	cmd.beams.clear();
	FoundationBeam edge = beam(Vec2{75.0, 0.0}, Vec2{75.0, 2730.0}); // 幅軸 u＝−X。外面 x=0
	edge.bottomWidth = 150.0;
	edge.haunchLeft = 0.0; // +u（−X）側＝外面は鉛直
	edge.haunchRight = 140.0;
	cmd.beams.push_back(edge);
	// 底盤は x=0 が縁になるよう、立上りの外面合わせ後の形（x=−60 まで）ではなく壁心のまま。
	const std::vector<std::vector<BeddingPrism>> beddings = core::foundationBeddings(cmd);
	CHECK_EQ(beddings.size(), std::size_t{1});
	if (beddings.size() != 1 || beddings[0].size() < 2)
	{
		CHECK(false);
		return;
	}
	const BeddingPrism& lean = beddings[0][0];
	CHECK_EQ(lean.drawClass, std::string(kLean));
	// 断面座標: 下端の中心が u=0、外面が u=+75、内側が u=−75。外周側（+u）は 50 張り出す。
	CHECK(near(maxX(lean.profile), 75.0 + kBeddingPerimeterMargin, 0.01));
	CHECK(near(minX(lean.profile), -75.0, 0.01));

	// 底盤の砕石の底（−230）より上へは砕石の帯を立ち上げない: 帯の天端 v は
	// −230 − (−240) = 10 以下。
	for (const BeddingPrism& piece : beddings[0])
		CHECK(maxY(piece.profile) <= 10.0 + 0.01);

	// 底盤の無い基礎では外周部が無く、帯は天端まで立ち上がる。
	cmd.slabs.clear();
	const std::vector<std::vector<BeddingPrism>> free = core::foundationBeddings(cmd);
	CHECK(free.size() == 1 && free[0].size() == 2);
	if (free.size() == 1 && free[0].size() == 2)
		CHECK(near(maxY(free[0][1].profile), 140.0));
}

TEST(ground_beam_bedding_never_bites_into_a_crossing_beam)
{
	// 直交する地中梁と取り合う区間では、傾斜部の帯を相手の下端まで切り下げる（実機で
	// 「端部で直交する斜め部分の砕石が食い込む」と分かった。docs/DEV-NOTES.md M17）。
	// 実フィクスチャ全件で、床付けのどの点も他の地中梁のコンクリートの中に入らないことを見る。
	forEachFixture(failures,
				   [&](const std::string&, const parse::Model& model)
				   {
					   const std::optional<FoundationCommand> foundation =
						   parse::buildFoundationCommand(model);
					   if (!foundation.has_value())
						   return;
					   std::vector<BeamPrism> beams;
					   for (const FoundationBeam& part : foundation->beams)
						   beams.push_back(core::beamPrism(part));
					   const std::vector<std::vector<BeddingPrism>> beddings =
						   core::foundationBeddings(*foundation);
					   CHECK_EQ(beddings.size(), beams.size());
					   for (std::size_t i = 0; i < beams.size() && i < beddings.size(); ++i)
					   {
						   for (const BeddingPrism& piece : beddings[i])
						   {
							   CHECK(piece.depth > 0.0);
							   CHECK(piece.start >= -1e-6);
							   CHECK(piece.start + piece.depth <= beams[i].depth + 1e-6);
							   CHECK(!beddingBitesAnyBeam(beams, i, piece));
						   }
					   }
				   });
}

TEST(ground_beam_bedding_of_the_real_fixtures)
{
	// 実フィクスチャ: すべての地中梁に 捨てコン と 砕石 が付き、床付けの下端は梁下端から
	// 130mm 下・捨てコンは梁下端から 30mm 下。区間（start / depth）は隙間なく全長を覆い、
	// 砕石の天端は底盤の砕石の底を越えない（越えると直交する地中梁へ食い込む）。
	std::size_t checked = 0;
	std::size_t perimeter = 0;
	forEachFixture(failures,
				   [&](const std::string&, const parse::Model& model)
				   {
					   const std::optional<FoundationCommand> foundation =
						   parse::buildFoundationCommand(model);
					   if (!foundation.has_value())
						   return;
					   std::vector<BeamPrism> beams;
					   for (const FoundationBeam& part : foundation->beams)
						   beams.push_back(core::beamPrism(part));
					   const std::vector<std::size_t> slabOf =
						   core::attachBeamsToSlabs(foundation->slabs, beams);
					   const std::vector<std::vector<BeddingPrism>> beddings =
						   core::foundationBeddings(*foundation);

					   for (std::size_t i = 0; i < beams.size(); ++i)
					   {
						   const BeamPrism& prism = beams[i];
						   CHECK(beddings[i].size() >= 2);
						   if (beddings[i].size() < 2 || slabOf[i] >= foundation->slabs.size())
							   continue;
						   const FoundationSlab& slab = foundation->slabs[slabOf[i]];
						   const double beddingBottomAbs =
							   slab.top - slab.thickness - kSlabBeddingThickness;
						   const double bottomLo = -foundation->beams[i].bottomWidth / 2.0;
						   const double bottomHi = -bottomLo;

						   double leanCovered = 0.0;
						   double gravelCovered = 0.0;
						   double leanEnd = 0.0;
						   double gravelEnd = 0.0;
						   for (const BeddingPrism& piece : beddings[i])
						   {
							   const bool lean = piece.drawClass == foundation->leanConcreteClass;
							   CHECK(lean || piece.drawClass == foundation->gravelClass);
							   if (lean)
							   {
								   CHECK(near(minY(piece.profile), -kSlabLeanConcreteThickness));
								   CHECK(near(maxY(piece.profile), 0.0));
								   const double spillLow = bottomLo - minX(piece.profile);
								   const double spillHigh = maxX(piece.profile) - bottomHi;
								   CHECK(near(spillLow, 0.0, 0.01) ||
										 near(spillLow, kBeddingPerimeterMargin, 0.01));
								   CHECK(near(spillHigh, 0.0, 0.01) ||
										 near(spillHigh, kBeddingPerimeterMargin, 0.01));
								   if (spillLow > 0.0 || spillHigh > 0.0)
									   ++perimeter;
								   CHECK(piece.start >= leanEnd - 0.01);
								   leanEnd = piece.start + piece.depth;
								   leanCovered += piece.depth;
							   }
							   else
							   {
								   CHECK(near(minY(piece.profile), -kSlabBeddingThickness));
								   CHECK(maxY(piece.profile) <=
										 std::max(beddingBottomAbs - prism.origin.z, 0.0) + 0.01);
								   CHECK(near(piece.start, gravelEnd, 0.01));
								   gravelEnd = piece.start + piece.depth;
								   gravelCovered += piece.depth;
							   }
						   }
						   CHECK(near(gravelCovered, prism.depth, 0.01));
						   CHECK(leanCovered > 0.0);
						   CHECK(leanCovered <= prism.depth + 0.01);
						   ++checked;
					   }
				   });
	CHECK(checked > 0);
	CHECK(perimeter > 0);
}

// ---------------------------------------------------------------------------
// 直列化（encodeFoundation / decodeFoundation）
// ---------------------------------------------------------------------------

TEST(encode_decode_round_trips_the_parts_classes_and_params)
{
	const FoundationCommand cmd = sample();
	const std::string text = core::encodeFoundation(cmd);
	CHECK(text.starts_with("HF1;"));
	CHECK(text.find('\n') == std::string::npos); // 1 行（レコードの文字列欄に入れる）

	FoundationCommand decoded;
	CHECK(core::decodeFoundation(text, decoded));
	CHECK_EQ(decoded.slabClass, cmd.slabClass);
	CHECK_EQ(decoded.riserClass, cmd.riserClass);
	CHECK_EQ(decoded.leanConcreteClass, cmd.leanConcreteClass);
	CHECK_EQ(decoded.gravelClass, cmd.gravelClass);
	CHECK_EQ(decoded.slabs.size(), std::size_t{1});
	CHECK_EQ(decoded.risers.size(), std::size_t{2});
	CHECK_EQ(decoded.beams.size(), std::size_t{1});
	if (decoded.slabs.size() != 1 || decoded.risers.size() != 2 || decoded.beams.size() != 1)
		return;
	CHECK_EQ(decoded.slabs[0].boundary.size(), std::size_t{4});
	CHECK(near(decoded.slabs[0].boundary[2].x, 3640.0) && near(decoded.slabs[0].top, 50.0));
	CHECK(near(decoded.risers[1].width, 150.0) && near(decoded.risers[1].end.y, 1000.0));
	CHECK(near(decoded.beams[0].haunchRight, 200.0) && near(decoded.beams[0].top, -100.0));
	CHECK(near(decoded.params.riserTop, 400.0) && near(decoded.params.beamDepth, 140.0));
	// レイヤ・PIO のクラスは保存しない（PIO 自身が持つ）。
	CHECK(decoded.layer.empty() && decoded.drawClass.empty());
	// もう一度エンコードしても同じ文字列（丸めで値が動かない）。
	CHECK_EQ(core::encodeFoundation(decoded).substr(4), text.substr(4));

	// 小数は 3 桁まで保つ（0.001mm の粒度）。負の 0 は 0。
	FoundationCommand fine = sample();
	fine.risers[0].start = Vec2{-0.0, 12.3456};
	CHECK(core::decodeFoundation(core::encodeFoundation(fine), decoded));
	CHECK(near(decoded.risers[0].start.y, 12.346, 1e-9));
	CHECK(core::encodeFoundation(fine).find("-0 ") == std::string::npos);
}

TEST(decode_rejects_other_versions_and_broken_records)
{
	FoundationCommand decoded;
	decoded.layer = "untouched";
	CHECK(!core::decodeFoundation("", decoded));
	CHECK(!core::decodeFoundation("HF2;C a|b|c|d;P 0 0 0 0 0 0 0;", decoded));
	CHECK(!core::decodeFoundation("HF1;P 0 0 0 0 0 0 0;", decoded));		 // クラス無し
	CHECK(!core::decodeFoundation("HF1;C a|b|c|d;", decoded));				 // 代表値無し
	CHECK(!core::decodeFoundation("HF1;C a|b|c;P 0 0 0 0 0 0 0;", decoded)); // クラスが 3 つ
	CHECK(!core::decodeFoundation("HF1;C a|b|c|d;P 0 0 0 0 0 0;", decoded)); // 代表値が 6 つ
	CHECK(!core::decodeFoundation("HF1;C a|b|c|d;P 0 0 0 0 0 0 0;X 1;", decoded)); // 知らない項目
	CHECK(!core::decodeFoundation("HF1;C a|b|c|d;P 0 0 0 0 0 0 0;S 50 150 4 0 0 1 1;", decoded));
	CHECK(
		!core::decodeFoundation("HF1;C a|b|c|d;P 0 0 0 0 0 0 0;R 0 0 1 x 120 -100 400;", decoded));
	CHECK_EQ(decoded.layer, std::string("untouched")); // 失敗しても out は触らない

	// 最小の妥当な記録（部品無し）は通る。
	CHECK(core::decodeFoundation("HF1;C a|b|c|d;P 1 2 3 4 5 6 7;", decoded));
	CHECK(near(decoded.params.haunchHeight, 7.0));
	CHECK(decoded.slabs.empty() && decoded.risers.empty() && decoded.beams.empty());
}

TEST_MAIN()
