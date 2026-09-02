//
//	core/Geometry.cpp
//
//	Mat4（4x4 同次変換行列）の実装。Vec2/Vec3 の演算はヘッダに inline で置き、
//	行列の生成・合成・適用だけをここに分ける（凸多角形の矩形クリップも同じ理由でここ）。
//	純粋な数値計算で SDK 非依存（core/Geometry.h の方針に従う）。GeometryTests で
//	手計算値と突き合わせる。
//

#include "core/Geometry.h"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::core
{
	Mat4 Mat4::identity()
	{
		Mat4 result;
		for (std::size_t i = 0; i < 4; ++i)
			result.m[i][i] = 1.0;
		return result;
	}

	Mat4 Mat4::translation(const Vec3& t)
	{
		Mat4 result = identity();
		result.m[0][3] = t.x;
		result.m[1][3] = t.y;
		result.m[2][3] = t.z;
		return result;
	}

	Mat4 Mat4::fromAxes(const Vec3& xAxis, const Vec3& yAxis, const Vec3& zAxis, const Vec3& origin)
	{
		// 列 0/1/2 に基底ベクトル、列 3 に原点を並べる。最下行は (0,0,0,1)。
		Mat4 result;
		result.m[0][0] = xAxis.x;
		result.m[1][0] = xAxis.y;
		result.m[2][0] = xAxis.z;
		result.m[0][1] = yAxis.x;
		result.m[1][1] = yAxis.y;
		result.m[2][1] = yAxis.z;
		result.m[0][2] = zAxis.x;
		result.m[1][2] = zAxis.y;
		result.m[2][2] = zAxis.z;
		result.m[0][3] = origin.x;
		result.m[1][3] = origin.y;
		result.m[2][3] = origin.z;
		result.m[3][3] = 1.0;
		return result;
	}

	Vec3 Mat4::transformPoint(const Vec3& p) const
	{
		// w=1 として回転＋平行移動を適用する。
		return Vec3{(m[0][0] * p.x) + (m[0][1] * p.y) + (m[0][2] * p.z) + m[0][3],
					(m[1][0] * p.x) + (m[1][1] * p.y) + (m[1][2] * p.z) + m[1][3],
					(m[2][0] * p.x) + (m[2][1] * p.y) + (m[2][2] * p.z) + m[2][3]};
	}

	Vec3 Mat4::transformDirection(const Vec3& d) const
	{
		// 平行移動（第 4 列）を無視し、回転 3x3 だけを適用する。
		return Vec3{(m[0][0] * d.x) + (m[0][1] * d.y) + (m[0][2] * d.z),
					(m[1][0] * d.x) + (m[1][1] * d.y) + (m[1][2] * d.z),
					(m[2][0] * d.x) + (m[2][1] * d.y) + (m[2][2] * d.z)};
	}

	Mat4 operator*(const Mat4& a, const Mat4& b)
	{
		Mat4 result;
		for (std::size_t row = 0; row < 4; ++row)
		{
			for (std::size_t col = 0; col < 4; ++col)
			{
				double sum = 0.0;
				for (std::size_t k = 0; k < 4; ++k)
					sum += a.m[row][k] * b.m[k][col];
				result.m[row][col] = sum;
			}
		}
		return result;
	}

	namespace
	{
		// 矩形の 4 辺。Sutherland–Hodgman はこの順に半平面で切っていく。
		enum class ClipEdge
		{
			Left,
			Right,
			Bottom,
			Top,
		};

		// 点が辺の内側（残す側）にあるか。
		bool insideEdge(const Vec2& point, ClipEdge edge, const Vec2& min, const Vec2& max)
		{
			switch (edge)
			{
			case ClipEdge::Left:
				return point.x >= min.x;
			case ClipEdge::Right:
				return point.x <= max.x;
			case ClipEdge::Bottom:
				return point.y >= min.y;
			case ClipEdge::Top:
				break;
			}
			return point.y <= max.y;
		}

		// 辺が「どの座標を何の値で切るか」。vertical なら x、そうでなければ y を limit で切る。
		void edgeCut(ClipEdge edge, const Vec2& min, const Vec2& max, bool& outVertical,
					 double& outLimit)
		{
			switch (edge)
			{
			case ClipEdge::Left:
				outVertical = true;
				outLimit = min.x;
				return;
			case ClipEdge::Right:
				outVertical = true;
				outLimit = max.x;
				return;
			case ClipEdge::Bottom:
				outVertical = false;
				outLimit = min.y;
				return;
			case ClipEdge::Top:
				break;
			}
			outVertical = false;
			outLimit = max.y;
		}

		// 線分 from→to が辺の直線と交わる点。呼び出し側は「一方が内側・他方が外側」と
		// 分かっているときだけ呼ぶので、分母が 0 になることはない（それでも念のため
		// 0 除算だけは避ける）。
		Vec2 intersectEdge(const Vec2& from, const Vec2& to, ClipEdge edge, const Vec2& min,
						   const Vec2& max)
		{
			bool vertical = false;
			double limit = 0.0;
			edgeCut(edge, min, max, vertical, limit);
			const double span = vertical ? (to.x - from.x) : (to.y - from.y);
			if (std::abs(span) < kGeomEps)
				return to;
			const double t = (limit - (vertical ? from.x : from.y)) / span;
			return Vec2{from.x + ((to.x - from.x) * t), from.y + ((to.y - from.y) * t)};
		}
	} // namespace

	std::vector<Vec2> clipPolygonToRect(const std::vector<Vec2>& polygon, const Vec2& min,
										const Vec2& max)
	{
		std::vector<Vec2> current = polygon;
		for (const ClipEdge edge :
			 {ClipEdge::Left, ClipEdge::Right, ClipEdge::Bottom, ClipEdge::Top})
		{
			if (current.size() < 3)
				return {};

			std::vector<Vec2> next;
			next.reserve(current.size() + 1);
			const std::size_t count = current.size();
			for (std::size_t i = 0; i < count; ++i)
			{
				const Vec2& from = current[(i + count - 1) % count];
				const Vec2& to = current[i];
				const bool fromIn = insideEdge(from, edge, min, max);
				const bool toIn = insideEdge(to, edge, min, max);
				if (toIn)
				{
					if (!fromIn)
						next.push_back(intersectEdge(from, to, edge, min, max));
					next.push_back(to);
				}
				else if (fromIn)
				{
					next.push_back(intersectEdge(from, to, edge, min, max));
				}
			}
			current = std::move(next);
		}
		return current.size() >= 3 ? current : std::vector<Vec2>{};
	}

	// --- 多角形の基本（符号付き面積・内外判定・オフセット）----------------------------

	double shoelaceSigned(const std::vector<Vec2>& polygon)
	{
		double total = 0.0;
		const std::size_t n = polygon.size();
		for (std::size_t i = 0; i < n; ++i)
		{
			const Vec2& a = polygon[i];
			const Vec2& b = polygon[(i + 1) % n];
			total += (a.x * b.y) - (b.x * a.y);
		}
		return total / 2.0;
	}

	bool pointInPolygon(const Vec2& point, const std::vector<Vec2>& polygon)
	{
		const std::size_t n = polygon.size();
		if (n < 3)
			return false;
		bool inside = false;
		for (std::size_t i = 0, j = n - 1; i < n; j = i++)
		{
			const Vec2& a = polygon[i];
			const Vec2& b = polygon[j];
			if ((a.y > point.y) != (b.y > point.y))
			{
				const double x = a.x + ((point.y - a.y) * (b.x - a.x) / (b.y - a.y));
				if (point.x < x)
					inside = !inside;
			}
		}
		return inside;
	}

	bool lineIntersection(const Vec2& p1, const Vec2& d1, const Vec2& p2, const Vec2& d2, Vec2& out)
	{
		const double denom = cross(d1, d2);
		if (std::abs(denom) < 1e-12)
			return false;
		const double t = cross(p2 - p1, d2) / denom;
		out = p1 + (d1 * t);
		return true;
	}

	std::vector<Vec2> offsetPolygon(const std::vector<Vec2>& polygon,
									const std::vector<double>& dists)
	{
		const std::size_t n = polygon.size();
		if (n < 3 || dists.size() != n)
			return polygon; // 距離が辺の数と合わない入力は動かさない（呼び出し側の取り違え）
		std::vector<std::pair<Vec2, Vec2>> lines; // (点, 方向)
		lines.reserve(n);
		for (std::size_t i = 0; i < n; ++i)
		{
			const Vec2& a = polygon[i];
			const Vec2 edge = polygon[(i + 1) % n] - a;
			const double len = length(edge);
			if (len <= 0.0)
			{
				lines.emplace_back(a, Vec2{1.0, 0.0});
				continue;
			}
			const Vec2 u{edge.x / len, edge.y / len};
			// CCW 多角形の外向き法線＝進行方向の右。
			lines.emplace_back(Vec2{a.x + (dists[i] * u.y), a.y - (dists[i] * u.x)}, u);
		}

		std::vector<Vec2> out;
		out.reserve(n);
		for (std::size_t i = 0; i < n; ++i)
		{
			const auto& [q1, d1] = lines[(i + n - 1) % n];
			const auto& [q2, d2] = lines[i];
			Vec2 vertex;
			if (!lineIntersection(q1, d1, q2, d2, vertex))
			{
				// 平行（同一直線の連続辺）: 法線方向へずらした点で代用する。
				vertex = Vec2{polygon[i].x + (dists[i] * d2.y), polygon[i].y - (dists[i] * d2.x)};
			}
			out.push_back(vertex);
		}
		return out;
	}
} // namespace HomeskzIfcImport::core
