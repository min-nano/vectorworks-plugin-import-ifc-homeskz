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
		// X 通り／Y 通りのクラス名。Python 版 ifc/grid.py の CLASS_X / CLASS_Y と一致させる
		// （'01作図-01線-01基準線-01通り芯-X通り' 等）。ROADMAP.md M1「X/Y でクラス分け」。
		constexpr const char* kGridClassX = "01作図-01線-01基準線-01通り芯-X通り";
		constexpr const char* kGridClassY = "01作図-01線-01基準線-01通り芯-Y通り";

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

		// 1 本の IfcGridAxis から AxisCurve(IfcPolyline) の全点を平面座標で集める。
		// Python 版 resolve_lines の
		//   pts = [(float(pt.Coordinates[0]), float(pt.Coordinates[1])) for pt in curve.Points]
		// に対応する。曲線が IfcPolyline でない／点が 2 つ未満／いずれかの点を解決できない
		// ときは false を返し、その軸ごとスキップさせる（Python は点にアクセスして例外に
		// なる＝実質その軸を捨てる。1 軸の欠損で全体を止めない）。label も同時に受け取る。
		bool polylinePoints(const Model& model, const Entity& axis, std::string& label,
							std::vector<Vec2>& pts)
		{
			// IfcGridAxis(AxisTag, AxisCurve, SameSense)。AxisTag は省略され得る。
			const Value& tag = axis.attribute(0);
			label = (tag.type == ValueType::String) ? tag.text : std::string();

			const Entity* curve = model.resolve(axis.attribute(1));
			// Python は curve.is_a('IfcPolyline') を確認する。型名で判定（常に大文字保持）。
			if (curve == nullptr || curve->type != "IFCPOLYLINE")
				return false;
			// IfcPolyline.Points は点参照のリスト（属性 0）。
			const Value& points = curve->attribute(0);
			if (!points.isList() || points.items.size() < 2)
				return false;

			pts.clear();
			pts.reserve(points.items.size());
			for (const Value& ref : points.items)
			{
				Vec2 point;
				if (!cartesianPoint(model, ref, point))
					return false; // 点が解決できない軸はスキップ
				pts.push_back(point);
			}
			return true;
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

		// 各 IfcGridAxis の全点を集め、連続する点対（線分）ごとに 1 本の通り芯を作る
		// （Python 版 resolve_lines の `for i in range(len(pts) - 1)` に対応。ポリラインが
		// 多点でも各区間が 1 本になる）。幾何的に重複する線分（向き反転も同一）は全体で
		// 1 本に畳む（最初に現れた 1 本を残す）。#id 昇順・点順で決定的。
		std::vector<RawLine> collectLines(const Model& model)
		{
			std::vector<RawLine> lines;
			for (const int id : model.byType("IFCGRIDAXIS"))
			{
				const Entity* axis = model.entity(id);
				if (axis == nullptr)
					continue;
				std::string name;
				std::vector<Vec2> pts;
				if (!polylinePoints(model, *axis, name, pts))
					continue; // 非ポリライン・点数不足・点未解決の軸はスキップ

				for (std::size_t i = 0; i + 1 < pts.size(); ++i)
				{
					const RawLine segment{name, pts[i], pts[i + 1]};

					bool duplicate = false;
					for (const RawLine& kept : lines)
					{
						if (sameLine(kept, segment))
						{
							duplicate = true;
							break;
						}
					}
					if (!duplicate)
						lines.push_back(segment);
				}
			}
			return lines;
		}

		// 全端点の bbox 中心を求める（原点へ寄せるオフセット。VW 上で図面が原点付近に
		// 来るようにする。ROADMAP.md M1「原点付近にセンタリング」）。lines は非空前提。
		Vec2 boundingCenter(const std::vector<RawLine>& lines)
		{
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
			return Vec2{(minX + maxX) * 0.5, (minY + maxY) * 0.5};
		}
	} // namespace

	bool resolveGridCenter(const Model& model, Vec2& out)
	{
		const std::vector<RawLine> lines = collectLines(model);
		if (lines.empty())
			return false;
		out = boundingCenter(lines);
		return true;
	}

	std::vector<GridCommand> buildGridCommands(const Model& model)
	{
		// 1. 通り芯の線分を集める（重複除去済み）。
		const std::vector<RawLine> lines = collectLines(model);
		if (lines.empty())
			return {};

		// 2. 全端点の bbox 中心（センタリングオフセット）。床・基礎・部材も同じ中心を使う。
		const Vec2 center = boundingCenter(lines);

		// 3. センタリング＋ X/Y 判定＋クラス付与 → GridCommand。X/Y 判定（軸名・線の向き）は
		//    平行移動で不変なので、センタリング前の生の線分に対して行ってよい。
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
