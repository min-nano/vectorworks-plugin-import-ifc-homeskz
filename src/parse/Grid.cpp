//
//	parse/Grid.cpp
//
//	通り芯（グリッド）解析の実装。Python 版 ifc/grid.py の build_grid_commands に対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Grid.h"

#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::GridCommand;
	using core::Vec2;

	namespace
	{
		// X 通り／Y 通りのクラス名。ROADMAP.md M1「X/Y でクラス分け」に対応する。
		//
		// 注意（要ローカル確認）: Python 版 ifc/grid.py が使う正確なクラス文字列は本
		// リポジトリからは参照できなかった（姉妹リポジトリが未添付）。ここでは意味の
		// 明確な既定値を 1 か所の定数として置く。Python 版の実クラス名が判明したら
		// この 2 定数を差し替えるだけでよい（描画・テストは名前に依存しない構造にしてある）。
		constexpr const char* kGridClassX = "通り芯-X";
		constexpr const char* kGridClassY = "通り芯-Y";

		// 座標比較の許容誤差（mm）。重複線除去と縮退判定に使う。ホームズ君 IFC の
		// 重複軸は通常ぴったり一致するので、丸め耐性の微小値で十分。
		constexpr double kEps = 1e-6;

		// 解析途中の 1 本の通り芯（センタリング前の生端点＋軸名）。
		struct RawLine
		{
			std::string label;
			Vec2 start;
			Vec2 end;
		};

		bool samePoint(const Vec2& a, const Vec2& b)
		{
			return std::abs(a.x - b.x) < kEps && std::abs(a.y - b.y) < kEps;
		}

		// 2 本が幾何的に同一の線分か（向きの反転も同一とみなす）。重複線除去に使う。
		bool sameLine(const RawLine& a, const RawLine& b)
		{
			return (samePoint(a.start, b.start) && samePoint(a.end, b.end)) ||
				   (samePoint(a.start, b.end) && samePoint(a.end, b.start));
		}

		// 参照値が指す IfcCartesianPoint の平面座標（X,Y）を取り出す。3D 点でも Z は
		// 捨てる（通り芯は平面で決まる）。解決できない・座標が足りないときは false。
		bool cartesianPoint(const Model& model, const Value& ref, Vec2& out)
		{
			const Entity* point = model.resolve(ref);
			if (point == nullptr)
				return false;
			// IfcCartesianPoint.Coordinates は実数のリスト（属性 0）。
			const Value& coords = point->attribute(0);
			if (!coords.isList() || coords.items.size() < 2)
				return false;
			out.x = coords.items[0].asReal();
			out.y = coords.items[1].asReal();
			return true;
		}

		// 1 本の IfcGridAxis を RawLine に変換する。AxisCurve(IfcPolyline) の最初と
		// 最後の点を始点・終点にする。曲線を解決できない／点が 2 つ未満なら false を
		// 返してスキップさせる（1 軸の欠損で全体を止めない）。
		bool rawLineFromAxis(const Model& model, const Entity& axis, RawLine& out)
		{
			// IfcGridAxis(AxisTag, AxisCurve, SameSense)。AxisTag は省略され得る。
			const Value& tag = axis.attribute(0);
			out.label = (tag.type == ValueType::String) ? tag.text : std::string();

			const Entity* curve = model.resolve(axis.attribute(1));
			if (curve == nullptr)
				return false;
			// IfcPolyline.Points は点参照のリスト（属性 0）。始点＝先頭、終点＝末尾。
			const Value& points = curve->attribute(0);
			if (!points.isList() || points.items.size() < 2)
				return false;

			return cartesianPoint(model, points.items.front(), out.start) &&
				   cartesianPoint(model, points.items.back(), out.end);
		}

		// 軸が X 通りか Y 通りかを判定する（ROADMAP.md M1）。まず軸名の先頭文字が
		// X/Y ならそれに従い（大文字小文字を無視）、判別できなければ線の向きで決める
		// （|Δx|<|Δy| すなわち縦長の線を X 通り＝X 軸上に並ぶ縦線とみなす。ホームズ君の
		// x1/x2… は実際に鉛直線として出力される）。
		bool isXAxis(const RawLine& line)
		{
			if (!line.label.empty())
			{
				const auto first = static_cast<unsigned char>(line.label.front());
				const int upper = std::toupper(first);
				if (upper == 'X')
					return true;
				if (upper == 'Y')
					return false;
			}
			const double dx = std::abs(line.end.x - line.start.x);
			const double dy = std::abs(line.end.y - line.start.y);
			return dx < dy;
		}
	} // namespace

	std::vector<GridCommand> buildGridCommands(const Model& model)
	{
		// 1. IfcGridAxis を #id 昇順（byType は昇順を保証）で RawLine 化しつつ、
		//    幾何的に重複する線を除去する（最初に現れた 1 本を残す＝決定的）。
		std::vector<RawLine> lines;
		for (const int id : model.byType("IFCGRIDAXIS"))
		{
			const Entity* axis = model.entity(id);
			if (axis == nullptr)
				continue;
			RawLine line;
			if (!rawLineFromAxis(model, *axis, line))
				continue; // 曲線未解決・点数不足の軸はスキップ

			bool duplicate = false;
			for (const RawLine& kept : lines)
			{
				if (sameLine(kept, line))
				{
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				lines.push_back(line);
		}

		if (lines.empty())
			return {};

		// 2. 全端点の bbox 中心を求め、原点へ寄せるオフセットを作る（VW 上で通り芯が
		//    原点付近に来るようにする。ROADMAP.md M1「原点付近にセンタリング」）。
		double minX = std::numeric_limits<double>::max();
		double minY = std::numeric_limits<double>::max();
		double maxX = std::numeric_limits<double>::lowest();
		double maxY = std::numeric_limits<double>::lowest();
		for (const RawLine& line : lines)
		{
			for (const Vec2& p : {line.start, line.end})
			{
				minX = std::min(minX, p.x);
				minY = std::min(minY, p.y);
				maxX = std::max(maxX, p.x);
				maxY = std::max(maxY, p.y);
			}
		}
		const Vec2 center{(minX + maxX) * 0.5, (minY + maxY) * 0.5};

		// 3. センタリング＋ X/Y 判定＋クラス付与 → GridCommand。判定は平行移動で
		//    不変なのでセンタリング前後どちらでもよいが、出力座標に揃えて後で行う。
		std::vector<GridCommand> commands;
		commands.reserve(lines.size());
		for (const RawLine& line : lines)
		{
			GridCommand cmd;
			cmd.label = line.label;
			// layer は既定の "共通" のまま（通り芯は常に共通レイヤ）。
			cmd.start = Vec2{line.start.x - center.x, line.start.y - center.y};
			cmd.end = Vec2{line.end.x - center.x, line.end.y - center.y};
			cmd.drawClass = isXAxis(line) ? kGridClassX : kGridClassY;
			commands.push_back(cmd);
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
