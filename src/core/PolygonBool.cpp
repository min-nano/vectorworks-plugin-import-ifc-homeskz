//
//	core/PolygonBool.cpp
//
//	平面多角形の集合演算の実装。意図・手順は core/PolygonBool.h を参照。
//

#include "core/PolygonBool.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <utility>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 交点計算で頂点を丸める粒度（1e-4mm = 0.1 ミクロン）。丸めた点をキーにするので
		// 集合・辞書で厳密比較できる。
		constexpr double kRoundScale = 1e4;

		// 丸めた平面点。std::set / std::map の鍵にするので、Vec2 ではなく比較可能な pair。
		using Pt2 = std::pair<double, double>;
		using Edge = std::pair<Pt2, Pt2>;
		using Ring = std::vector<Pt2>;

		Pt2 roundPt(double x, double y)
		{
			return Pt2{std::round(x * kRoundScale) / kRoundScale,
					   std::round(y * kRoundScale) / kRoundScale};
		}

		// 外形を丸めた頂点列にし、末尾の閉じ重複・連続する同一点を除く。
		Ring cleanRing(const std::vector<Vec2>& boundary)
		{
			Ring out;
			out.reserve(boundary.size());
			for (const Vec2& p : boundary)
			{
				const Pt2 rp = roundPt(p.x, p.y);
				if (out.empty() || out.back() != rp)
					out.push_back(rp);
			}
			if (out.size() > 1 && out.front() == out.back())
				out.pop_back();
			return out;
		}

		std::vector<Vec2> toVec(const Ring& ring)
		{
			std::vector<Vec2> out;
			out.reserve(ring.size());
			for (const Pt2& p : ring)
				out.push_back(Vec2{p.first, p.second});
			return out;
		}

		// 点 (x, y) が単純多角形の内部（境界は含めない近似）にあるか。水平レイキャスト
		// （半開ルール）。呼び出し側は辺から法線方向へ kPolySideEps ずらした点を渡すので、
		// 辺ちょうどの縮退は問題にならない。
		bool pointInRing(double x, double y, const Ring& poly)
		{
			bool inside = false;
			const std::size_t n = poly.size();
			if (n < 3)
				return false;
			std::size_t j = n - 1;
			for (std::size_t i = 0; i < n; ++i)
			{
				const auto [xi, yi] = poly[i];
				const auto [xj, yj] = poly[j];
				if ((yi > y) != (yj > y))
				{
					const double xint = xi + ((y - yi) * (xj - xi) / (yj - yi));
					if (x < xint)
						inside = !inside;
				}
				j = i;
			}
			return inside;
		}

		bool inAnyRing(double x, double y, const std::vector<Ring>& rings)
		{
			return std::ranges::any_of(rings, [x, y](const Ring& ring)
									   { return pointInRing(x, y, ring); });
		}

		// 線分 ab を分割すべき点（線分 cd との交点）を ab 上の点として返す。非平行なら区間内
		// の交点、共線なら cd の端点を ab 上へ射影した点（区間内）。これで交差・T 字接合・
		// 共線オーバーラップの分割点をすべて拾う。
		std::vector<Pt2> segSplitPoints(const Pt2& a, const Pt2& b, const Pt2& c, const Pt2& d)
		{
			const double rx = b.first - a.first;
			const double ry = b.second - a.second;
			const double sx = d.first - c.first;
			const double sy = d.second - c.second;
			const double rLen = std::hypot(rx, ry);
			const double sLen = std::hypot(sx, sy);
			if (rLen <= 0.0 || sLen <= 0.0)
				return {};

			std::vector<Pt2> out;
			const double denom = (rx * sy) - (ry * sx);
			if (std::abs(denom) > kPolyAngleTol * rLen * sLen)
			{
				const double t =
					(((c.first - a.first) * sy) - ((c.second - a.second) * sx)) / denom;
				const double u =
					(((c.first - a.first) * ry) - ((c.second - a.second) * rx)) / denom;
				if (t >= -1e-9 && t <= 1.0 + 1e-9 && u >= -1e-9 && u <= 1.0 + 1e-9)
					out.emplace_back(a.first + (t * rx), a.second + (t * ry));
				return out;
			}
			// 平行: 共線ならオーバーラップ端点を分割点にする。
			if (std::abs(((c.first - a.first) * ry) - ((c.second - a.second) * rx)) >
				kPolyDistTol * rLen)
				return out;
			for (const Pt2& p : {c, d})
			{
				const double t =
					(((p.first - a.first) * rx) + ((p.second - a.second) * ry)) / (rLen * rLen);
				if (t >= -1e-9 && t <= 1.0 + 1e-9)
					out.emplace_back(a.first + (t * rx), a.second + (t * ry));
			}
			return out;
		}

		// 有向辺 a→b を分割点 cuts で細分した有向部分辺のリスト。
		std::vector<Edge> splitEdge(const Pt2& a, const Pt2& b, const std::set<Pt2>& cuts)
		{
			const double rx = b.first - a.first;
			const double ry = b.second - a.second;
			const double length2 = (rx * rx) + (ry * ry);

			std::map<Pt2, double> params;
			const auto add = [&](const Pt2& raw)
			{
				const Pt2 rp = roundPt(raw.first, raw.second);
				const double t =
					(length2 > 0.0)
						? ((((rp.first - a.first) * rx) + ((rp.second - a.second) * ry)) / length2)
						: 0.0;
				if (t >= -1e-9 && t <= 1.0 + 1e-9)
					params[rp] = t;
			};
			add(a);
			add(b);
			for (const Pt2& cut : cuts)
				add(cut);

			std::vector<Pt2> ordered;
			ordered.reserve(params.size());
			for (const auto& entry : params)
				ordered.push_back(entry.first);
			std::stable_sort(ordered.begin(), ordered.end(),
							 [&params](const Pt2& lhs, const Pt2& rhs)
							 { return params.at(lhs) < params.at(rhs); });

			std::vector<Edge> out;
			for (std::size_t i = 0; i + 1 < ordered.size(); ++i)
			{
				if (ordered[i] != ordered[i + 1])
					out.emplace_back(ordered[i], ordered[i + 1]);
			}
			return out;
		}

		// 境界追跡で分岐点に来たとき、内側を左に保つ次の辺（最も時計回り）を選ぶ。
		Edge nextBoundaryEdge(const Edge& current, const std::vector<Edge>& options)
		{
			const double reverse = std::atan2(current.first.second - current.second.second,
											  current.first.first - current.second.first);
			const auto clockwise = [reverse](const Edge& edge)
			{
				const double d = std::atan2(edge.second.second - edge.first.second,
											edge.second.first - edge.first.first);
				double angle = std::fmod(reverse - d, 2.0 * std::numbers::pi);
				if (angle < 0.0)
					angle += 2.0 * std::numbers::pi;
				return (angle > 1e-9) ? angle : 2.0 * std::numbers::pi;
			};
			return *std::min_element(options.begin(), options.end(),
									 [&clockwise](const Edge& lhs, const Edge& rhs)
									 { return clockwise(lhs) < clockwise(rhs); });
		}

		// 閉リングから共線の中間点を除いた頂点列。
		Ring simplifyRing(const Ring& ring)
		{
			Ring pts = ring;
			if (pts.size() > 1 && pts.front() == pts.back())
				pts.pop_back();
			const std::size_t n = pts.size();
			Ring out;
			for (std::size_t i = 0; i < n; ++i)
			{
				const Pt2& a = pts[(i + n - 1) % n];
				const Pt2& b = pts[i];
				const Pt2& c = pts[(i + 1) % n];
				const double cross = ((b.first - a.first) * (c.second - b.second)) -
									 ((b.second - a.second) * (c.first - b.first));
				if (std::abs(cross) > kPolyDistTol)
					out.push_back(b);
			}
			return out;
		}

		// 有向境界辺をつないで閉ループのリストにする。開ループが生じたら false。
		bool chainBoundary(const std::vector<Edge>& edges, std::vector<Ring>& out)
		{
			std::map<Pt2, std::vector<Edge>> fromMap;
			for (const Edge& edge : edges)
				fromMap[edge.first].push_back(edge);
			std::set<Edge> remaining(edges.begin(), edges.end());

			std::vector<Ring> loops;
			while (!remaining.empty())
			{
				const Edge start = *remaining.begin(); // std::set は辞書順なので決定的
				Edge cur = start;
				Ring ring{cur.first};
				while (true)
				{
					remaining.erase(cur);
					ring.push_back(cur.second);
					if (cur.second == start.first)
						break;
					std::vector<Edge> options;
					const auto found = fromMap.find(cur.second);
					if (found != fromMap.end())
					{
						for (const Edge& edge : found->second)
						{
							if (remaining.contains(edge))
								options.push_back(edge);
						}
					}
					if (options.empty())
						return false;
					cur = nextBoundaryEdge(cur, options);
				}
				Ring simplified = simplifyRing(ring);
				if (simplified.size() >= 3)
					loops.push_back(std::move(simplified));
			}
			out = std::move(loops);
			return true;
		}

		// 集合演算の本体。すべての辺を交点で細分し、**左が領域の内・右が領域の外**の向きの
		// 部分辺だけを境界として残してつなぐ。領域は「subject のどれかの内側で、clip の
		// どれの内側でもないところ」——clip が空なら和、空でなければ差になる（ヘッダ冒頭）。
		bool booleanBoundary(const std::vector<Ring>& subject, const std::vector<Ring>& clip,
							 std::vector<Ring>& out)
		{
			const auto inRegion = [&subject, &clip](double x, double y)
			{ return inAnyRing(x, y, subject) && !inAnyRing(x, y, clip); };

			std::vector<Edge> directed;
			for (const std::vector<Ring>* rings : {&subject, &clip})
			{
				for (const Ring& ring : *rings)
				{
					const std::size_t n = ring.size();
					for (std::size_t i = 0; i < n; ++i)
						directed.emplace_back(ring[i], ring[(i + 1) % n]);
				}
			}
			if (directed.empty())
			{
				out.clear();
				return true;
			}

			const std::size_t m = directed.size();
			std::vector<std::set<Pt2>> cuts(m);
			for (std::size_t i = 0; i < m; ++i)
			{
				for (std::size_t j = 0; j < m; ++j)
				{
					if (i == j)
						continue;
					for (const Pt2& pt : segSplitPoints(directed[i].first, directed[i].second,
														directed[j].first, directed[j].second))
						cuts[i].insert(roundPt(pt.first, pt.second));
				}
			}

			std::vector<Edge> boundary;
			std::set<Edge> seen; // 向きを問わず 1 度だけ判定する（共有辺の重複を落とす）
			for (std::size_t i = 0; i < m; ++i)
			{
				for (const Edge& part : splitEdge(directed[i].first, directed[i].second, cuts[i]))
				{
					const Edge key =
						(part.first < part.second) ? part : Edge{part.second, part.first};
					if (!seen.insert(key).second)
						continue;
					const double mx = (part.first.first + part.second.first) / 2.0;
					const double my = (part.first.second + part.second.second) / 2.0;
					const double ex = part.second.first - part.first.first;
					const double ey = part.second.second - part.first.second;
					const double len = std::hypot(ex, ey);
					if (len <= 0.0)
						continue; // 細分で長さ 0 の部分辺は出ないが、0 除算はしない
					// 進行方向 p→q の右向き法線 (ey, −ex)/len。両側を突いて領域の内外を見る。
					const double nx = kPolySideEps * ey / len;
					const double ny = -kPolySideEps * ex / len;
					const bool rightInside = inRegion(mx + nx, my + ny);
					const bool leftInside = inRegion(mx - nx, my - ny);
					if (leftInside == rightInside)
						continue; // 領域の内部（または外部）を横切る辺は境界ではない
					// 内側を左に保つ向きで残す。
					if (leftInside)
						boundary.push_back(part);
					else
						boundary.emplace_back(part.second, part.first);
				}
			}
			return chainBoundary(boundary, out);
		}

		// 線分 a→b 上の点 p の正規化パラメータ（0=a, 1=b）。
		double paramOn(const Pt2& a, const Pt2& b, const Pt2& p)
		{
			const double rx = b.first - a.first;
			const double ry = b.second - a.second;
			const double length2 = (rx * rx) + (ry * ry);
			if (length2 <= 0.0)
				return 0.0;
			return (((p.first - a.first) * rx) + ((p.second - a.second) * ry)) / length2;
		}

		// 共線の線分 ab・cd の重なり長さ（共線でなければ 0）。
		double collinearOverlap(const Pt2& a, const Pt2& b, const Pt2& c, const Pt2& d)
		{
			const double rx = b.first - a.first;
			const double ry = b.second - a.second;
			const double rLen = std::hypot(rx, ry);
			if (rLen <= 0.0)
				return 0.0;
			const double sx = d.first - c.first;
			const double sy = d.second - c.second;
			const double sLen = std::hypot(sx, sy);
			if (sLen <= 0.0)
				return 0.0;
			if (std::abs((rx * sy) - (ry * sx)) > kPolyAngleTol * rLen * sLen)
				return 0.0;
			if (std::abs(((c.first - a.first) * ry) - ((c.second - a.second) * rx)) >
				kPolyDistTol * rLen)
				return 0.0;
			const double tc =
				(((c.first - a.first) * rx) + ((c.second - a.second) * ry)) / (rLen * rLen);
			const double td =
				(((d.first - a.first) * rx) + ((d.second - a.second) * ry)) / (rLen * rLen);
			const double lo = std::max(0.0, std::min(tc, td));
			const double hi = std::min(1.0, std::max(tc, td));
			return (hi > lo) ? (hi - lo) * rLen : 0.0;
		}

		// 点が多角形の**境界の上**にあるか（辺までの距離が許容以内）。角どうしが触れているだけの
		// 並びを「繋がっている」と読まないために使う——レイキャストの半開ルールでは、相手の
		// 頂点にちょうど乗った点が内側に落ちることがある。
		bool pointOnRing(const Pt2& p, const Ring& ring)
		{
			const std::size_t n = ring.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Pt2& a = ring[i];
				const Pt2& b = ring[(i + 1) % n];
				const double rx = b.first - a.first;
				const double ry = b.second - a.second;
				const double len2 = (rx * rx) + (ry * ry);
				double t = 0.0;
				if (len2 > 0.0)
					t = std::clamp((((p.first - a.first) * rx) + ((p.second - a.second) * ry)) /
									   len2,
								   0.0, 1.0);
				if (std::hypot(p.first - (a.first + (t * rx)), p.second - (a.second + (t * ry))) <=
					kPolyDistTol)
					return true;
			}
			return false;
		}

		// 点が多角形の**内部**にある（境界の上は含めない）か。
		bool pointStrictlyInRing(const Pt2& p, const Ring& ring)
		{
			return pointInRing(p.first, p.second, ring) && !pointOnRing(p, ring);
		}

		bool ringsConnected(const Ring& a, const Ring& b)
		{
			const std::size_t na = a.size();
			const std::size_t nb = b.size();
			for (std::size_t i = 0; i < na; ++i)
			{
				const Pt2& a1 = a[i];
				const Pt2& a2 = a[(i + 1) % na];
				for (std::size_t j = 0; j < nb; ++j)
				{
					const Pt2& b1 = b[j];
					const Pt2& b2 = b[(j + 1) % nb];
					if (collinearOverlap(a1, a2, b1, b2) > kPolyDistTol)
						return true;
					// 内部で交差（端点を含まない真の交差）
					for (const Pt2& pt : segSplitPoints(a1, a2, b1, b2))
					{
						const double t = paramOn(a1, a2, pt);
						const double u = paramOn(b1, b2, pt);
						if (t > kPolyAngleTol && t < 1.0 - kPolyAngleTol && u > kPolyAngleTol &&
							u < 1.0 - kPolyAngleTol)
							return true;
					}
				}
			}
			if (std::ranges::any_of(a, [&b](const Pt2& p) { return pointStrictlyInRing(p, b); }))
				return true;
			return std::ranges::any_of(b, [&a](const Pt2& p) { return pointStrictlyInRing(p, a); });
		}
	} // namespace

	bool polygonUnion(const PolygonList& polys, PolygonList& out)
	{
		std::vector<Ring> rings;
		rings.reserve(polys.size());
		for (const std::vector<Vec2>& poly : polys)
		{
			Ring ring = cleanRing(poly);
			if (ring.size() >= 3)
				rings.push_back(std::move(ring));
		}

		std::vector<Ring> loops;
		if (!booleanBoundary(rings, {}, loops)) // 引く相手が空＝和
			return false;

		PolygonList result;
		result.reserve(loops.size());
		for (const Ring& loop : loops)
			result.push_back(toVec(loop));
		out = std::move(result);
		return true;
	}

	bool polygonDifference(const PolygonList& subject, const PolygonList& clip, PolygonList& out)
	{
		std::vector<Ring> subjectRings;
		for (const std::vector<Vec2>& poly : subject)
		{
			Ring ring = cleanRing(poly);
			if (ring.size() >= 3)
				subjectRings.push_back(std::move(ring));
		}
		std::vector<Ring> clipRings;
		for (const std::vector<Vec2>& poly : clip)
		{
			Ring ring = cleanRing(poly);
			if (ring.size() >= 3)
				clipRings.push_back(std::move(ring));
		}

		std::vector<Ring> loops;
		if (!booleanBoundary(subjectRings, clipRings, loops))
			return false;

		PolygonList result;
		result.reserve(loops.size());
		for (const Ring& loop : loops)
			result.push_back(toVec(loop));
		out = std::move(result);
		return true;
	}

	bool polygonsConnected(const std::vector<Vec2>& a, const std::vector<Vec2>& b)
	{
		const Ring ra = cleanRing(a);
		const Ring rb = cleanRing(b);
		if (ra.size() < 3 || rb.size() < 3)
			return false;
		return ringsConnected(ra, rb);
	}

	std::vector<std::vector<std::size_t>> polygonComponents(const PolygonList& polys)
	{
		const std::size_t n = polys.size();
		std::vector<std::size_t> parent(n);
		for (std::size_t i = 0; i < n; ++i)
			parent[i] = i;
		const auto find = [&parent](std::size_t a)
		{
			while (parent[a] != a)
			{
				parent[a] = parent[parent[a]];
				a = parent[a];
			}
			return a;
		};

		std::vector<Ring> rings;
		rings.reserve(n);
		for (const std::vector<Vec2>& poly : polys)
			rings.push_back(cleanRing(poly));

		for (std::size_t p = 0; p < n; ++p)
		{
			for (std::size_t q = p + 1; q < n; ++q)
			{
				if (rings[p].size() < 3 || rings[q].size() < 3)
					continue;
				if (!ringsConnected(rings[p], rings[q]))
					continue;
				const std::size_t ra = find(p);
				const std::size_t rb = find(q);
				if (ra != rb)
					parent[std::max(ra, rb)] = std::min(ra, rb);
			}
		}

		std::map<std::size_t, std::vector<std::size_t>> comps;
		for (std::size_t i = 0; i < n; ++i)
			comps[find(i)].push_back(i);

		std::vector<std::vector<std::size_t>> result;
		result.reserve(comps.size());
		for (auto& comp : comps)
			result.push_back(std::move(comp.second));
		return result;
	}

	PolygonList mergePolygons(const PolygonList& polys)
	{
		PolygonList result;
		for (const std::vector<std::size_t>& component : polygonComponents(polys))
		{
			const auto keepAsIs = [&]
			{
				for (const std::size_t index : component)
					result.push_back(polys[index]);
			};
			if (component.size() < 2)
			{
				keepAsIs();
				continue;
			}
			PolygonList members;
			members.reserve(component.size());
			for (const std::size_t index : component)
				members.push_back(polys[index]);
			PolygonList merged;
			// 和が単一の穴なしループにならない成分は畳まない（ヘッダ mergePolygons）。
			if (!polygonUnion(members, merged) || merged.size() != 1 || merged.front().size() < 3)
			{
				keepAsIs();
				continue;
			}
			result.push_back(merged.front());
		}
		return result;
	}
} // namespace HomeskzIfcImport::core
