//
//	core/Layout.cpp
//
//	シートレイヤ（用紙）の割り付けの実装。意図・決まりごとは core/Layout.h を参照。
//	【SDK 非依存】ここでは VectorWorks SDK も STEP／IFC も一切参照しない。
//

#include "core/Layout.h"
#include "core/Geometry.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace HomeskzIfcImport::core
{
	PaperArea drawingArea(const PaperArea& page)
	{
		// 余白を引くと潰れる（＝図が 1 つも置けない）ほど小さい用紙では、外形をそのまま
		// 使う。0 幅の作図域を返すと以降の割り算がすべて意味を失う。
		if (page.width() <= 2.0 * kSheetMargin || page.height() <= 2.0 * kSheetMargin)
			return page;
		return PaperArea{Vec2{page.min.x + kSheetMargin, page.min.y + kSheetMargin},
						 Vec2{page.max.x - kSheetMargin, page.max.y - kSheetMargin}};
	}

	double fitScale(const Vec2& content, const Vec2& available)
	{
		// 階梯は昇順（図が大きくなる順）なので、最初に収まったものが「収まる中で最も大きい
		// 図」になる。**どれにも収まらなければいちばん小さい図**（末尾＝最大の分母）を返す
		// ——図がはみ出すくらいなら小さく描く。
		const double smallest = kScaleDenominators.back();
		if (content.x <= 0.0 || content.y <= 0.0 || available.x <= 0.0 || available.y <= 0.0)
			return smallest;
		for (const double scale : kScaleDenominators)
		{
			if (content.x / scale <= available.x && content.y / scale <= available.y)
				return scale;
		}
		return smallest;
	}

	PlanLayout planLayout(const Vec2& content, const PaperArea& page, double legendWidth)
	{
		const PaperArea area = drawingArea(page);

		PlanLayout layout;
		layout.plan = area;
		layout.legendTopRight = area.max;

		// ★**縮尺は凡例のぶんを差し引かずに決める**（用紙いっぱいで最大の図にする。要件）。
		// 凡例のために幅を先取りすると、そのぶん図が 1 段階小さい縮尺へ落ちてしまう
		// ——凡例は用紙の隅の空きへ置けば足りるので、図の大きさを削ってまで場所を確保しない。
		layout.scale = fitScale(content, area.size());

		// 置き場所だけが凡例を避ける。図は**左へ寄せて**右上に凡例のぶんの空きを作る:
		//   * 避けきれる（図＋間隔＋凡例が横に並ぶ）… 凡例の帯を除いた領域の中央へ置く。
		//     いたずらに左端へ寄せず、空きを図の左右へ分ける。
		//   * 避けきれない（図が広すぎる）… 左端いっぱいへ寄せる。凡例とは重なるが、
		//     **重なりが最も小さくなる置き方**になる（描画側が実測して診断へ残す）。
		const double drawn = content.x / layout.scale;
		const double avoid = legendWidth > 0.0 ? legendWidth + kViewportGap : 0.0;
		const double free = area.width() - avoid;
		const double centerX =
			drawn <= free ? area.min.x + (free / 2.0) : area.min.x + (drawn / 2.0);
		layout.viewportCenter = Vec2{centerX, area.center().y};
		return layout;
	}

	SectionLayout sectionLayout(const Vec2& content, const PaperArea& page)
	{
		SectionLayout layout;
		layout.area = drawingArea(page);

		// **上下 2 段が縦に収まる**ことを条件に縮尺を選ぶ（要件）。段の間に間隔が 1 つ
		// 入るので、1 段に使える高さは (作図域の高さ − 間隔) ÷ 2。
		const auto rows = static_cast<double>(kSectionRows);
		const double perRow = (layout.area.height() - ((rows - 1.0) * kViewportGap)) / rows;
		layout.scale = fitScale(content, Vec2{layout.area.width(), perRow});
		layout.cell = Vec2{content.x / layout.scale, content.y / layout.scale};

		// 1 段に並ぶ枚数。間隔は「枚数 − 1」個ぶんなので、幅に間隔 1 つを足してから
		// 「1 枚＋間隔」で割ると枚数になる。**必ず 1 枚は置く**（1 枚も入らない大きさでも
		// 図を捨てない。はみ出しはローカルで縮尺を見直す手掛かりになる）。
		if (layout.cell.x > 0.0)
		{
			const double fit =
				(layout.area.width() + kViewportGap) / (layout.cell.x + kViewportGap);
			if (fit >= 2.0)
				layout.columns = static_cast<std::size_t>(fit);
		}
		return layout;
	}

	Vec2 sectionSlotCenter(const SectionLayout& layout, std::size_t indexInSheet)
	{
		const std::size_t columns = std::max<std::size_t>(layout.columns, 1);
		const std::size_t slots = columns * kSectionRows;
		// 範囲外は最後のマスへ丸める（重なって置かれるが、図そのものは残る）。
		const std::size_t index = indexInSheet < slots ? indexInSheet : slots - 1;
		const std::size_t row = index / columns;
		const std::size_t column = index % columns;

		// 段組み全体を作図域の中央に置く（左に寄せると右が間延びする）。
		const double totalWidth = (static_cast<double>(columns) * layout.cell.x) +
								  (static_cast<double>(columns - 1) * kViewportGap);
		const double totalHeight = (static_cast<double>(kSectionRows) * layout.cell.y) +
								   (static_cast<double>(kSectionRows - 1) * kViewportGap);
		const Vec2 center = layout.area.center();
		const double left = center.x - (totalWidth / 2.0);
		const double top = center.y + (totalHeight / 2.0);
		return Vec2{left + (static_cast<double>(column) * (layout.cell.x + kViewportGap)) +
						(layout.cell.x / 2.0),
					top - (static_cast<double>(row) * (layout.cell.y + kViewportGap)) -
						(layout.cell.y / 2.0)};
	}

	std::size_t sectionSheetCount(const SectionLayout& layout, std::size_t viewports)
	{
		if (viewports == 0)
			return 0;
		const std::size_t perSheet = std::max<std::size_t>(layout.perSheet(), 1);
		return ((viewports + perSheet) - 1) / perSheet;
	}

	std::string sectionSheetTitle(const std::string& base, std::size_t page, std::size_t pages)
	{
		// 1 枚に収まるなら連番を付けない（"軸組図"）。複数枚のときだけ 1 起点で振る。
		if (pages <= 1)
			return base;
		return base + "(" + std::to_string(page + 1) + ")";
	}
} // namespace HomeskzIfcImport::core
