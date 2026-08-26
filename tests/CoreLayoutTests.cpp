//
//	CoreLayoutTests.cpp
//
//	用紙の割り付け（src/core/Layout）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（実装をなぞらず、A3・A2 といった実在の用紙で手計算した値を書く）。
//
//	検証項目（docs/DEV-NOTES.md M16）:
//	  * 縮尺は**階梯の値だけ**から選ばれ、収まる中で最も大きい図（＝最小の分母）になること。
//	    収まらなければ最も小さい図（1/200）へ倒れること。
//	  * 伏図は**凡例のぶんだけ右を空けた**残りへ収まり、図の中心が空けた側にはみ出さないこと。
//	    同じ内容・同じ用紙なら**何度計算しても同じ**（用紙をめくっても図が動かない）。
//	  * 軸組図は**上下 2 段**が縦に収まる縮尺になること・1 段の枚数が用紙の幅から決まること・
//	    マスが重ならないこと・必要なシートレイヤの枚数とタイトルの連番。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Geometry.h"
#include "core/Layout.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::PaperArea;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcTests::near;

namespace
{
	// A3 横（420 × 297mm）の用紙。**用紙は原点中心**（draw/DrawUtil の SheetPageArea）。
	PaperArea a3()
	{
		return PaperArea{Vec2{-210.0, -148.5}, Vec2{210.0, 148.5}};
	}

	// A2 横（594 × 420mm）。
	PaperArea a2()
	{
		return PaperArea{Vec2{-297.0, -210.0}, Vec2{297.0, 210.0}};
	}

	// 用紙の端との比較に持たせる遊び（mm）。マスの大きさは割り算で端数が出るので、
	// 「はみ出していない」の判定は厳密な不等号では見ない。
	constexpr double kEdgeTol = 1e-6;

	// 縮尺が階梯の値そのものか。
	bool onLadder(double scale)
	{
		return std::ranges::find(core::kScaleDenominators, scale) != core::kScaleDenominators.end();
	}
} // namespace

TEST(DrawingAreaKeepsTheMarginOnEverySide)
{
	const PaperArea area = core::drawingArea(a3());
	CHECK(area.width() == 420.0 - (2.0 * core::kSheetMargin));
	CHECK(area.height() == 297.0 - (2.0 * core::kSheetMargin));
	// 中心は動かない（四辺から同じだけ内側へ寄せる）。
	CHECK(area.center().x == 0.0);
	CHECK(area.center().y == 0.0);
}

TEST(DrawingAreaLeavesTinyPaperAlone)
{
	// 余白を引くと潰れる用紙では外形をそのまま返す（0 幅の作図域を作らない）。
	const PaperArea tiny{Vec2{0.0, 0.0}, Vec2{10.0, 10.0}};
	const PaperArea area = core::drawingArea(tiny);
	CHECK(area.width() == 10.0);
	CHECK(area.height() == 10.0);
}

TEST(FitScalePicksTheLargestDrawingThatFits)
{
	// 10m × 6m の建物を 200 × 150mm へ。1/50 なら 200 × 120mm で収まり、1/30 では
	// 333mm となって横にはみ出す → 1/50。
	CHECK(core::fitScale(Vec2{10000.0, 6000.0}, Vec2{200.0, 150.0}) == 50.0);
	// 高さで決まる場合: 1/50 なら 120mm だが使える高さが 100mm しかない → 1/75。
	CHECK(core::fitScale(Vec2{10000.0, 6000.0}, Vec2{200.0, 100.0}) == 75.0);
	// ちょうど収まる大きさは「収まる」側（境界を含む）。
	CHECK(core::fitScale(Vec2{1000.0, 1000.0}, Vec2{100.0, 100.0}) == 10.0);
}

TEST(FitScaleFallsBackToTheSmallestDrawing)
{
	// どの縮尺でも収まらない（1/200 でも 500mm 必要）→ いちばん小さい図で描く。
	CHECK(core::fitScale(Vec2{100000.0, 1000.0}, Vec2{100.0, 100.0}) == 200.0);
	// 退化した入力でも階梯の値を返す（0 除算や 0 縮尺を後段へ流さない）。
	CHECK(core::fitScale(Vec2{0.0, 0.0}, Vec2{100.0, 100.0}) == 200.0);
	CHECK(core::fitScale(Vec2{1000.0, 1000.0}, Vec2{0.0, 0.0}) == 200.0);
}

TEST(FitScaleOnlyEverReturnsLadderValues)
{
	// 1mm 刻みで用紙を変えても、返るのは必ず階梯の値（"キリの良い縮尺だけ" の要件）。
	for (int width = 20; width <= 400; ++width)
	{
		const double scale =
			core::fitScale(Vec2{8000.0, 5000.0}, Vec2{static_cast<double>(width), 200.0});
		CHECK(onLadder(scale));
	}
}

TEST(PlanLayoutSubtractsTheLegendBeforeChoosingTheScale)
{
	// ★**縮尺は凡例の幅を引いてから決める**（要件）。A3 の作図域は 390 × 267mm で、
	// 凡例 60mm ＋ 間隔 15mm を引くと図の領域は 315 × 267mm。
	//   * 引いた領域では 8m × 5m は 1/30（266.7 × 166.7mm。1/25 だと 320mm で入らない）
	//   * 引かなければ 1/25 まで上がる——**凡例が縮尺を 1 段階下げる**のが意図した挙動で、
	//     こうしないとギリギリの建物で凡例の置き場所が無くなる。
	constexpr double kLegend = 60.0;
	const core::PlanLayout wide = core::planLayout(Vec2{8000.0, 5000.0}, a3(), kLegend);
	const core::PlanLayout none = core::planLayout(Vec2{8000.0, 5000.0}, a3(), 0.0);
	CHECK(wide.scale == 30.0);
	CHECK(none.scale == 25.0);

	// 図が占めてよい領域は、作図域から「凡例＋間隔」を引いた残り。
	const PaperArea area = core::drawingArea(a3());
	CHECK(near(wide.plan.max.x, area.max.x - (kLegend + core::kViewportGap)));
	CHECK(near(wide.plan.width(), area.width() - (kLegend + core::kViewportGap)));
	// 高さは削らない（凡例は横に並ぶ）。
	CHECK(wide.plan.height() == area.height());

	// 凡例は作図域の右上に付く（引いた帯の中でいちばん端）。
	CHECK(wide.legendTopRight.x == area.max.x);
	CHECK(wide.legendTopRight.y == area.max.y);
}

TEST(PlanLayoutCentersTheDrawingClearOfTheLegend)
{
	// 図は「凡例のぶんを除いた領域」の中央へ置く。ここへ置けば、どんな内容でも図と凡例は
	// 重ならない（縮尺の段階でスペースを確保してあるため）。
	constexpr double kLegend = 60.0;
	const core::PlanLayout layout = core::planLayout(Vec2{8000.0, 5000.0}, a3(), kLegend);

	const PaperArea area = core::drawingArea(a3());
	const double planRight = area.max.x - (kLegend + core::kViewportGap);
	CHECK(near(layout.viewportCenter.x, (area.min.x + planRight) / 2.0));
	CHECK(near(layout.viewportCenter.y, area.center().y));

	// 図の右端が凡例の左端を越えない＝重ならない。
	const double drawnRight = layout.viewportCenter.x + ((8000.0 / layout.scale) / 2.0);
	CHECK(drawnRight <= planRight + kEdgeTol);
	CHECK(drawnRight <= layout.legendTopRight.x - kLegend);
}

TEST(PlanLayoutKeepsTheWholeSheetWhenTheLegendWouldEatIt)
{
	// 凡例が作図域より広い（＝引くと図の領域が消える）ときは引かない。0 幅の領域を
	// 渡すといちばん小さい図が返るだけで、かえって読めない図になる。
	const core::PlanLayout layout = core::planLayout(Vec2{8000.0, 5000.0}, a3(), 400.0);
	const PaperArea area = core::drawingArea(a3());
	CHECK(layout.plan.max.x == area.max.x);
	CHECK(layout.scale == 25.0);
}

TEST(PlanLayoutWithoutLegendUsesTheWholeSheet)
{
	// 凡例を 1 つも置かなかった文書（幅 0）では引くものが無いので、作図域いっぱいで縮尺を
	// 決め、図は作図域の**中央**へ来る。8m × 5m を A3 の作図域 390 × 267mm へ → 1/25。
	const core::PlanLayout layout = core::planLayout(Vec2{8000.0, 5000.0}, a3(), 0.0);
	CHECK(layout.scale == 25.0);
	const PaperArea area = core::drawingArea(a3());
	CHECK(layout.plan.max.x == area.max.x);
	CHECK(near(layout.viewportCenter.x, area.center().x));
	CHECK(near(layout.viewportCenter.y, area.center().y));
}

TEST(PlanLayoutIsTheSameForEverySheet)
{
	// 同じ内容・同じ用紙なら何度計算しても同じ（伏図は全図が同じ縮尺・同じ位置。要件）。
	const core::PlanLayout first = core::planLayout(Vec2{12000.0, 9000.0}, a2(), 60.0);
	const core::PlanLayout second = core::planLayout(Vec2{12000.0, 9000.0}, a2(), 60.0);
	CHECK(first.scale == second.scale);
	CHECK(first.viewportCenter.x == second.viewportCenter.x);
	CHECK(first.viewportCenter.y == second.viewportCenter.y);
	CHECK(first.legendTopRight.x == second.legendTopRight.x);
}

TEST(SectionLayoutFitsTwoRowsOfViewports)
{
	// 1 枚 8m × 7m を A3 へ。作図域は 390 × 267mm、2 段だと 1 段に使える高さは
	// (267 − 15) / 2 = 126mm → 1/75 で 106.7 × 93.3mm（1/50 では 140mm で 2 段に入らない）。
	const core::SectionLayout layout = core::sectionLayout(Vec2{8000.0, 7000.0}, a3());
	CHECK(layout.scale == 75.0);

	// 2 段ぶんの高さ（＋段間）が作図域に収まっている。
	const double stacked = (2.0 * layout.cell.y) + core::kViewportGap;
	CHECK(stacked <= layout.area.height());

	// 1 段の枚数は用紙の幅から決まる: (390 + 15) / (106.7 + 15) = 3 枚。
	CHECK(layout.columns == 3);
	CHECK(layout.perSheet() == 6);
}

TEST(SectionSlotsFillLeftToRightThenTheLowerRow)
{
	const core::SectionLayout layout = core::sectionLayout(Vec2{8000.0, 7000.0}, a3());
	const Vec2 first = core::sectionSlotCenter(layout, 0);
	const Vec2 second = core::sectionSlotCenter(layout, 1);
	const Vec2 lower = core::sectionSlotCenter(layout, layout.columns);

	// 上段は左から右へ、マスの間隔ぶんだけ離れて並ぶ。
	CHECK(second.x > first.x);
	CHECK(second.y == first.y);
	CHECK(near(second.x - first.x, layout.cell.x + core::kViewportGap));
	// 次の段は下（上下 2 段）。
	CHECK(lower.x == first.x);
	CHECK(near(first.y - lower.y, layout.cell.y + core::kViewportGap));

	// どのマスも作図域からはみ出さない。
	for (std::size_t i = 0; i < layout.perSheet(); ++i)
	{
		const Vec2 center = core::sectionSlotCenter(layout, i);
		CHECK(center.x - (layout.cell.x / 2.0) >= layout.area.min.x - kEdgeTol);
		CHECK(center.x + (layout.cell.x / 2.0) <= layout.area.max.x + kEdgeTol);
		CHECK(center.y - (layout.cell.y / 2.0) >= layout.area.min.y - kEdgeTol);
		CHECK(center.y + (layout.cell.y / 2.0) <= layout.area.max.y + kEdgeTol);
	}

	// 範囲外の索引は最後のマスへ丸める（重なっても図は残す）。
	const Vec2 last = core::sectionSlotCenter(layout, layout.perSheet() - 1);
	const Vec2 beyond = core::sectionSlotCenter(layout, layout.perSheet() + 5);
	CHECK(beyond.x == last.x);
	CHECK(beyond.y == last.y);
}

TEST(SectionSheetCountSplitsAcrossSheets)
{
	const core::SectionLayout layout = core::sectionLayout(Vec2{8000.0, 7000.0}, a3());
	CHECK(layout.perSheet() == 6);
	CHECK(core::sectionSheetCount(layout, 0) == 0);
	CHECK(core::sectionSheetCount(layout, 1) == 1);
	CHECK(core::sectionSheetCount(layout, 6) == 1);
	CHECK(core::sectionSheetCount(layout, 7) == 2);
	CHECK(core::sectionSheetCount(layout, 13) == 3);
}

TEST(SectionSheetTitleNumbersOnlyWhenSplit)
{
	// 1 枚に収まるなら連番を付けない。
	CHECK(core::sectionSheetTitle("軸組図", 0, 1) == "軸組図");
	// 複数枚なら 1 起点で振る。
	CHECK(core::sectionSheetTitle("軸組図", 0, 3) == "軸組図(1)");
	CHECK(core::sectionSheetTitle("軸組図", 2, 3) == "軸組図(3)");
}

TEST_MAIN();
