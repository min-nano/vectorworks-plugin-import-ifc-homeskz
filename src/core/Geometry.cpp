//
//	core/Geometry.cpp
//
//	Mat4（4x4 同次変換行列）の実装。Vec2/Vec3 の演算はヘッダに inline で置き、
//	行列の生成・合成・適用だけをここに分ける（凸多角形の矩形クリップも同じ理由でここ）。
//	純粋な数値計算で SDK 非依存（core/Geometry.h の方針に従う）。GeometryTests で
//	手計算値と突き合わせる。
//

#include "core/Geometry.h"

#include <cstddef>
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

	std::vector<Vec2> clipPolygonToHalfPlane(const std::vector<Vec2>& polygon, const Vec2& origin,
											 const Vec2& direction)
	{
		// Sutherland–Hodgman の 1 段。直線 origin + t·direction の**左手側**
		// （cross(direction, p − origin) ≥ 0）を残す。
		const auto side = [&](const Vec2& point) { return cross(direction, point - origin); };

		std::vector<Vec2> next;
		next.reserve(polygon.size() + 1);
		const std::size_t count = polygon.size();
		for (std::size_t i = 0; i < count; ++i)
		{
			const Vec2& from = polygon[(i + count - 1) % count];
			const Vec2& to = polygon[i];
			const double fromSide = side(from);
			const double toSide = side(to);
			const bool fromIn = fromSide >= 0.0;
			const bool toIn = toSide >= 0.0;
			// 交点。「一方が内側・他方が外側」のときだけ求めるので分母は 0 にならない
			// （それでも念のため 0 除算だけは避ける）。
			const auto crossing = [&]
			{
				const double denominator = fromSide - toSide;
				if (std::abs(denominator) < kGeomEps)
					return to;
				return from + ((to - from) * (fromSide / denominator));
			};
			if (toIn)
			{
				if (!fromIn)
					next.push_back(crossing());
				next.push_back(to);
			}
			else if (fromIn)
			{
				next.push_back(crossing());
			}
		}
		return next.size() >= 3 ? next : std::vector<Vec2>{};
	}

	std::vector<Vec2> clipPolygonToConvex(const std::vector<Vec2>& polygon,
										  const std::vector<Vec2>& clip)
	{
		if (clip.size() < 3)
			return {};

		// 切る側の辺を 1 本ずつ半平面として当てていく。clip は反時計回りなので、
		// 辺 a→b の**左手側**が残す側（clipPolygonToHalfPlane の向きそのまま）。
		std::vector<Vec2> current = polygon;
		const std::size_t edges = clip.size();
		for (std::size_t e = 0; e < edges; ++e)
		{
			if (current.size() < 3)
				return {};
			const Vec2& a = clip[e];
			current = clipPolygonToHalfPlane(current, a, clip[(e + 1) % edges] - a);
		}
		return current;
	}
} // namespace HomeskzIfcImport::core
