//
//	CoreLayoutTests.cpp
//
//	用紙の割り付け（src/core/Layout）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（実装をなぞらず、A3・A2 といった実在の用紙で手計算した値を書く）。
//
//	検証項目（docs/DEV-NOTES.md M18）:
//	  * 縮尺は**階梯の値だけ**から選ばれ、収まる中で最も大きい図（＝最小の分母）になること。
//	    収まらなければ最も小さい図（1/200）へ倒れること。
//	  * 伏図は**凡例のぶんだけ右を空けた**残りへ収まり、図の中心が空けた側にはみ出さないこと。
//	    同じ内容・同じ印刷可能領域なら**何度計算しても同じ**（用紙をめくっても図が動かない）。
//	  * 軸組図は**上下 2 段**が縦に収まる縮尺になること・1 段の枚数が用紙の幅から決まること・
//	    マスが重ならないこと・必要なシートレイヤの枚数とタイトルの連番。
//	  * 用紙の余白の解釈（resolvePageMargins）——単位がインチか mm かを「用紙 − 余白 ＝
//	    シートレイヤの大きさ」で決めること・**四辺 0 を「余白なし」として受け取る**こと
//	    （縁なし印刷ができる機種の設定。ここを「読めなかった」に倒すと誤警告になる）・
//	    そのうえで**シートレイヤが用紙より小さいときの 0 は信用しない**こと。
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
	// A3 横（420 × 297mm）いっぱいの印刷可能領域。**用紙は原点中心**（draw/DrawUtil の
	// SheetPaperArea）。割り付けが受け取るのは用紙の外形ではなく**印刷可能領域**なので、
	// 余白はここで引いておく（引かない＝余白 0 の印刷設定に相当）。
	PaperArea a3()
	{
		return PaperArea{Vec2{-210.0, -148.5}, Vec2{210.0, 148.5}};
	}

	// A2 横（594 × 420mm）いっぱい。
	PaperArea a2()
	{
		return PaperArea{Vec2{-297.0, -210.0}, Vec2{297.0, 210.0}};
	}

	// 余白のある印刷可能領域（A3 の四辺から 10mm）。**余白は仮定せず SDK から読む**ので
	// （M18）、割り付け側は「渡された矩形をそのまま使う」ことだけを保証すればよい。
	PaperArea a3Inset()
	{
		return PaperArea{Vec2{-200.0, -138.5}, Vec2{200.0, 138.5}};
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

TEST(PlanLayoutUsesTheGivenPrintableAreaAsIs)
{
	// ★**余白は割り付け側で仮定しない**（M18）。渡された矩形＝印刷可能領域がそのまま
	// 図の領域になる（凡例が無ければ丸ごと）。A3 の四辺 10mm を引いた 400 × 277mm を
	// 渡せば、その中で縮尺が決まる: 8m × 5m は 1/20（400 × 250mm。ちょうど収まる）。
	const core::PlanLayout layout = core::planLayout(Vec2{8000.0, 5000.0}, a3Inset(), 0.0);
	CHECK(layout.plan.min.x == -200.0);
	CHECK(layout.plan.max.x == 200.0);
	CHECK(layout.plan.min.y == -138.5);
	CHECK(layout.plan.max.y == 138.5);
	CHECK(layout.scale == 20.0);
}

TEST(PlanLayoutFollowsAnOffCenterPrintableArea)
{
	// 余白は左右・上下で違いうる（印刷の設定が決める）。中心のずれた矩形を渡したら、
	// 図の中心も凡例の右上も**その矩形に従う**（勝手に用紙の中心へ寄せ直さない）。
	const PaperArea shifted{Vec2{-200.0, -130.0}, Vec2{190.0, 140.0}};
	const core::PlanLayout layout = core::planLayout(Vec2{8000.0, 5000.0}, shifted, 0.0);
	CHECK(near(layout.viewportCenter.x, shifted.center().x));
	CHECK(near(layout.viewportCenter.y, shifted.center().y));
	CHECK(layout.legendTopRight.x == shifted.max.x);
	CHECK(layout.legendTopRight.y == shifted.max.y);
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
	// ★**縮尺は凡例の幅を引いてから決める**（要件）。印刷可能領域が A3 いっぱいの
	// 420 × 297mm のとき、凡例 60mm ＋ 間隔 15mm を引くと図の領域は 345 × 297mm。
	//   * 引いた領域では 9m × 5m は 1/30（300 × 166.7mm。1/25 だと 360mm で入らない）
	//   * 引かなければ 1/25 まで上がる——**凡例が縮尺を 1 段階下げる**のが意図した挙動で、
	//     こうしないとギリギリの建物で凡例の置き場所が無くなる。
	constexpr double kLegend = 60.0;
	const core::PlanLayout wide = core::planLayout(Vec2{9000.0, 5000.0}, a3(), kLegend);
	const core::PlanLayout none = core::planLayout(Vec2{9000.0, 5000.0}, a3(), 0.0);
	CHECK(wide.scale == 30.0);
	CHECK(none.scale == 25.0);

	// 図が占めてよい領域は、印刷可能領域から「凡例＋間隔」を引いた残り。
	const PaperArea area = a3();
	CHECK(near(wide.plan.max.x, area.max.x - (kLegend + core::kViewportGap)));
	CHECK(near(wide.plan.width(), area.width() - (kLegend + core::kViewportGap)));
	// 高さは削らない（凡例は横に並ぶ）。
	CHECK(wide.plan.height() == area.height());

	// 凡例は印刷可能領域の右上に付く（引いた帯の中でいちばん端）。
	CHECK(wide.legendTopRight.x == area.max.x);
	CHECK(wide.legendTopRight.y == area.max.y);
}

TEST(PlanLayoutCentersTheDrawingClearOfTheLegend)
{
	// 図は「凡例のぶんを除いた領域」の中央へ置く。ここへ置けば、どんな内容でも図と凡例は
	// 重ならない（縮尺の段階でスペースを確保してあるため）。
	constexpr double kLegend = 60.0;
	const core::PlanLayout layout = core::planLayout(Vec2{9000.0, 5000.0}, a3(), kLegend);

	const PaperArea area = a3();
	const double planRight = area.max.x - (kLegend + core::kViewportGap);
	CHECK(near(layout.viewportCenter.x, (area.min.x + planRight) / 2.0));
	CHECK(near(layout.viewportCenter.y, area.center().y));

	// 図の右端が凡例の左端を越えない＝重ならない。
	const double drawnRight = layout.viewportCenter.x + ((9000.0 / layout.scale) / 2.0);
	CHECK(drawnRight <= planRight + kEdgeTol);
	CHECK(drawnRight <= layout.legendTopRight.x - kLegend);
}

TEST(PlanLayoutKeepsTheWholeSheetWhenTheLegendWouldEatIt)
{
	// 凡例が印刷可能領域より広い（＝引くと図の領域が消える）ときは引かない。0 幅の領域を
	// 渡すといちばん小さい図が返るだけで、かえって読めない図になる。
	const core::PlanLayout layout = core::planLayout(Vec2{8000.0, 5000.0}, a3(), 420.0);
	const PaperArea area = a3();
	CHECK(layout.plan.max.x == area.max.x);
	CHECK(layout.scale == 20.0);
}

TEST(PlanLayoutWithoutLegendUsesTheWholeSheet)
{
	// 凡例を 1 つも置かなかった文書（幅 0）では引くものが無いので、印刷可能領域いっぱいで
	// 縮尺を決め、図はその**中央**へ来る。8m × 5m を 420 × 297mm へ → 1/20。
	const core::PlanLayout layout = core::planLayout(Vec2{8000.0, 5000.0}, a3(), 0.0);
	CHECK(layout.scale == 20.0);
	const PaperArea area = a3();
	CHECK(layout.plan.max.x == area.max.x);
	CHECK(near(layout.viewportCenter.x, area.center().x));
	CHECK(near(layout.viewportCenter.y, area.center().y));
}

TEST(PlanLayoutIsTheSameForEverySheet)
{
	// 同じ内容・同じ印刷可能領域なら何度計算しても同じ（伏図は全図が同じ縮尺・同じ位置。
	// 要件）。
	const core::PlanLayout first = core::planLayout(Vec2{12000.0, 9000.0}, a2(), 60.0);
	const core::PlanLayout second = core::planLayout(Vec2{12000.0, 9000.0}, a2(), 60.0);
	CHECK(first.scale == second.scale);
	CHECK(first.viewportCenter.x == second.viewportCenter.x);
	CHECK(first.viewportCenter.y == second.viewportCenter.y);
	CHECK(first.legendTopRight.x == second.legendTopRight.x);
}

TEST(SectionLayoutFitsTwoRowsOfViewports)
{
	// 1 枚 8m × 7m を A3 いっぱい（420 × 297mm）へ。2 段だと 1 段に使える高さは
	// (297 − 15) / 2 = 141mm → 1/50 で 160 × 140mm（1/30 では 233mm で 2 段に入らない）。
	const core::SectionLayout layout = core::sectionLayout(Vec2{8000.0, 7000.0}, a3());
	CHECK(layout.scale == 50.0);

	// 2 段ぶんの高さ（＋段間）が印刷可能領域に収まっている。
	const double stacked = (2.0 * layout.cell.y) + core::kViewportGap;
	CHECK(stacked <= layout.area.height());

	// 1 段の枚数は幅から決まる: (420 + 15) / (160 + 15) = 2 枚。
	CHECK(layout.columns == 2);
	CHECK(layout.perSheet() == 4);
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

	// どのマスも印刷可能領域からはみ出さない。
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
	CHECK(layout.perSheet() == 4);
	CHECK(core::sectionSheetCount(layout, 0) == 0);
	CHECK(core::sectionSheetCount(layout, 1) == 1);
	CHECK(core::sectionSheetCount(layout, 4) == 1);
	CHECK(core::sectionSheetCount(layout, 5) == 2);
	CHECK(core::sectionSheetCount(layout, 9) == 3);
}

TEST(SectionSheetTitleNumbersOnlyWhenSplit)
{
	// 1 枚に収まるなら連番を付けない。
	CHECK(core::sectionSheetTitle("軸組図", 0, 1) == "軸組図");
	// 複数枚なら 1 起点で振る。
	CHECK(core::sectionSheetTitle("軸組図", 0, 3) == "軸組図(1)");
	CHECK(core::sectionSheetTitle("軸組図", 2, 3) == "軸組図(3)");
}

TEST(PageMarginsZeroMeansNoMargin)
{
	// ★**四辺 0 は「読めなかった」ではない**（縁なし印刷ができる機種では余白 0 の用紙設定
	// が実際に選べる）。用紙とシートレイヤの大きさが同じ＝余白の入る隙が無いのだから、
	// 0 をそのまま受け取る（ここを false に倒すと、正しい設定に警告が出る）。
	const core::PageMarginsResolution resolved =
		core::resolvePageMargins(core::PageMargins{}, Vec2{420.0, 297.0}, Vec2{420.0, 297.0});
	CHECK(resolved.resolved);
	CHECK(resolved.margins.left == 0.0);
	CHECK(resolved.margins.right == 0.0);
	CHECK(resolved.margins.bottom == 0.0);
	CHECK(resolved.margins.top == 0.0);

	// シートレイヤの大きさが読めなかった（0 を渡した）ときも同じ——0 を疑う根拠が無い。
	const core::PageMarginsResolution noSheet =
		core::resolvePageMargins(core::PageMargins{}, Vec2{420.0, 297.0}, Vec2{});
	CHECK(noSheet.resolved);
}

TEST(PageMarginsZeroIsRejectedWhenTheSheetIsSmallerThanThePaper)
{
	// 印刷可能領域（シートレイヤ 414 × 291）が用紙（420 × 297）より小さいのに余白 0 が
	// 返るのは辻褄が合わない＝その 0 は信用できない。解釈できなかった側へ倒して、生の値を
	// 診断へ出させる（draw/Sheet）。
	const core::PageMarginsResolution resolved =
		core::resolvePageMargins(core::PageMargins{}, Vec2{420.0, 297.0}, Vec2{414.0, 291.0});
	CHECK(!resolved.resolved);
	CHECK(resolved.margins.left == 0.0);
}

TEST(PageMarginsReadAsMillimetersWhenTheyMatchTheSheet)
{
	// 実機の実測値（M18。mm の図面で 左2.963 右3.006 下2.963 上2.963 が返り、
	// 420 − 5.969 = 414.03 ≒ シートレイヤの 414）。mm とみなした方が一致するので mm。
	const core::PageMargins raw{2.963, 3.006, 2.963, 2.963};
	const core::PageMarginsResolution resolved =
		core::resolvePageMargins(raw, Vec2{420.0, 297.0}, Vec2{414.0, 291.0});
	CHECK(resolved.resolved);
	CHECK(!resolved.inInches);
	CHECK(near(resolved.margins.left, 2.963));
	CHECK(near(resolved.margins.top, 2.963));
}

TEST(PageMarginsReadAsInchesWhenTheyMatchTheSheet)
{
	// 図面の単位がインチなら余白もインチで返るはず。四辺 0.25 インチ（6.35mm）なら
	// 用紙 420 × 297 に対しシートレイヤは 407.3 × 284.3 になる——一致するのでインチ。
	const core::PageMargins raw{0.25, 0.25, 0.25, 0.25};
	const core::PageMarginsResolution resolved =
		core::resolvePageMargins(raw, Vec2{420.0, 297.0}, Vec2{407.3, 284.3});
	CHECK(resolved.resolved);
	CHECK(resolved.inInches);
	CHECK(near(resolved.margins.left, 6.35));
}

TEST(PageMarginsFallBackToWhicheverUnitFitsThePaper)
{
	// シートレイヤの大きさが読めない（0）ときは突き合わせができないので、用紙に収まる方を
	// 採る。0.25 はインチでもmm でも収まる → **インチ**（用紙まわりの長さは SDK では
	// 一貫してインチなので、そちらが本命）。
	const core::PageMarginsResolution inches = core::resolvePageMargins(
		core::PageMargins{0.25, 0.25, 0.25, 0.25}, Vec2{420.0, 297.0}, Vec2{});
	CHECK(inches.resolved);
	CHECK(inches.inInches);

	// 四辺 100 はインチとみなすと 5080mm となって用紙に載らない → mm。
	const core::PageMarginsResolution millimeters = core::resolvePageMargins(
		core::PageMargins{100.0, 100.0, 100.0, 100.0}, Vec2{420.0, 297.0}, Vec2{});
	CHECK(millimeters.resolved);
	CHECK(!millimeters.inInches);
	CHECK(near(millimeters.margins.left, 100.0));
}

TEST(PageMarginsRejectValuesThatCannotBeMeant)
{
	// 負の余白（読めていない）。
	const core::PageMarginsResolution negative = core::resolvePageMargins(
		core::PageMargins{-1.0, 0.0, 0.0, 0.0}, Vec2{420.0, 297.0}, Vec2{414.0, 291.0});
	CHECK(!negative.resolved);

	// どちらの単位でも用紙に載らない（＝解釈のしようがない）。用紙いっぱいで割り付ける。
	const core::PageMarginsResolution huge = core::resolvePageMargins(
		core::PageMargins{1000.0, 1000.0, 1000.0, 1000.0}, Vec2{420.0, 297.0}, Vec2{});
	CHECK(!huge.resolved);
	CHECK(huge.margins.left == 0.0);
}

TEST_MAIN();
