//
//	core/Region.cpp
//
//	filledUnionOutlines の実装（方式はヘッダの「方式」参照）。
//	【SDK 非依存】標準 C++ だけで完結する純粋な幾何計算。
//

#include "core/Region.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <vector>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 座標の同一視幅（mm 単位系。IFC の座標は mm なので 1µm 未満は同一点とみなす）。
		constexpr double kAxisEps = 1e-6;

		// 部品の軸平行バウンディングボックス（セル中心の内外判定を安く弾くため）。
		struct Bounds
		{
			double minX = 0.0;
			double maxX = 0.0;
			double minY = 0.0;
			double maxY = 0.0;
		};

		Bounds boundsOf(const std::vector<Vec2>& part)
		{
			Bounds b{part[0].x, part[0].x, part[0].y, part[0].y};
			for (const Vec2& p : part)
			{
				b.minX = std::min(b.minX, p.x);
				b.maxX = std::max(b.maxX, p.x);
				b.minY = std::min(b.minY, p.y);
				b.maxY = std::max(b.maxY, p.y);
			}
			return b;
		}

		// 値を昇順に並べて重複（kAxisEps 以内）を潰した格子線座標を返す。引数の器を
		// そのまま詰め直して返す（作業用のコンテナを増やさない）。
		std::vector<double> makeAxis(std::vector<double> values)
		{
			std::sort(values.begin(), values.end());
			const auto duplicate = [](double previous, double current)
			{ return current - previous <= kAxisEps; };
			values.erase(std::unique(values.begin(), values.end(), duplicate), values.end());
			return values;
		}

		// 点がポリゴンの内側か（ray casting。境界上の扱いは不定でよい——格子線は必ず
		// 部品の辺に載るので、判定はセル「中心」で行い境界には当たらない）。
		bool pointInPolygon(const std::vector<Vec2>& poly, double x, double y)
		{
			bool inside = false;
			const std::size_t n = poly.size();
			for (std::size_t i = 0, j = n - 1; i < n; j = i++)
			{
				const Vec2& a = poly[i];
				const Vec2& b = poly[j];
				if ((a.y > y) == (b.y > y))
					continue;
				const double t = (y - a.y) / (b.y - a.y);
				if (x < a.x + t * (b.x - a.x))
					inside = !inside;
			}
			return inside;
		}

		// セル格子。実体フラグと連結成分ラベルを持つ。
		struct Grid
		{
			std::vector<double> xs;	 // 格子線 X（昇順・重複なし）
			std::vector<double> ys;	 // 格子線 Y（昇順・重複なし）
			std::size_t nx = 0;		 // セル列数（xs.size() - 1）
			std::size_t ny = 0;		 // セル行数（ys.size() - 1）
			std::vector<char> solid; // ny * nx。実体セルなら 1

			std::size_t index(std::size_t ix, std::size_t iy) const
			{
				return iy * nx + ix;
			}
		};

		// 部品群からセル格子を作り、中心が部品内にあるセルを実体にする。
		Grid buildGrid(const std::vector<std::vector<Vec2>>& parts)
		{
			std::vector<double> xValues;
			std::vector<double> yValues;
			for (const std::vector<Vec2>& part : parts)
			{
				for (const Vec2& p : part)
				{
					xValues.push_back(p.x);
					yValues.push_back(p.y);
				}
			}

			Grid grid;
			grid.xs = makeAxis(std::move(xValues));
			grid.ys = makeAxis(std::move(yValues));
			if (grid.xs.size() < 2 || grid.ys.size() < 2)
				return grid; // 面積のある領域にならない
			grid.nx = grid.xs.size() - 1;
			grid.ny = grid.ys.size() - 1;
			grid.solid.assign(grid.nx * grid.ny, 0);

			std::vector<Bounds> bounds;
			bounds.reserve(parts.size());
			for (const std::vector<Vec2>& part : parts)
				bounds.push_back(boundsOf(part));

			for (std::size_t iy = 0; iy < grid.ny; ++iy)
			{
				const double cy = (grid.ys[iy] + grid.ys[iy + 1]) * 0.5;
				for (std::size_t ix = 0; ix < grid.nx; ++ix)
				{
					const double cx = (grid.xs[ix] + grid.xs[ix + 1]) * 0.5;
					for (std::size_t k = 0; k < parts.size(); ++k)
					{
						const Bounds& b = bounds[k];
						if (cx < b.minX || cx > b.maxX || cy < b.minY || cy > b.maxY)
							continue;
						if (pointInPolygon(parts[k], cx, cy))
						{
							grid.solid[grid.index(ix, iy)] = 1;
							break;
						}
					}
				}
			}
			return grid;
		}

		// 外周から実体でないセルを塗り広げ、外へ抜けられなかった空隙（＝骨組みに囲まれた
		// 領域）を求める。戻り値は「囲まれた空隙なら 1」のマスク。
		std::vector<char> enclosedCells(const Grid& grid)
		{
			std::vector<char> outside(grid.solid.size(), 0);
			std::vector<std::size_t> stack;
			// 種は格子の外周セル（実体でないもの）。
			for (std::size_t iy = 0; iy < grid.ny; ++iy)
			{
				for (std::size_t ix = 0; ix < grid.nx; ++ix)
				{
					const bool border =
						(ix == 0 || iy == 0 || ix + 1 == grid.nx || iy + 1 == grid.ny);
					const std::size_t at = grid.index(ix, iy);
					if (border && grid.solid[at] == 0 && outside[at] == 0)
					{
						outside[at] = 1;
						stack.push_back(at);
					}
				}
			}
			while (!stack.empty())
			{
				const std::size_t at = stack.back();
				stack.pop_back();
				const std::size_t ix = at % grid.nx;
				const std::size_t iy = at / grid.nx;
				const std::size_t neighbours[4] = {
					ix > 0 ? at - 1 : at, ix + 1 < grid.nx ? at + 1 : at,
					iy > 0 ? at - grid.nx : at, iy + 1 < grid.ny ? at + grid.nx : at};
				for (const std::size_t next : neighbours)
				{
					if (next == at || grid.solid[next] != 0 || outside[next] != 0)
						continue;
					outside[next] = 1;
					stack.push_back(next);
				}
			}

			std::vector<char> enclosed(grid.solid.size(), 0);
			for (std::size_t at = 0; at < grid.solid.size(); ++at)
				enclosed[at] = static_cast<char>(grid.solid[at] == 0 && outside[at] == 0);
			return enclosed;
		}

		// 領域を「囲まれた空隙」＋「それに接する実体セル（＝空隙を囲っている部材）」に
		// 絞り込む。どの空隙にも接しない実体セル——骨組みから外へ突き出しただけの部材や
		// 孤立した部材——は落ちるので、外形に細いヒゲが生えない。
		void keepEnclosingOnly(Grid& grid)
		{
			const std::vector<char> enclosed = enclosedCells(grid);
			std::vector<char> region = enclosed;
			for (std::size_t iy = 0; iy < grid.ny; ++iy)
			{
				for (std::size_t ix = 0; ix < grid.nx; ++ix)
				{
					const std::size_t at = grid.index(ix, iy);
					if (grid.solid[at] == 0)
						continue;
					// 斜めも含む 8 近傍で空隙に接していれば、その空隙を囲う部材とみなす
					// （4 近傍だけだと角のセルが落ちて外形がギザギザになる）。
					const std::size_t x0 = ix > 0 ? ix - 1 : 0;
					const std::size_t x1 = std::min(ix + 1, grid.nx - 1);
					const std::size_t y0 = iy > 0 ? iy - 1 : 0;
					const std::size_t y1 = std::min(iy + 1, grid.ny - 1);
					for (std::size_t ny = y0; ny <= y1 && region[at] == 0; ++ny)
					{
						for (std::size_t nx = x0; nx <= x1; ++nx)
						{
							if (enclosed[grid.index(nx, ny)] != 0)
							{
								region[at] = 1;
								break;
							}
						}
					}
				}
			}
			grid.solid = std::move(region);
		}

		// 実体セルを 4 近傍で連結成分に分ける。戻り値は成分数、labels は各セルの
		// 成分番号（実体でないセルは 0、実体は 1 以上）。
		std::size_t labelComponents(const Grid& grid, std::vector<std::size_t>& labels)
		{
			labels.assign(grid.solid.size(), 0);
			std::size_t count = 0;
			std::vector<std::size_t> stack;
			for (std::size_t seed = 0; seed < grid.solid.size(); ++seed)
			{
				if (grid.solid[seed] == 0 || labels[seed] != 0)
					continue;
				++count;
				labels[seed] = count;
				stack.push_back(seed);
				while (!stack.empty())
				{
					const std::size_t at = stack.back();
					stack.pop_back();
					const std::size_t ix = at % grid.nx;
					const std::size_t iy = at / grid.nx;
					const std::size_t neighbours[4] = {
						ix > 0 ? at - 1 : at, ix + 1 < grid.nx ? at + 1 : at,
						iy > 0 ? at - grid.nx : at, iy + 1 < grid.ny ? at + grid.nx : at};
					for (const std::size_t next : neighbours)
					{
						if (next == at || grid.solid[next] == 0 || labels[next] != 0)
							continue;
						labels[next] = count;
						stack.push_back(next);
					}
				}
			}
			return count;
		}

		// 格子点（格子線の交点）の通し番号。辺の連結は浮動小数の比較ではなく
		// この整数 ID で行う（丸め誤差で経路が切れないようにするため）。
		std::size_t cornerId(const Grid& grid, std::size_t ix, std::size_t iy)
		{
			return iy * (grid.xs.size()) + ix;
		}

		// 成分の境界辺（実体側を左に見る向き）を、格子点 ID の有向辺として edges へ集める。
		void boundaryEdges(const Grid& grid, const std::vector<std::size_t>& labels,
						   std::size_t label, std::multimap<std::size_t, std::size_t>& edges)
		{
			edges.clear();
			for (std::size_t iy = 0; iy < grid.ny; ++iy)
			{
				for (std::size_t ix = 0; ix < grid.nx; ++ix)
				{
					if (labels[grid.index(ix, iy)] != label)
						continue;
					// セルを反時計回りに一周し、隣が同じ成分でない辺だけを残す。
					const bool down = (iy == 0) || labels[grid.index(ix, iy - 1)] != label;
					const bool right =
						(ix + 1 == grid.nx) || labels[grid.index(ix + 1, iy)] != label;
					const bool up = (iy + 1 == grid.ny) || labels[grid.index(ix, iy + 1)] != label;
					const bool left = (ix == 0) || labels[grid.index(ix - 1, iy)] != label;
					if (down)
						edges.emplace(cornerId(grid, ix, iy), cornerId(grid, ix + 1, iy));
					if (right)
						edges.emplace(cornerId(grid, ix + 1, iy), cornerId(grid, ix + 1, iy + 1));
					if (up)
						edges.emplace(cornerId(grid, ix + 1, iy + 1), cornerId(grid, ix, iy + 1));
					if (left)
						edges.emplace(cornerId(grid, ix, iy + 1), cornerId(grid, ix, iy));
				}
			}
		}

		// 格子点 ID を座標へ戻す。
		Vec2 cornerPoint(const Grid& grid, std::size_t id)
		{
			const std::size_t ix = id % grid.xs.size();
			const std::size_t iy = id / grid.xs.size();
			return Vec2{grid.xs[ix], grid.ys[iy]};
		}

		// 多角形の符号付き面積の 2 倍（反時計回りで正）。
		double signedArea2(const std::vector<Vec2>& ring)
		{
			double sum = 0.0;
			const std::size_t n = ring.size();
			for (std::size_t i = 0, j = n - 1; i < n; j = i++)
				sum += (ring[j].x * ring[i].y) - (ring[i].x * ring[j].y);
			return sum;
		}

		// 共線の中間点を落とす（軸並行の外形は角だけで表せる）。ring を詰め直す。
		void dropCollinear(std::vector<Vec2>& ring)
		{
			const std::vector<Vec2> source = ring;
			ring.clear();
			const std::size_t n = source.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& prev = source[(i + n - 1) % n];
				const Vec2& here = source[i];
				const Vec2& next = source[(i + 1) % n];
				const double cross =
					(here.x - prev.x) * (next.y - here.y) - (here.y - prev.y) * (next.x - here.x);
				if (std::abs(cross) > kAxisEps)
					ring.push_back(here);
			}
		}

		// 有向辺を繋いで閉ループを取り出し、面積が最大のもの（＝外形）を outline へ入れる。
		// 穴のループは面積が小さい（かつ向きが逆）ので落ちる。edges は消費する。
		void traceOutline(const Grid& grid, std::multimap<std::size_t, std::size_t>& edges,
						  std::vector<Vec2>& outline)
		{
			outline.clear();
			std::vector<Vec2> ring;
			double bestArea = 0.0;
			while (!edges.empty())
			{
				const std::size_t start = edges.begin()->first;
				ring.clear();
				std::size_t at = start;
				// 各辺は 1 度だけ使う。境界辺は格子点ごとに入次数＝出次数なので、辿り
				// 始めれば必ず始点へ戻る（行き止まりは起きない＝ find は失敗しない）。
				for (auto edge = edges.find(at); edge != edges.end(); edge = edges.find(at))
				{
					const std::size_t next = edge->second;
					edges.erase(edge);
					ring.push_back(cornerPoint(grid, at));
					at = next;
					if (at == start)
						break;
				}
				const double area = signedArea2(ring);
				if (area > bestArea)
				{
					bestArea = area;
					outline = ring;
				}
			}
			dropCollinear(outline);
		}
	} // namespace

	std::vector<std::vector<Vec2>> filledUnionOutlines(const std::vector<std::vector<Vec2>>& parts)
	{
		std::vector<std::vector<Vec2>> outlines;

		// 3 点未満の部品は面積を持たないので落とす（内外判定も定義できない）。
		std::vector<std::vector<Vec2>> usable;
		for (const std::vector<Vec2>& part : parts)
		{
			if (part.size() >= 3)
				usable.push_back(part);
		}
		if (usable.empty())
			return outlines;

		Grid grid = buildGrid(usable);
		if (grid.solid.empty())
			return outlines;
		keepEnclosingOnly(grid);

		std::vector<std::size_t> labels;
		std::multimap<std::size_t, std::size_t> edges;
		std::vector<Vec2> outline;
		const std::size_t components = labelComponents(grid, labels);
		for (std::size_t label = 1; label <= components; ++label)
		{
			boundaryEdges(grid, labels, label, edges);
			traceOutline(grid, edges, outline);
			if (outline.size() >= 3)
				outlines.push_back(std::move(outline));
		}
		return outlines;
	}
} // namespace HomeskzIfcImport::core
