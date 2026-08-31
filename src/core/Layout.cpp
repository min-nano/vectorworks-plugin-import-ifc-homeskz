//
//	core/Layout.cpp
//
//	シートレイヤ（用紙）の割り付けの実装。意図・決まりごとは core/Layout.h を参照。
//	【SDK 非依存】ここでは VectorWorks SDK も STEP／IFC も一切参照しない。
//

#include "core/Layout.h"
#include "core/Geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace HomeskzIfcImport::core
{
	PageMarginsResolution resolvePageMargins(const PageMargins& raw, const Vec2& paper,
											 const Vec2& sheet)
	{
		PageMarginsResolution resolution;

		// 負の余白は意味を成さない（＝読めていない）。用紙いっぱいへ倒す。
		if (raw.left < 0.0 || raw.right < 0.0 || raw.bottom < 0.0 || raw.top < 0.0)
			return resolution;

		const bool haveSheet = sheet.x > 0.0 && sheet.y > 0.0;
		const double horizontal = raw.left + raw.right;
		const double vertical = raw.bottom + raw.top;

		// 「用紙 − 余白」がシートレイヤの大きさ（＝印刷可能領域）と一致するか。
		const auto matchesSheet = [&](double scale)
		{
			if (!haveSheet)
				return false;
			return std::abs((paper.x - (horizontal * scale)) - sheet.x) <= kPageMarginMatchTol &&
				   std::abs((paper.y - (vertical * scale)) - sheet.y) <= kPageMarginMatchTol;
		};

		if (horizontal <= 0.0 && vertical <= 0.0)
		{
			// ★**四辺 0 は「読めなかった」ではない**（Layout.h）。縁なし印刷ができる機種
			// では余白 0 の用紙設定が実際に選べるので、そのまま「余白なし」として受け取る。
			// 単位の突き合わせは要らない——0 はインチでも mm でも 0 なので、どちらの解釈でも
			// 同じ矩形になる。
			//
			// 例外は**シートレイヤが用紙より小さい**とき。印刷可能領域が用紙より狭いのに
			// 余白が 0 で返ったということなので、その 0 は信用しない（解釈できなかった側へ
			// 倒し、生の値を診断へ出させる）。逆にシートレイヤが読めない・用紙と同じなら、
			// 0 を疑う根拠が無いので受け取る。
			const bool contradicted = haveSheet && ((paper.x - sheet.x) > kPageMarginMatchTol ||
													(paper.y - sheet.y) > kPageMarginMatchTol);
			resolution.resolved = !contradicted;
			return resolution;
		}

		// 候補は 2 つだけ。**インチが先**（用紙まわりの長さは SDK では一貫してインチ）。
		constexpr std::array<double, 2> kUnits{kMillimetersPerInch, 1.0};
		const auto fits = [&](double scale)
		{ return (horizontal * scale) < paper.x && (vertical * scale) < paper.y; };

		// 1. 「用紙 − 余白」がシートレイヤの大きさと一致する単位。両方の候補を先に見てから
		//    2 へ落ちる（一致は「収まる」より強い根拠なので順序を混ぜない）。
		// 2. どちらとも一致しなければ、用紙に収まる方。
		double scale = 0.0;
		for (const double unit : kUnits)
		{
			if (matchesSheet(unit))
			{
				scale = unit;
				break;
			}
		}
		for (const double unit : kUnits)
		{
			if (scale > 0.0)
				break;
			if (fits(unit))
				scale = unit;
		}
		if (scale <= 0.0)
			return resolution;

		resolution.resolved = true;
		resolution.inInches = scale == kMillimetersPerInch;
		resolution.margins =
			PageMargins{raw.left * scale, raw.right * scale, raw.bottom * scale, raw.top * scale};
		return resolution;
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

	PlanLayout planLayout(const Vec2& content, const PaperArea& area, double legendWidth)
	{
		// ★**縮尺は凡例のぶんを差し引いてから決める**（要件）。用紙いっぱいで縮尺を決めて
		// しまうと、建物がギリギリの大きさのときに凡例を置く場所が残らない——凡例は図面の
		// 一部なので、置けなくなるくらいなら図を 1 段階小さく描く。差し引くのは
		// 「実測した凡例の幅＋間隔」で、凡例が 1 つも無ければ何も引かない。
		PaperArea plan = area;
		if (legendWidth > 0.0)
		{
			// 引くと潰れる（＝図の領域が無くなる）ほど凡例が広いときは引かない。0 幅の領域を
			// 渡すと fitScale がいちばん小さい図を返すだけで、かえって読めない図になる。
			if (const double width = area.width() - (legendWidth + kViewportGap); width > 0.0)
				plan.max.x = area.min.x + width;
		}

		PlanLayout layout;
		layout.scale = fitScale(content, plan.size());
		layout.plan = plan;
		// 図は**凡例のぶんを除いた領域の中央**へ置く（左端に寄せると右が間延びする）。
		layout.viewportCenter = plan.center();
		// 凡例は印刷可能領域の右上——図のために空けた帯の中で、いちばん端へ寄せる。
		layout.legendTopRight = area.max;
		return layout;
	}

	SectionLayout sectionLayout(const Vec2& content, const PaperArea& area)
	{
		SectionLayout layout;
		layout.area = area;

		// **上下 2 段が縦に収まる**ことを条件に縮尺を選ぶ（要件）。段の間に間隔が 1 つ
		// 入るので、1 段に使える高さは (印刷可能領域の高さ − 間隔) ÷ 2。
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

		// 段組み全体を印刷可能領域の中央に置く（左に寄せると右が間延びする）。
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
