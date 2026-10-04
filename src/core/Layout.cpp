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
#include <numbers>
#include <cstddef>
#include <optional>
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

	PlanLayout planLayout(const Vec2& content, const PaperArea& area, double legendWidth,
						  double band)
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
		// 寸法の帯（M31）を四辺から引いた残りで縮尺を選ぶ。引くと潰れるときは引かない
		// （凡例と同じ理由）。
		Vec2 available = plan.size();
		if (band > 0.0 && available.x > 2.0 * band && available.y > 2.0 * band)
			available = Vec2{available.x - (2.0 * band), available.y - (2.0 * band)};
		layout.scale = fitScale(content, available);
		layout.plan = plan;
		// 図は**凡例のぶんを除いた領域の中央**へ置く（左端に寄せると右が間延びする）。
		layout.viewportCenter = plan.center();
		// 凡例は印刷可能領域の右上——図のために空けた帯の中で、いちばん端へ寄せる。
		layout.legendTopRight = area.max;
		return layout;
	}

	PaperArea insetFrameArea(const PaperArea& printable, const PaperArea& frame)
	{
		const PaperArea inside{Vec2{std::max(printable.min.x, frame.min.x + kTitleBlockInset),
									std::max(printable.min.y, frame.min.y + kTitleBlockInset)},
							   Vec2{std::min(printable.max.x, frame.max.x - kTitleBlockInset),
									std::min(printable.max.y, frame.max.y - kTitleBlockInset)}};
		if (inside.width() <= 0.0 || inside.height() <= 0.0)
			return printable;
		return inside;
	}

	bool frameCoversPaper(const PaperArea& frame, const PaperArea& printable)
	{
		return frame.width() >= kTitleBlockMinCoverage * printable.width() &&
			   frame.height() >= kTitleBlockMinCoverage * printable.height();
	}

	PaperArea reserveTitleStrip(const PaperArea& printable, const PaperArea& strip)
	{
		PaperArea area = printable;
		area.min.y = printable.min.y + strip.height() + (2.0 * kTitleBlockInset);
		if (area.height() <= 0.0)
			return printable;
		return area;
	}

	SectionLayout sectionLayout(const Vec2& content, const PaperArea& area,
								const SectionBands& bands, double heightMargin, bool alignTop)
	{
		SectionLayout layout;
		layout.area = area;
		layout.alignTop = alignTop;

		const auto rows = static_cast<double>(kSectionRows);
		const double left = std::max(bands.left, 0.0);
		const double right = std::max(bands.right, 0.0);
		const double margin = std::max(heightMargin, 0.0);

		// 縮尺 scale でのマスと、図の中心のずれ。上下の帯は高さ範囲の余白（用紙の上では
		// margin ÷ scale）に収め、はみ出すぶんだけを足す（Layout.h の sectionLayout）。
		const auto cellAt = [&](double scale, Vec2& offset)
		{
			const double room = margin / scale;
			const double below = std::max(bands.bottom - room, 0.0);
			const double above = std::max(bands.top - room, 0.0);
			offset = Vec2{(left - right) / 2.0, (below - above) / 2.0};
			return Vec2{(content.x / scale) + left + right, (content.y / scale) + below + above};
		};
		// **上下 2 段が縦に収まる**ことを条件に縮尺を選ぶ（要件）。段の間に間隔が 1 つ入る。
		const auto fits = [&](const Vec2& cell)
		{
			return cell.x <= layout.area.width() &&
				   (rows * cell.y) + ((rows - 1.0) * kViewportGap) <= layout.area.height();
		};

		// 階梯は昇順（図が大きくなる順）なので、最初に収まったものが「収まる中で最も大きい
		// 図」になる。**どれにも収まらなければいちばん小さい図**（fitScale と同じ）。
		layout.scale = kScaleDenominators.back();
		if (content.x > 0.0 && content.y > 0.0)
		{
			for (const double scale : kScaleDenominators)
			{
				Vec2 offset;
				if (fits(cellAt(scale, offset)))
				{
					layout.scale = scale;
					break;
				}
			}
		}
		layout.cell = cellAt(layout.scale, layout.viewportOffset);
		layout.below = std::max(bands.bottom - (margin / layout.scale), 0.0);

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

		// 段組み全体を領域の左右の中央に置く（左に寄せると右が間延びする）。上下は中央か、
		// 図面枠があれば上端（余りを表題欄のある下へ回す。Layout.h の SectionLayout）。
		const double totalWidth = (static_cast<double>(columns) * layout.cell.x) +
								  (static_cast<double>(columns - 1) * kViewportGap);
		const double totalHeight = (static_cast<double>(kSectionRows) * layout.cell.y) +
								   (static_cast<double>(kSectionRows - 1) * kViewportGap);
		const Vec2 center = layout.area.center();
		const double left = center.x - (totalWidth / 2.0);
		const double top = layout.alignTop ? layout.area.max.y : center.y + (totalHeight / 2.0);
		return Vec2{left + (static_cast<double>(column) * (layout.cell.x + kViewportGap)) +
						(layout.cell.x / 2.0),
					top - (static_cast<double>(row) * (layout.cell.y + kViewportGap)) -
						(layout.cell.y / 2.0)};
	}

	Vec2 sectionViewportCenter(const SectionLayout& layout, std::size_t indexInSheet)
	{
		return sectionSlotCenter(layout, indexInSheet) + layout.viewportOffset;
	}

	double sectionGroundY(const SectionLayout& layout, std::size_t indexInSheet, double rangeStart,
						  double groundZ)
	{
		const double cellBottom = sectionSlotCenter(layout, indexInSheet).y - (layout.cell.y / 2.0);
		const double scale = layout.scale > 0.0 ? layout.scale : 1.0;
		return cellBottom + layout.below + ((groundZ - rangeStart) / scale);
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

	double dimensionLineCoord(double base, int side, int tier, double scale)
	{
		const double direction = side < 0 ? -1.0 : 1.0;
		const double steps = static_cast<double>(std::max(tier, 0));
		return base + direction * (kDimensionFirstGap + steps * kDimensionTierPitch) * scale;
	}

	double dimensionBand(int outermostTier)
	{
		if (outermostTier < 0)
			return 0.0;
		return kDimensionFirstGap + (static_cast<double>(outermostTier) * kDimensionTierPitch) +
			   kDimensionTextAllowance;
	}

	double gridShoulderAboveDimensions(double shoulder, double gridTop, double dimensionTop,
									   double scale)
	{
		if (scale <= 0.0)
			return shoulder;
		// いまの符号の下端（上端からラベル枠の高さだけ下）が、狙いより何 mm 低いか（用紙）。
		const double bubbleBottom = gridTop - (kGridBubbleHeight * scale);
		const double shortfall = ((dimensionTop - bubbleBottom) / scale) + kGridBubbleClearance;
		return shortfall > 0.0 ? shoulder + shortfall : shoulder;
	}

	LevelMarkShape levelMarkShape(double textSize, double textWidth)
	{
		LevelMarkShape shape;
		shape.triangleHeight = std::max(textSize, 0.0) * kLevelMarkTriangleRatio;
		// 正三角形の高さ h と辺 a は h = a·√3/2。底辺の半分は a/2 = h/√3。
		shape.triangleHalfWidth = shape.triangleHeight / std::numbers::sqrt3;
		shape.textLeft = (2.0 * shape.triangleHalfWidth) + kLevelMarkTextGap;
		shape.textBottom = kLevelMarkTextGap;
		shape.width = shape.textLeft + std::max(textWidth, 0.0);
		return shape;
	}

	double levelMarkStartX(double left, int dimensionTier, double markWidth, double dimensionScale,
						   double markScale)
	{
		// 左の寸法が占める左端（最も外の寸法線の、さらに外の文字まで）。寸法が無ければ図の左端。
		const double occupied = dimensionTier < 0
									? left
									: dimensionLineCoord(left, -1, dimensionTier, dimensionScale) -
										  (kDimensionTextAllowance * dimensionScale);
		return occupied - ((kLevelMarkClearance + std::max(markWidth, 0.0)) * markScale);
	}

	double levelLineLength(double startX, double right, double scale)
	{
		if (scale <= 0.0)
			return kLevelLineOvershoot;
		return std::max((right - startX) / scale, 0.0) + kLevelLineOvershoot;
	}

	std::optional<double> rotatedRectHeight(double angleDegrees, double boundsWidth,
											double boundsHeight)
	{
		const double radians = angleDegrees * std::numbers::pi / 180.0;
		const double c = std::abs(std::cos(radians));
		const double s = std::abs(std::sin(radians));
		const double conditioning = (c * c) - (s * s);
		if (std::abs(conditioning) < kRotatedRectMinConditioning)
			return std::nullopt;
		const double width = std::max(boundsWidth, 0.0);
		const double height = std::max(boundsHeight, 0.0);
		const double solved = ((height * c) - (width * s)) / conditioning;
		// 実測の丸めで外接矩形より大きく解けることがあるので、外接矩形の短辺で頭を押さえる
		// （自身の高さは幅・高さのどちらも超えない）。
		const double clamped = std::min(solved, std::min(width, height));
		if (!(clamped > 0.0))
			return std::nullopt;
		return clamped;
	}
} // namespace HomeskzIfcImport::core
