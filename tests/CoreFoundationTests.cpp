//
//	CoreFoundationTests.cpp
//
//	基礎を 1 つの立体オブジェクトとして描くための純計算（src/core/Foundation）の単体テスト。
//	VectorWorks SDK を一切 include せず、無 SDK のテストハーネス（TestFramework.h）で走る
//	（CLAUDE.md「テスト方針」）。**期待値は手書きで持つ**。
//
//	検証項目（docs/DEV-NOTES.md M20・M17）:
//	  * 地中梁の断面の当てはめ（BeamPrism → 底の面の外形と斜め寸法。実データの非対称な台形・
//	    鉛直部つきの断面・読めない断面の外接矩形）
//	  * 取り合いの高さ（外形の下に来る底盤の底面）
//	  * 代表値（外形の面積で重み付けした最頻値）と、代表値の変更をグループへ配る規則
//	  * ソリッドの組み立て（底盤・砕石・立上り・地中梁の本体と斜め部・床付け。縮退した部品は
//	    落とす／底盤の砕石が地中梁のコンクリートを避ける／斜め部は内側の辺にだけ付く）
//	  * PIO のレコードへ保存する直列化（往復・壊れた入力の拒否）
//	  * 実フィクスチャ全件でソリッドが組め、決定的であること
//	実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"
#include "Fixtures.h"

#include "core/Foundation.h"
#include "parse/Footing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::BeamPrism;
using HomeskzIfcImport::core::FoundationBeamFit;
using HomeskzIfcImport::core::FoundationBeamGroup;
using HomeskzIfcImport::core::FoundationCommand;
using HomeskzIfcImport::core::FoundationParams;
using HomeskzIfcImport::core::FoundationRiserGroup;
using HomeskzIfcImport::core::FoundationSlabGroup;
using HomeskzIfcImport::core::FoundationSolid;
using HomeskzIfcImport::core::kBeddingPerimeterMargin;
using HomeskzIfcImport::core::kGroundBeamSlabBite;
using HomeskzIfcImport::core::kSlabBeddingThickness;
using HomeskzIfcImport::core::kSlabLeanConcreteThickness;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::Vec3;
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

	// 押し出しソリッドの平面上の広がり（底の多角形＋押し出しベクトルの水平成分）。
	std::vector<Vec2> planOf(const FoundationSolid& solid)
	{
		std::vector<Vec2> pts;
		for (const Vec3& p : solid.base)
		{
			pts.push_back(Vec2{p.x, p.y});
			pts.push_back(Vec2{p.x + solid.extent.x, p.y + solid.extent.y});
		}
		return pts;
	}

	// ソリッドの Z の範囲（押し出しベクトルの鉛直成分を含む）。
	void zRange(const FoundationSolid& solid, double& low, double& high)
	{
		low = std::min(minZ(solid.base), minZ(solid.base) + solid.extent.z);
		high = std::max(maxZ(solid.base), maxZ(solid.base) + solid.extent.z);
	}

	std::size_t countOf(const std::vector<FoundationSolid>& solids, FoundationSolid::Kind kind)
	{
		return static_cast<std::size_t>(std::ranges::count_if(
			solids, [kind](const FoundationSolid& solid) { return solid.kind == kind; }));
	}

	// 台形断面の地中梁プリズム（下端幅 bottomWidth・両側の張り出し・せい 140）。断面原点は
	// 下端の中心で、v=0 が梁下端。
	BeamPrism trapezoid(double bottomWidth, double left, double right, double depth = 140.0,
						double haunchHeight = 140.0)
	{
		BeamPrism prism;
		const double half = bottomWidth / 2.0;
		const double kink = depth - haunchHeight;
		prism.profile.push_back(Vec2{-half, 0.0});
		prism.profile.push_back(Vec2{half, 0.0});
		if (left > 0.0 && kink > 0.0)
			prism.profile.push_back(Vec2{half, kink});
		prism.profile.push_back(Vec2{half + left, depth});
		prism.profile.push_back(Vec2{-half - right, depth});
		if (right > 0.0 && kink > 0.0)
			prism.profile.push_back(Vec2{-half, kink});
		prism.depth = 2000.0;
		prism.origin = Vec3{0.0, 0.0, -240.0};
		prism.azimuth = 0.0;
		return prism;
	}

	// 合成の基礎命令。底盤 1 枚（3640×2730・天端 50・厚 150）、立上り 1 本（南の縁に沿う帯・
	// 天端 400）、地中梁 1 本（底 −240・幅 300・斜め 200×140）が Y 方向に底盤を横断する。
	// 取り合いで決まる値は 底盤の底面 −100 → 立上りの下端 −100・地中梁の天端 −90（呑み込み
	// 10）・地中梁のせい 140。
	FoundationCommand sample()
	{
		FoundationCommand cmd;
		cmd.layer = "F-基礎";
		cmd.drawClass = "04構造-01基礎-02基礎スラブ";
		cmd.slabClass = "04構造-01基礎-02基礎スラブ";
		cmd.riserClass = "04構造-01基礎-03立ち上がり";
		cmd.leanConcreteClass = kLean;
		cmd.gravelClass = kGravel;
		cmd.slabs.push_back(FoundationSlabGroup{50.0, 150.0, {rect(0.0, 0.0, 3640.0, 2730.0)}});
		cmd.risers.push_back(FoundationRiserGroup{400.0, {rect(0.0, 0.0, 3640.0, 120.0)}});
		cmd.beams.push_back(
			FoundationBeamGroup{-240.0, 200.0, 140.0, {rect(1670.0, 0.0, 1970.0, 2730.0)}});
		cmd.params = core::foundationBaseParams(cmd);
		return cmd;
	}
} // namespace

// ---------------------------------------------------------------------------
// 地中梁の断面の当てはめ（fitFoundationBeam / beamFitOutline）
// ---------------------------------------------------------------------------

TEST(fit_reads_the_bottom_width_and_the_haunch_from_the_prism)
{
	// 方位角 0（+X へ走る）なら幅軸 u は +Y。下端の中心線が原点にあり、断面は u=±150 の
	// 下端・u=±350 の天端（せい 140・両側とも全高の斜め部）。
	FoundationBeamFit fit;
	CHECK(core::fitFoundationBeam(trapezoid(300.0, 200.0, 200.0), fit));
	CHECK(near(fit.bottomWidth, 300.0, 1e-6));
	CHECK(near(fit.haunchLeft, 200.0, 1e-6));
	CHECK(near(fit.haunchRight, 200.0, 1e-6));
	CHECK(near(fit.haunchHeight, 140.0, 1e-6));
	CHECK(near(fit.depth, 140.0, 1e-6));
	CHECK(near(fit.top, -100.0, 1e-6));
	CHECK(near(fit.start.x, 0.0, 1e-6) && near(fit.start.y, 0.0, 1e-6));
	CHECK(near(fit.end.x, 2000.0, 1e-6) && near(fit.end.y, 0.0, 1e-6));

	// 鉛直部つき（斜め部の高さ 40 → 鉛直部 100）も同じ値へ当てはまる。
	CHECK(core::fitFoundationBeam(trapezoid(300.0, 200.0, 200.0, 140.0, 40.0), fit));
	CHECK(near(fit.haunchHeight, 40.0, 1e-6));
	CHECK(near(fit.bottomWidth, 300.0, 1e-6));

	// 張り出しの無い矩形は斜め部を持たない（高さは全高で返る＝決定的）。
	CHECK(core::fitFoundationBeam(trapezoid(300.0, 0.0, 0.0), fit));
	CHECK(near(fit.haunchLeft, 0.0, 1e-6) && near(fit.haunchRight, 0.0, 1e-6));
	CHECK(near(fit.haunchHeight, 140.0, 1e-6));
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

	FoundationBeamFit fit;
	CHECK(core::fitFoundationBeam(prism, fit));
	CHECK(near(fit.bottomWidth, 150.0, 1e-6));
	CHECK(near(fit.haunchLeft, 0.0, 1e-6));	   // +u 側（外面）は鉛直
	CHECK(near(fit.haunchRight, 140.0, 1e-6)); // −u 側（内側）が斜め
	CHECK(near(fit.haunchHeight, 140.0, 1e-6));
	CHECK(near(fit.depth, 140.0, 1e-6));
	CHECK(near(fit.top, -100.0, 1e-6));
	CHECK(near(fit.start.x, 1075.0, 1e-6) && near(fit.start.y, 2000.0, 1e-6));
	CHECK(near(fit.end.x, 1075.0, 1e-6) && near(fit.end.y, 5000.0, 1e-6));

	// 底の面の外形は下端幅 150 × 長さ 3000 の矩形（反時計回り）。
	const std::vector<Vec2> outline = core::beamFitOutline(fit);
	CHECK_EQ(outline.size(), std::size_t{4});
	CHECK(near(minX(outline), 1000.0, 1e-6) && near(maxX(outline), 1150.0, 1e-6));
	CHECK(near(minY(outline), 2000.0, 1e-6) && near(maxY(outline), 5000.0, 1e-6));
	CHECK(core::shoelaceSigned(outline) > 0.0);
}

TEST(fit_falls_back_to_the_bounding_box_and_rejects_degenerate_prisms)
{
	// 天端の辺が無い三角形は外接矩形（下端幅＝u の幅・張り出し 0・せい＝v の幅）で近似する。
	BeamPrism triangle;
	triangle.profile = {Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}, Vec2{0.0, 100.0}};
	triangle.depth = 1000.0;
	triangle.origin = Vec3{0.0, 0.0, -200.0};
	FoundationBeamFit fit;
	CHECK(core::fitFoundationBeam(triangle, fit));
	CHECK(near(fit.bottomWidth, 200.0) && near(fit.depth, 100.0));
	CHECK(near(fit.haunchLeft, 0.0) && near(fit.haunchRight, 0.0));
	CHECK(near(fit.top, -100.0));

	// 面にならない断面・押し出し長 0 は当てはまらない。
	BeamPrism flat = triangle;
	flat.profile.resize(2);
	CHECK(!core::fitFoundationBeam(flat, fit));
	BeamPrism zero = triangle;
	zero.depth = 0.0;
	CHECK(!core::fitFoundationBeam(zero, fit));

	// 長さ 0・幅 0 の当てはめからは外形が作れない。
	FoundationBeamFit degenerate;
	degenerate.start = Vec2{10.0, 10.0};
	degenerate.end = Vec2{10.0, 10.0};
	degenerate.bottomWidth = 300.0;
	CHECK(core::beamFitOutline(degenerate).empty());
}

// ---------------------------------------------------------------------------
// 取り合いの高さ（foundationSlabBottom）
// ---------------------------------------------------------------------------

TEST(slab_bottom_comes_from_the_slab_under_the_outline)
{
	FoundationCommand cmd = sample();
	cmd.slabs.push_back(
		FoundationSlabGroup{-90.0, 200.0, {rect(5000.0, 0.0, 7000.0, 2000.0)}}); // 底面 −290

	CHECK(near(core::foundationSlabBottom(cmd, rect(100.0, 100.0, 200.0, 200.0)), -100.0));
	CHECK(near(core::foundationSlabBottom(cmd, rect(5100.0, 100.0, 5200.0, 200.0)), -290.0));
	// どの底盤にも入らない外形は重心が最も近い底盤（ここでは 2 枚目）へ付く。
	CHECK(near(core::foundationSlabBottom(cmd, rect(9000.0, 900.0, 9100.0, 1000.0)), -290.0));
	// 底盤が 1 グループも無ければ代表値から求める。
	cmd.slabs.clear();
	CHECK(near(core::foundationSlabBottom(cmd, rect(0.0, 0.0, 100.0, 100.0)),
			   cmd.params.slabTop - cmd.params.slabThickness));
}

// ---------------------------------------------------------------------------
// 代表値（foundationBaseParams）と適用（applyFoundationParams）
// ---------------------------------------------------------------------------

TEST(base_params_take_the_weighted_mode_of_the_groups)
{
	FoundationCommand cmd = sample();
	// 小さい底盤（2×2m・厚 200・天端 −90）は大きい底盤（3.64×2.73m）に負ける。
	cmd.slabs.push_back(FoundationSlabGroup{-90.0, 200.0, {rect(5000.0, 0.0, 7000.0, 2000.0)}});
	const FoundationParams params = core::foundationBaseParams(cmd);
	CHECK(near(params.slabTop, 50.0));
	CHECK(near(params.slabThickness, 150.0));
	CHECK(near(params.riserTop, 400.0));
	// せいは取り合いで決まる（底盤の底面 −100 − 底 −240）。
	CHECK(near(params.beamDepth, 140.0));
	CHECK(near(params.haunchWidth, 200.0));
	CHECK(near(params.haunchHeight, 140.0));

	// 部品の無い項目は 0（0 も「取り込み時にその部品が無かった」として正常）。
	FoundationCommand empty;
	const FoundationParams none = core::foundationBaseParams(empty);
	CHECK(near(none.slabTop, 0.0) && near(none.riserTop, 0.0) && near(none.beamDepth, 0.0));
}

TEST(apply_params_spreads_the_difference_over_the_groups)
{
	FoundationCommand cmd = sample();
	// 厚さの違う底盤（厚 200）を足して、差が配られる（一律に置き換わらない）ことを見る。
	cmd.slabs.push_back(FoundationSlabGroup{50.0, 200.0, {rect(5000.0, 0.0, 6000.0, 1000.0)}});
	cmd.risers.push_back(FoundationRiserGroup{250.0, {rect(0.0, 500.0, 1000.0, 620.0)}});
	cmd.params = core::foundationBaseParams(cmd);

	FoundationParams edited = cmd.params;
	edited.slabThickness += 50.0; // 底盤を 50 厚く（底面は 50 下がる）
	edited.slabTop += 10.0;		  // 天端を 10 上げる
	edited.riserTop += 100.0;	  // 立上りを 100 高く
	edited.beamDepth += 60.0;	  // 地中梁を 60 深く
	edited.haunchWidth -= 50.0;
	edited.haunchHeight += 20.0;
	const FoundationCommand moved = core::applyFoundationParams(cmd, edited);

	CHECK(near(moved.slabs[0].thickness, 200.0) && near(moved.slabs[0].top, 60.0));
	CHECK(near(moved.slabs[1].thickness, 250.0)); // 厚い底盤は厚いまま 50 増える
	CHECK(near(moved.risers[0].top, 500.0) && near(moved.risers[1].top, 350.0));
	// 底面は 10 − 50 = −40 動く。せいは 60 増えるので、底は −40 − 60 = −100 動く。
	CHECK(near(moved.beams[0].bottom, -340.0));
	CHECK(
		near(core::foundationSlabBottom(moved, moved.beams[0].outlines[0]) - moved.beams[0].bottom,
			 200.0));
	CHECK(near(moved.beams[0].haunchWidth, 150.0));
	CHECK(near(moved.beams[0].haunchHeight, 160.0));
	CHECK(near(moved.params.slabTop, edited.slabTop));

	// 引きすぎても負の寸法にはしない。
	FoundationParams shrunk = cmd.params;
	shrunk.slabThickness -= 1000.0;
	shrunk.haunchWidth -= 1000.0;
	shrunk.haunchHeight -= 1000.0;
	const FoundationCommand clamped = core::applyFoundationParams(cmd, shrunk);
	CHECK(near(clamped.slabs[0].thickness, 0.0));
	CHECK(near(clamped.beams[0].haunchWidth, 0.0));
	CHECK(near(clamped.beams[0].haunchHeight, 0.0));
}

// ---------------------------------------------------------------------------
// ソリッドの組み立て（foundationSolids / foundationPlanShapes）
// ---------------------------------------------------------------------------

TEST(solids_cover_every_group_in_a_fixed_order)
{
	const std::vector<FoundationSolid> solids = core::foundationSolids(sample());

	// 並びは 底盤 → 底盤の下の砕石 → 立上り → 地中梁（本体 → 斜め部 → 床付け）。
	CHECK(solids.size() >= 6);
	if (solids.size() < 6)
		return;
	CHECK(solids[0].kind == FoundationSolid::Kind::Slab);

	// 底盤: 天端 50・底面 −100。
	double low = 0.0;
	double high = 0.0;
	zRange(solids[0], low, high);
	CHECK(near(low, -100.0) && near(high, 50.0));
	CHECK_EQ(solids[0].drawClass, std::string("04構造-01基礎-02基礎スラブ"));

	// 立上り: 天端 400・下端は底盤の底面 −100（取り合い）。
	const auto riser = std::ranges::find_if(solids, [](const FoundationSolid& solid)
											{ return solid.kind == FoundationSolid::Kind::Riser; });
	CHECK(riser != solids.end());
	if (riser == solids.end())
		return;
	zRange(*riser, low, high);
	CHECK(near(low, -100.0) && near(high, 400.0));
	CHECK_EQ(riser->drawClass, std::string("04構造-01基礎-03立ち上がり"));

	// 地中梁の本体: 底 −240 から 斜め部の下端（天端 −90 − 斜め 140 = −230）まで。
	const auto beam = std::ranges::find_if(solids, [](const FoundationSolid& solid)
										   { return solid.kind == FoundationSolid::Kind::Beam; });
	CHECK(beam != solids.end());
	if (beam == solids.end())
		return;
	zRange(*beam, low, high);
	CHECK(near(low, -240.0) && near(high, -230.0));

	// 斜め部は内側の 2 辺だけ（南北の端は底盤の縁に接するので外周部＝鉛直）。斜め部の
	// ソリッドは辺に沿う押し出しなので、押し出しベクトルが水平になる。
	const std::size_t slanted = static_cast<std::size_t>(std::ranges::count_if(
		solids, [](const FoundationSolid& solid)
		{ return solid.kind == FoundationSolid::Kind::Beam && solid.extent.z == 0.0; }));
	CHECK_EQ(slanted, std::size_t{2});

	// 床付け: 捨てコンは梁下端の下 30、その下に砕石 100。
	const auto lean = std::ranges::find_if(solids, [](const FoundationSolid& solid)
										   { return solid.drawClass == kLean; });
	CHECK(lean != solids.end());
	if (lean == solids.end())
		return;
	zRange(*lean, low, high);
	CHECK(near(high, -240.0) && near(low, -240.0 - kSlabLeanConcreteThickness));
	// 外周部（南北の端）は 50 だけ張り出し、内側の辺は帯のぶん 130 広がる。
	const std::vector<Vec2> plan = planOf(*lean);
	CHECK(near(minY(plan), -kBeddingPerimeterMargin, 0.01));
	CHECK(near(minX(plan), 1670.0 - kSlabBeddingThickness, 0.01));
}

TEST(slab_gravel_avoids_the_beam_concrete)
{
	// 底盤の砕石（底面の下 130）は、地中梁の天端の外形を抜いた形になる。地中梁は底盤を
	// Y 方向に横断するので、砕石は東西 2 枚に分かれる。
	const std::vector<FoundationSolid> solids = core::foundationSolids(sample());
	std::vector<const FoundationSolid*> gravel;
	for (const FoundationSolid& solid : solids)
	{
		double low = 0.0;
		double high = 0.0;
		zRange(solid, low, high);
		if (solid.drawClass == kGravel && near(high, -100.0))
			gravel.push_back(&solid);
	}
	CHECK_EQ(gravel.size(), std::size_t{2});
	if (gravel.size() != 2)
		return;
	// 斜め部で広がった天端の幅（300 + 200×2 = 700）ぶんの帯が抜けている。
	double left = 0.0;
	double right = 0.0;
	for (const FoundationSolid* piece : gravel)
	{
		const std::vector<Vec2> plan = planOf(*piece);
		if (minX(plan) < 1.0)
			left = maxX(plan);
		else
			right = minX(plan);
		double low = 0.0;
		double high = 0.0;
		zRange(*piece, low, high);
		CHECK(near(low, -100.0 - kSlabBeddingThickness));
	}
	CHECK(near(left, 1470.0, 0.01));
	CHECK(near(right, 2170.0, 0.01));

	// 地中梁が無ければ底盤の外形いっぱいに 1 枚。
	FoundationCommand noBeam = sample();
	noBeam.beams.clear();
	const std::vector<FoundationSolid> plain = core::foundationSolids(noBeam);
	CHECK_EQ(countOf(plain, FoundationSolid::Kind::Bedding), std::size_t{1});

	// 抜くのは**その底盤に取り合う地中梁だけ**。離れた場所にある別高さの底盤の砕石は
	// 丸ごと 1 枚のまま（隣の底盤にぶら下がる梁で穴を空けない）。
	FoundationCommand twoSlabs = sample();
	twoSlabs.slabs.push_back(
		FoundationSlabGroup{-90.0, 150.0, {rect(5000.0, 0.0, 7000.0, 2730.0)}});
	twoSlabs.params = core::foundationBaseParams(twoSlabs);
	std::size_t whole = 0;
	for (const FoundationSolid& solid : core::foundationSolids(twoSlabs))
	{
		double low = 0.0;
		double high = 0.0;
		zRange(solid, low, high);
		if (solid.drawClass == kGravel && near(high, -240.0))
		{
			++whole;
			const std::vector<Vec2> plan = planOf(solid);
			CHECK(near(minX(plan), 5000.0) && near(maxX(plan), 7000.0));
		}
	}
	CHECK_EQ(whole, std::size_t{1});
}

TEST(beam_top_outline_widens_only_the_inner_edges)
{
	const FoundationCommand cmd = sample();
	const core::PolygonList slabOutlines = cmd.slabs[0].outlines;
	const std::vector<Vec2> top =
		core::beamTopOutline(cmd.beams[0].outlines[0], 200.0, slabOutlines, {});
	CHECK(near(minX(top), 1470.0, 0.01) && near(maxX(top), 2170.0, 0.01));
	CHECK(near(minY(top), 0.0, 0.01) && near(maxY(top), 2730.0, 0.01)); // 南北は外周部

	// 他の地中梁と取り合う辺も広げない（相手のコンクリートの中）。
	const core::PolygonList other = {rect(1970.0, 1000.0, 3000.0, 1300.0)};
	const std::vector<Vec2> joined =
		core::beamTopOutline(rect(1670.0, 0.0, 1970.0, 2730.0), 200.0, slabOutlines, other);
	CHECK(near(maxX(joined), 2170.0, 0.01)); // 取り合いは辺の一部だけなので東側は広がる

	// 斜め寸法が 0 なら底の面と同じ。
	CHECK_EQ(core::beamTopOutline(cmd.beams[0].outlines[0], 0.0, slabOutlines, {}).size(),
			 std::size_t{4});
}

TEST(solids_skip_groups_that_have_no_body)
{
	FoundationCommand cmd = sample();
	cmd.slabs[0].thickness = 0.0; // 厚み 0 の底盤
	cmd.risers[0].top = -100.0;	  // 底盤の底面と同じ高さ＝背の無い立上り
	cmd.beams[0].outlines.push_back({Vec2{0.0, 0.0}}); // 面にならない外形
	const std::vector<FoundationSolid> solids = core::foundationSolids(cmd);
	CHECK_EQ(countOf(solids, FoundationSolid::Kind::Slab), std::size_t{0});
	CHECK_EQ(countOf(solids, FoundationSolid::Kind::Riser), std::size_t{0});
	for (const FoundationSolid& solid : solids)
		CHECK(solid.base.size() >= 3);
}

TEST(plan_shapes_outline_slabs_risers_and_beam_tops)
{
	const std::vector<core::FoundationPlanShape> shapes = core::foundationPlanShapes(sample());
	CHECK_EQ(shapes.size(), std::size_t{3});
	if (shapes.size() != 3)
		return;
	CHECK(shapes[0].kind == FoundationSolid::Kind::Slab);
	CHECK(shapes[1].kind == FoundationSolid::Kind::Riser);
	CHECK(shapes[2].kind == FoundationSolid::Kind::Beam);
	CHECK(near(maxX(shapes[0].outline), 3640.0));
	CHECK(near(maxY(shapes[1].outline), 120.0));
	// 地中梁は斜め部で広がった天端の外形。
	CHECK(near(minX(shapes[2].outline), 1470.0, 0.01));
	CHECK_EQ(shapes[2].drawClass, std::string("04構造-01基礎-02基礎スラブ"));
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(solids_of_the_real_fixtures_are_well_formed_and_deterministic)
{
	std::size_t checked = 0;
	forEachFixture(
		failures,
		[&](const std::string&, const parse::Model& model)
		{
			const std::optional<FoundationCommand> foundation =
				parse::buildFoundationCommand(model);
			if (!foundation.has_value())
				return;
			const std::vector<FoundationSolid> solids = core::foundationSolids(*foundation);
			CHECK(!solids.empty());
			for (const FoundationSolid& solid : solids)
			{
				CHECK(solid.base.size() >= 3);
				CHECK(!solid.drawClass.empty());
				CHECK(core::length(core::Vec3{solid.extent.x, solid.extent.y, solid.extent.z}) >
					  0.0);
			}
			// 2 度組み立てても同じ（決定的）。
			const std::vector<FoundationSolid> again = core::foundationSolids(*foundation);
			CHECK_EQ(again.size(), solids.size());
			CHECK_EQ(core::encodeFoundation(*foundation), core::encodeFoundation(*foundation));
			// 地中梁のある基礎には床付けの捨てコンが付く。
			if (!foundation->beams.empty())
				CHECK(std::ranges::any_of(
					solids, [&](const FoundationSolid& solid)
					{ return solid.drawClass == foundation->leanConcreteClass; }));
			++checked;
		});
	CHECK(checked > 0);
}

// ---------------------------------------------------------------------------
// 直列化（encodeFoundation / decodeFoundation）
// ---------------------------------------------------------------------------

TEST(encode_decode_round_trips_the_groups_classes_and_params)
{
	const FoundationCommand cmd = sample();
	const std::string text = core::encodeFoundation(cmd);
	CHECK(text.starts_with("HF2;"));
	CHECK(text.find('\n') == std::string::npos); // 1 行（レコードの文字列欄に入れる）

	FoundationCommand decoded;
	CHECK(core::decodeFoundation(text, decoded));
	CHECK_EQ(decoded.slabClass, cmd.slabClass);
	CHECK_EQ(decoded.riserClass, cmd.riserClass);
	CHECK_EQ(decoded.leanConcreteClass, cmd.leanConcreteClass);
	CHECK_EQ(decoded.gravelClass, cmd.gravelClass);
	CHECK_EQ(decoded.slabs.size(), std::size_t{1});
	CHECK_EQ(decoded.risers.size(), std::size_t{1});
	CHECK_EQ(decoded.beams.size(), std::size_t{1});
	if (decoded.slabs.size() != 1 || decoded.risers.size() != 1 || decoded.beams.size() != 1)
		return;
	CHECK_EQ(decoded.slabs[0].outlines.size(), std::size_t{1});
	CHECK_EQ(decoded.slabs[0].outlines[0].size(), std::size_t{4});
	CHECK(near(decoded.slabs[0].outlines[0][2].x, 3640.0) && near(decoded.slabs[0].top, 50.0));
	CHECK(near(decoded.risers[0].top, 400.0));
	CHECK(near(decoded.beams[0].bottom, -240.0) && near(decoded.beams[0].haunchWidth, 200.0));
	CHECK(near(decoded.params.riserTop, 400.0) && near(decoded.params.beamDepth, 140.0));
	// レイヤ・PIO のクラスは保存しない（PIO 自身が持つ）。
	CHECK(decoded.layer.empty() && decoded.drawClass.empty());
	// もう一度エンコードしても同じ文字列（丸めで値が動かない）。
	CHECK_EQ(core::encodeFoundation(decoded).substr(4), text.substr(4));

	// 小数は 3 桁まで保つ（0.001mm の粒度）。負の 0 は 0。
	FoundationCommand fine = sample();
	fine.risers[0].outlines[0][0] = Vec2{-0.0, 12.3456};
	CHECK(core::decodeFoundation(core::encodeFoundation(fine), decoded));
	CHECK(near(decoded.risers[0].outlines[0][0].y, 12.346, 1e-9));
	CHECK(core::encodeFoundation(fine).find("-0 ") == std::string::npos);
}

TEST(decode_rejects_other_versions_and_broken_records)
{
	FoundationCommand decoded;
	decoded.layer = "untouched";
	CHECK(!core::decodeFoundation("", decoded));
	CHECK(!core::decodeFoundation("HF1;C a|b|c|d;P 0 0 0 0 0 0;", decoded)); // 前の版
	CHECK(!core::decodeFoundation("HF2;P 0 0 0 0 0 0;", decoded));			 // クラス無し
	CHECK(!core::decodeFoundation("HF2;C a|b|c|d;", decoded));				 // 代表値無し
	CHECK(!core::decodeFoundation("HF2;C a|b|c;P 0 0 0 0 0 0;", decoded));	 // クラスが 3 つ
	CHECK(!core::decodeFoundation("HF2;C a|b|c|d;P 0 0 0 0 0;", decoded));	 // 代表値が 5 つ
	CHECK(!core::decodeFoundation("HF2;C a|b|c|d;P 0 0 0 0 0 0;X 1;", decoded)); // 知らない項目
	// 外形の頂点が足りない／数が読めない。
	CHECK(!core::decodeFoundation("HF2;C a|b|c|d;P 0 0 0 0 0 0;S 50 150 1 4 0 0 1 1;", decoded));
	CHECK(!core::decodeFoundation("HF2;C a|b|c|d;P 0 0 0 0 0 0;R 400 x;", decoded));
	CHECK_EQ(decoded.layer, std::string("untouched")); // 失敗しても out は触らない

	// 最小の妥当な記録（部品無し）は通る。
	CHECK(core::decodeFoundation("HF2;C a|b|c|d;P 1 2 3 4 5 6;", decoded));
	CHECK(near(decoded.params.haunchHeight, 6.0));
	CHECK(decoded.slabs.empty() && decoded.risers.empty() && decoded.beams.empty());

	// 外形が 0 個のグループも書式としては妥当（描くときに落ちる）。
	CHECK(core::decodeFoundation("HF2;C a|b|c|d;P 1 2 3 4 5 6;S 50 150 0;", decoded));
	CHECK_EQ(decoded.slabs.size(), std::size_t{1});
}

TEST_MAIN()
