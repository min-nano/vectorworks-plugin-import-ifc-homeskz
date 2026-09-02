//
//	core/Foundation.cpp
//
//	基礎の命令（FoundationCommand）から押し出しソリッド群を組み立てる純計算と、PIO の
//	レコードへ保存する直列化の実装。意図は core/Foundation.h を参照。
//	【SDK 非依存】core/ は VectorWorks SDK を一切 include しない。
//
//	地中梁の床付け（捨てコン・砕石）の断面の組み立ては M17（docs/DEV-NOTES.md「基礎の
//	床付け」）を parse/Footing からそのまま移したもの——PIO がパラメータの変更のたびに
//	床付けを描き直すので、解析側ではなく**部品から描く側**（＝ここ）に住む。
//

#include "core/Foundation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// --- 小さな共通ヘルパー ---------------------------------------------------

		// 符号付き面積（CCW で正・CW で負）。
		double shoelaceSigned(const std::vector<Vec2>& pts)
		{
			double total = 0.0;
			const std::size_t n = pts.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& a = pts[i];
				const Vec2& b = pts[(i + 1) % n];
				total += (a.x * b.y) - (b.x * a.y);
			}
			return total / 2.0;
		}

		// 多角形の重心（頂点の相加平均）。空なら原点。
		Vec2 polygonCentroid(const std::vector<Vec2>& pts)
		{
			if (pts.empty())
				return Vec2{};
			double sx = 0.0;
			double sy = 0.0;
			for (const Vec2& p : pts)
			{
				sx += p.x;
				sy += p.y;
			}
			const auto n = static_cast<double>(pts.size());
			return Vec2{sx / n, sy / n};
		}

		// 平面外形の代表点（重心・各頂点・各辺の中点）。
		std::vector<Vec2> footprintSamples(const std::vector<Vec2>& pts)
		{
			std::vector<Vec2> samples;
			samples.reserve((pts.size() * 2) + 1);
			samples.push_back(polygonCentroid(pts));
			samples.insert(samples.end(), pts.begin(), pts.end());
			const std::size_t n = pts.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& a = pts[i];
				const Vec2& b = pts[(i + 1) % n];
				samples.push_back(Vec2{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0});
			}
			return samples;
		}

		// 点が多角形の内側か（交差数の偶奇）。辺の上は「内側」に数える。
		bool pointInPolygon(const Vec2& p, const std::vector<Vec2>& poly)
		{
			const std::size_t n = poly.size();
			if (n < 3)
				return false;
			bool inside = false;
			for (std::size_t i = 0, j = n - 1; i < n; j = i++)
			{
				const Vec2& a = poly[i];
				const Vec2& b = poly[j];
				if ((a.y > p.y) != (b.y > p.y))
				{
					const double x = a.x + ((p.y - a.y) * (b.x - a.x) / (b.y - a.y));
					if (p.x < x)
						inside = !inside;
				}
			}
			return inside;
		}

		// 点 p・方向 d の 2 直線の交点（平行なら false）。
		bool lineIntersection(const Vec2& p1, const Vec2& d1, const Vec2& p2, const Vec2& d2,
							  Vec2& out)
		{
			const double denom = (d1.x * d2.y) - (d1.y * d2.x);
			if (std::abs(denom) < 1e-12)
				return false;
			const double dx = p2.x - p1.x;
			const double dy = p2.y - p1.y;
			const double t = ((dx * d2.y) - (dy * d2.x)) / denom;
			out = Vec2{p1.x + (t * d1.x), p1.y + (t * d1.y)};
			return true;
		}

		// 許容値で丸めた整数キー（代表値の集計の鍵）。
		long long roundKey(double value, double tolerance)
		{
			return std::llround(value / tolerance);
		}

		// --- 地中梁の床付け（捨てコン・砕石）--------------------------------------------
		//
		// 床付けは「地中梁の**下面**（側面 → 下端 → 側面 と続く折れ線。天端の辺は含まない）を
		// 外向きへ kSlabBeddingThickness だけオフセットした帯」で表す。オフセットは辺ごとに
		// 法線方向へ動かして隣どうしの交点（マイター）を新しい頂点にするので、**傾斜部でも
		// 法線方向の厚みがそのまま保たれる**（下端の下では真下へ 130mm、45 度の傾斜部では
		// 斜め下へ 130mm）。帯のうち下端の直下の 30mm だけが捨てコンで、残りは砕石になる。
		//
		// 外周部の側面（底盤の外形に面した側）は例外で、側面を回り込ませずに下端の床付けを
		// kBeddingPerimeterMargin だけ横へ張り出して終わらせる（外にはコンクリートが無いので、
		// 回り込ませると建物の外に砕石の壁が立ってしまう）。

		// 床付けの断面を組み立てるときの許容値。天端／下端の辺とみなす v の差（mm）と、
		// 辺の長さが 0 でないとみなす下限（mm）。
		constexpr double kBeddingLevelTol = 0.5;
		constexpr double kBeddingEdgeEps = 1e-6;

		// 連続する同一点と末尾の閉じ重複を除いた頂点列。
		std::vector<Vec2> dedupeRing(const std::vector<Vec2>& pts)
		{
			std::vector<Vec2> out;
			out.reserve(pts.size());
			for (const Vec2& p : pts)
			{
				if (out.empty() || !samePoint(out.back(), p, kBeddingEdgeEps))
					out.push_back(p);
			}
			while (out.size() > 1 && samePoint(out.front(), out.back(), kBeddingEdgeEps))
				out.pop_back();
			return out;
		}

		// 地中梁の断面を**下面の折れ線**へ分解する。折れ線は天端の辺を除いた残りで、u が
		// 小さい側の天端頂点から始まり、下端の辺を通って u が大きい側の天端頂点で終わる。
		// first / last には折れ線のうち下端（v 最小）の辺の最初・最後の添字が入る。
		// 天端の辺か下端の辺が見つからない断面（三角形・高さ 0 等）は false。
		bool groundBeamUnderside(const std::vector<Vec2>& profile, std::vector<Vec2>& path,
								 std::size_t& first, std::size_t& last)
		{
			std::vector<Vec2> ring = dedupeRing(profile);
			if (ring.size() < 3)
				return false;
			if (shoelaceSigned(ring) < 0.0)
				std::ranges::reverse(ring); // 以降は CCW（外向き法線＝進行方向の右）前提

			double vTop = ring.front().y;
			double vBot = ring.front().y;
			for (const Vec2& p : ring)
			{
				vTop = std::max(vTop, p.y);
				vBot = std::min(vBot, p.y);
			}
			if (vTop - vBot <= kBeddingLevelTol)
				return false; // せいが無い（水平な板）

			// 天端の辺＝両端が v 最大の辺。CCW なので u の大きい側 → 小さい側へ向かう。
			const std::size_t n = ring.size();
			std::size_t top = n;
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& a = ring[i];
				const Vec2& b = ring[(i + 1) % n];
				if (a.y >= vTop - kBeddingLevelTol && b.y >= vTop - kBeddingLevelTol &&
					b.x < a.x - kBeddingEdgeEps)
				{
					top = i;
					break;
				}
			}
			if (top == n)
				return false;

			path.clear();
			path.reserve(n);
			for (std::size_t k = 0; k < n; ++k)
				path.push_back(ring[(top + 1 + k) % n]);

			// 下端の辺＝v 最小の頂点が並ぶ区間。
			bool found = false;
			first = 0;
			last = 0;
			for (std::size_t i = 0; i < path.size(); ++i)
			{
				if (path[i].y > vBot + kBeddingLevelTol)
					continue;
				if (!found)
				{
					first = i;
					found = true;
				}
				last = i;
			}
			return found && last > first && path[last].x > path[first].x + kBeddingEdgeEps;
		}

		// 地中梁の側面（断面 u の最小側・最大側）が外周部かを判定する。側面のすぐ外側を
		// 押し出し方向の 3 か所で突き、**すべてが底盤の外形の外**なら外周部とみなす。
		// 一部だけ外に出る（外形の角を跨ぐ）地中梁を外周部と読まないよう、全点一致にする。
		// 底盤の外形が無ければどちらも外周部ではない。
		void groundBeamPerimeterSides(const BeamPrism& prism, const std::vector<Vec2>& slabRing,
									  bool& low, bool& high)
		{
			low = false;
			high = false;
			if (prism.profile.empty() || slabRing.size() < 3)
				return;
			double uLo = prism.profile.front().x;
			double uHi = uLo;
			for (const Vec2& p : prism.profile)
			{
				uLo = std::min(uLo, p.x);
				uHi = std::max(uHi, p.x);
			}

			Vec2 axis;
			Vec2 width;
			beamPrismAxes(prism, axis, width);
			const auto outside = [&](double u)
			{
				constexpr std::array<double, 3> kProbeFractions{0.25, 0.5, 0.75};
				return std::ranges::all_of(
					kProbeFractions,
					[&](double fraction)
					{
						const double along = prism.depth * fraction;
						return !pointInPolygon(
							Vec2{prism.origin.x + (axis.x * along) + (width.x * u),
								 prism.origin.y + (axis.y * along) + (width.y * u)},
							slabRing);
					});
			};
			low = outside(uLo - kBeddingOutsideProbe);
			high = outside(uHi + kBeddingOutsideProbe);
		}

		// 断面を半平面 v ≤ top で切った多角形（Sutherland-Hodgman）。床付けの帯を切り上げる
		// のに使う。**切っても 2 つに割れない**ことは形が保証する——床付けは下端の下で全幅に
		// 繋がっており、top は必ずその繋がりより上にあるので、落ちるのは左右の帯の上端だけに
		// なる。面にならなくなったら空を返す。
		std::vector<Vec2> clipProfileBelow(const std::vector<Vec2>& profile, double top)
		{
			std::vector<Vec2> out;
			const std::size_t n = profile.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& a = profile[i];
				const Vec2& b = profile[(i + 1) % n];
				const bool aIn = a.y <= top;
				const bool bIn = b.y <= top;
				if (aIn)
					out.push_back(a);
				if (aIn != bIn)
				{
					const double dv = b.y - a.y;
					const double t = (top - a.y) / dv; // aIn != bIn なので dv は 0 でない
					out.push_back(Vec2{a.x + ((b.x - a.x) * t), top});
				}
			}
			out = dedupeRing(out);
			return out.size() >= 3 ? out : std::vector<Vec2>{};
		}

		// 床付けを 1 つ足す。**同じ層（クラスと断面が同じ）で押し出し方向に続く区間は
		// 1 本へ繋ぐ**（区間を切ったせいで、断面の変わらない捨てコンまで細切れのソリッドに
		// なるのを防ぐ）。
		void appendBedding(std::vector<BeddingPrism>& out, BeddingPrism bedding)
		{
			for (BeddingPrism& previous : out)
			{
				if (previous.drawClass != bedding.drawClass ||
					previous.profile.size() != bedding.profile.size() ||
					std::abs((previous.start + previous.depth) - bedding.start) > kBeddingEdgeEps)
					continue;
				bool same = true;
				for (std::size_t i = 0; i < previous.profile.size() && same; ++i)
					same = samePoint(previous.profile[i], bedding.profile[i], kBeddingEdgeEps);
				if (!same)
					continue;
				previous.depth = (bedding.start + bedding.depth) - previous.start;
				return;
			}
			out.push_back(std::move(bedding));
		}

		// 押し出し方向の区間 [start, start + depth) と、その区間で床付けを切り上げる高さ。
		struct BeddingSpan
		{
			double start = 0.0;
			double depth = 0.0;
			double top = 0.0;
		};

		// 地中梁 A の床付けを、押し出し方向の区間へ切り分ける。取り合う地中梁が 1 つも無ければ
		// 全長 1 区間（top = 底盤の砕石の底）。掛かる地中梁がある区間は、その相手の**下端**まで
		// 切り下げる（相手のコンクリートがある高さには床付けを置かない）。
		//
		// 掛かるかどうかは A の**床付けの平面外形**（帯を含む幅）と相手のコンクリートの平面外形
		// で見る。相手の外形の 4 隅を A の (t, u) 座標へ写した外接矩形を使うので、直交・平行な
		// 取り合い（実データはこの 2 通りしか無い）では厳密、斜めでは安全側（広めに切る）になる。
		std::vector<BeddingSpan> beddingSpans(const std::vector<BeamPrism>& beams, std::size_t self,
											  double slabTop, double beddingWidthLo,
											  double beddingWidthHi)
		{
			const BeamPrism& prism = beams[self];
			Vec2 axis;
			Vec2 width;
			beamPrismAxes(prism, axis, width);

			// 掛かる区間（[t0, t1] と、そこで切り下げる高さ）を集める。**自分自身は除く**。
			std::vector<BeddingSpan> blocks;
			for (std::size_t i = 0; i < beams.size(); ++i)
			{
				if (i == self)
					continue;
				const BeamPrism& other = beams[i];
				if (other.profile.empty())
					continue; // 断面を持たない地中梁は掛かりようがない
				const std::vector<Vec2> footprint = beamPrismFootprint(other);
				double tLo = 0.0;
				double tHi = 0.0;
				double uLo = 0.0;
				double uHi = 0.0;
				bool first = true;
				for (const Vec2& corner : footprint)
				{
					const double dx = corner.x - prism.origin.x;
					const double dy = corner.y - prism.origin.y;
					const double t = (dx * axis.x) + (dy * axis.y);
					const double u = (dx * width.x) + (dy * width.y);
					tLo = first ? t : std::min(tLo, t);
					tHi = first ? t : std::max(tHi, t);
					uLo = first ? u : std::min(uLo, u);
					uHi = first ? u : std::max(uHi, u);
					first = false;
				}
				if (uHi <= beddingWidthLo || uLo >= beddingWidthHi)
					continue; // 幅方向で離れている（床付けに掛からない）
				const double start = std::max(tLo, 0.0);
				const double end = std::min(tHi, prism.depth);
				if (end - start <= kBeddingEdgeEps)
					continue; // 押し出し方向で離れている
				blocks.push_back(BeddingSpan{start, end - start, other.origin.z - prism.origin.z});
			}

			if (blocks.empty())
				return {BeddingSpan{0.0, prism.depth, slabTop}};

			// 区切り位置で細切れにし、各区間の切り上げ高さ＝掛かる相手の下端の最小値
			// （と底盤の砕石の底）を採る。並びは押し出し方向で決定的。
			std::vector<double> breaks{0.0, prism.depth};
			for (const BeddingSpan& block : blocks)
			{
				breaks.push_back(block.start);
				breaks.push_back(block.start + block.depth);
			}
			std::ranges::sort(breaks);

			std::vector<BeddingSpan> spans;
			for (std::size_t i = 0; i + 1 < breaks.size(); ++i)
			{
				const double start = breaks[i];
				const double end = breaks[i + 1];
				if (end - start <= kBeddingEdgeEps)
					continue;
				const double middle = (start + end) / 2.0;
				double top = slabTop;
				for (const BeddingSpan& block : blocks)
				{
					if (middle > block.start && middle < block.start + block.depth)
						top = std::min(top, block.top);
				}
				// 切り上げ高さの同じ区間は 1 つにまとめる（無用な継目を作らない）。
				if (!spans.empty() && std::abs(spans.back().top - top) <= kBeddingEdgeEps)
					spans.back().depth = end - spans.back().start;
				else
					spans.push_back(BeddingSpan{start, end - start, top});
			}
			return spans;
		}

		// --- 部品 → ソリッド ---------------------------------------------------------

		// プリズム（断面 ＋ 水平押し出し）を 3D 多角形＋押し出しベクトルへ写す。断面原点
		// （u=v=0）が origin、u 軸が幅軸・v 軸がワールド Z。
		FoundationSolid solidFromPrism(const BeamPrism& prism, FoundationSolid::Kind kind,
									   const std::string& drawClass)
		{
			Vec2 axis;
			Vec2 width;
			beamPrismAxes(prism, axis, width);
			FoundationSolid solid;
			solid.kind = kind;
			solid.drawClass = drawClass;
			solid.base.reserve(prism.profile.size());
			for (const Vec2& p : prism.profile)
			{
				solid.base.push_back(Vec3{prism.origin.x + (width.x * p.x),
										  prism.origin.y + (width.y * p.x), prism.origin.z + p.y});
			}
			solid.extent = Vec3{axis.x * prism.depth, axis.y * prism.depth, 0.0};
			return solid;
		}

		// 立上りの平面矩形（壁芯の両側へ半幅）。長さ・幅が無ければ空。
		std::vector<Vec2> riserFootprint(const FoundationRiser& riser)
		{
			const Vec2 delta = riser.end - riser.start;
			const double length = core::length(delta);
			if (length <= 0.0 || riser.width <= 0.0)
				return {};
			const double half = riser.width / 2.0;
			const Vec2 n{-delta.y / length * half, delta.x / length * half};
			return {riser.start - n, riser.end - n, riser.end + n, riser.start + n};
		}

		// 面積／長さで重み付けした最頻値。値は kFoundationTol で丸めた鍵でまとめ、重みの
		// 合計が最大の鍵の値（同率なら大きい値）を返す。鍵の中では最大値を代表にする（同じ
		// 鍵の値は許容内で同一とみなせる）。空なら 0。
		double weightedMode(const std::vector<std::pair<double, double>>& valueWeights)
		{
			std::map<long long, std::pair<double, double>> groups; // 鍵 → (重み合計, 代表値)
			for (const auto& [value, weight] : valueWeights)
			{
				auto& entry = groups[roundKey(value, kFoundationTol)];
				if (entry.first == 0.0)
					entry.second = value;
				entry.first += weight;
				entry.second = std::max(entry.second, value);
			}
			double bestWeight = -1.0;
			double best = 0.0;
			for (const auto& [key, entry] : groups)
			{
				if (entry.first > bestWeight || (entry.first == bestWeight && entry.second > best))
				{
					bestWeight = entry.first;
					best = entry.second;
				}
			}
			return best;
		}

		// --- 直列化 ---------------------------------------------------------------

		// 数値を小数 3 桁までの最短表記にする（"150" / "-99.5" / "0.125"）。
		std::string formatNumber(double value)
		{
			std::array<char, 64> buffer{};
			std::snprintf(buffer.data(), buffer.size(), "%.3f", value);
			std::string text(buffer.data());
			if (text.find('.') != std::string::npos)
			{
				while (!text.empty() && text.back() == '0')
					text.pop_back();
				if (!text.empty() && text.back() == '.')
					text.pop_back();
			}
			if (text == "-0")
				text = "0";
			return text;
		}

		void appendNumber(std::string& out, double value)
		{
			out += ' ';
			out += formatNumber(value);
		}

		void appendPoint(std::string& out, const Vec2& point)
		{
			appendNumber(out, point.x);
			appendNumber(out, point.y);
		}

		// 空白区切りの数値列を順に読む。読めなければ false。
		class NumberReader
		{
		public:
			explicit NumberReader(std::string_view text) : fText(text) {}

			bool next(double& out)
			{
				while (fPos < fText.size() && fText[fPos] == ' ')
					++fPos;
				if (fPos >= fText.size())
					return false;
				const std::string token(fText.substr(fPos, fText.find(' ', fPos) - fPos));
				char* end = nullptr;
				const double value = std::strtod(token.c_str(), &end);
				if (end == nullptr || *end != '\0' || !std::isfinite(value))
					return false;
				fPos += token.size();
				out = value;
				return true;
			}

			bool nextPoint(Vec2& out)
			{
				return next(out.x) && next(out.y);
			}

			bool done()
			{
				while (fPos < fText.size() && fText[fPos] == ' ')
					++fPos;
				return fPos >= fText.size();
			}

		private:
			std::string_view fText;
			std::size_t fPos = 0;
		};

		constexpr std::string_view kEncodingMagic = "HF1";
	} // namespace

	// --- 地中梁のプリズム -------------------------------------------------------------

	void beamPrismAxes(const BeamPrism& prism, Vec2& axis, Vec2& width)
	{
		const double phi = prism.azimuth * std::numbers::pi / 180.0;
		axis = Vec2{std::cos(phi), std::sin(phi)};
		width = Vec2{-axis.y, axis.x}; // 幅軸 u（走る向きを +90 度回した水平単位ベクトル）
	}

	std::vector<Vec2> beamPrismFootprint(const BeamPrism& prism)
	{
		if (prism.profile.empty())
			return {};
		Vec2 axis;
		Vec2 width;
		beamPrismAxes(prism, axis, width);
		double uLo = prism.profile.front().x;
		double uHi = prism.profile.front().x;
		for (const Vec2& p : prism.profile)
		{
			uLo = std::min(uLo, p.x);
			uHi = std::max(uHi, p.x);
		}
		const Vec2 start{prism.origin.x, prism.origin.y};
		const Vec2 end{start.x + (axis.x * prism.depth), start.y + (axis.y * prism.depth)};
		return {Vec2{start.x + (width.x * uLo), start.y + (width.y * uLo)},
				Vec2{start.x + (width.x * uHi), start.y + (width.y * uHi)},
				Vec2{end.x + (width.x * uHi), end.y + (width.y * uHi)},
				Vec2{end.x + (width.x * uLo), end.y + (width.y * uLo)}};
	}

	BeamPrism raiseBeamPrismTop(const BeamPrism& prism, double bite)
	{
		if (bite <= 0.0 || prism.profile.empty())
			return prism;

		// 天端＝最大 v。そこから kModifierTopVertexTol 以内の頂点を天端の辺とみなす。
		double vMax = prism.profile.front().y;
		for (const Vec2& p : prism.profile)
			vMax = std::max(vMax, p.y);
		const auto isTop = [&](std::size_t i)
		{ return prism.profile[i].y >= vMax - kModifierTopVertexTol; };

		const std::size_t n = prism.profile.size();
		BeamPrism raised = prism;
		for (std::size_t i = 0; i < n; ++i)
		{
			if (!isTop(i))
				continue;
			const Vec2& top = prism.profile[i];
			// 隣接する 2 頂点のうち**下端側**（側辺の相手）を探し、その斜辺の延長線上へ
			// 動かす。見つからない（天端が水平に分割された中間頂点）／側辺がほぼ水平なら
			// 真上へ上げる。
			double du = 0.0;
			for (const std::size_t j : {(i + n - 1) % n, (i + 1) % n})
			{
				if (isTop(j))
					continue;
				const double dv = top.y - prism.profile[j].y;
				if (std::abs(dv) > kModifierTopVertexTol)
					du = ((top.x - prism.profile[j].x) / dv) * bite;
				break;
			}
			raised.profile[i] = Vec2{top.x + du, top.y + bite};
		}
		return raised;
	}

	std::vector<BeddingPrism> groundBeamBedding(const BeamPrism& prism, bool lowPerimeter,
												bool highPerimeter, double topLimit,
												const std::string& leanClass,
												const std::string& gravelClass)
	{
		std::vector<Vec2> path;
		std::size_t first = 0;
		std::size_t last = 0;
		if (!groundBeamUnderside(prism.profile, path, first, last))
			return {}; // 床付けを敷く下面を取り出せない断面（三角形・水平な板等）

		const double vTop = path.front().y; // 天端＝底盤の底面
		const double vBot = path[first].y;	// 下端（v=0。地中梁の底）
		const double uLow = path[first].x;	// 下端の辺の u 小さい側
		const double uHigh = path[last].x;	// 同 大きい側

		// オフセットする辺の範囲。外周部の側面はここから外し、下端の床付けを横へ張り出して
		// 終わらせる。
		const std::size_t begin = lowPerimeter ? first : 0;
		const std::size_t end = highPerimeter ? last : path.size() - 1;

		// 各辺を外向き法線へ kSlabBeddingThickness だけ動かした線分。
		//
		// **辺は必ず 1 本以上あり、長さも必ず正**なので、空判定も 0 除算の番人も要らない:
		// begin ≤ first < last ≤ end（下端の辺が 1 本以上あることは groundBeamUnderside が
		// 保証する）だから範囲は空にならず、折れ線の連続する 2 点は dedupeRing が
		// kBeddingEdgeEps 以上離れていることを保証している。
		struct OffsetEdge
		{
			Vec2 start;
			Vec2 end;
			Vec2 dir;
		};
		std::vector<OffsetEdge> edges;
		edges.reserve(end - begin);
		for (std::size_t i = begin; i < end; ++i)
		{
			const Vec2 delta = path[i + 1] - path[i];
			const double length = core::length(delta);
			const Vec2 dir{delta.x / length, delta.y / length};
			// CCW ポリゴンの外向き法線＝進行方向の右。
			const Vec2 offset{dir.y * kSlabBeddingThickness, -dir.x * kSlabBeddingThickness};
			edges.push_back(OffsetEdge{path[i] + offset, path[i + 1] + offset, dir});
		}

		// オフセット線を天端（v = vTop）まで伸ばした点。ほぼ水平な辺では伸ばせないので
		// 与えられた端点で代用する。
		const auto atTop = [vTop](const Vec2& point, const Vec2& dir, const Vec2& fallback)
		{
			if (std::abs(dir.y) <= kBeddingEdgeEps)
				return fallback;
			const double t = (vTop - point.y) / dir.y;
			return Vec2{point.x + (dir.x * t), vTop};
		};

		// 外側の折れ線（u の小さい側 → 大きい側）。
		const double beddingBottom = vBot - kSlabBeddingThickness;
		std::vector<Vec2> outer;
		outer.reserve(edges.size() + 2);
		outer.push_back(lowPerimeter
							? Vec2{uLow - kBeddingPerimeterMargin, beddingBottom}
							: atTop(edges.front().start, edges.front().dir, edges.front().start));
		for (std::size_t i = 1; i < edges.size(); ++i)
		{
			Vec2 vertex;
			if (!lineIntersection(edges[i - 1].start, edges[i - 1].dir, edges[i].start,
								  edges[i].dir, vertex))
				vertex = edges[i].start; // 平行（同一直線の連続辺）: マイターは要らない
			outer.push_back(vertex);
		}
		outer.push_back(highPerimeter
							? Vec2{uHigh + kBeddingPerimeterMargin, beddingBottom}
							: atTop(edges.back().end, edges.back().dir, edges.back().end));

		// 内側の折れ線（u の大きい側 → 小さい側）。地中梁の下面をなぞり、下端の下だけは
		// 捨てコンの底（vBot − 30）を通る＝そこが砕石と捨てコンの境になる。
		const double leanBottom = vBot - kSlabLeanConcreteThickness;
		const double uLeanLow = lowPerimeter ? uLow - kBeddingPerimeterMargin : uLow;
		const double uLeanHigh = highPerimeter ? uHigh + kBeddingPerimeterMargin : uHigh;
		std::vector<Vec2> inner;
		inner.reserve(path.size() + 4);
		if (highPerimeter)
		{
			inner.push_back(Vec2{uLeanHigh, leanBottom});
		}
		else
		{
			for (std::size_t i = path.size(); i > last; --i)
				inner.push_back(path[i - 1]); // 天端 → 下端（u 大きい側の側面）
			inner.push_back(Vec2{uHigh, leanBottom});
		}
		inner.push_back(Vec2{uLeanLow, leanBottom});
		if (!lowPerimeter)
		{
			for (std::size_t i = first + 1; i > 0; --i)
				inner.push_back(path[i - 1]); // 下端（u 小さい側）→ 天端
		}

		// 各層は最後に v ≤ topLimit で切り上げる（傾斜部の帯が直交する地中梁へ食い込むのを
		// 防ぐ）。面にならなくなった層は落とす。
		std::vector<BeddingPrism> beddings;
		// 捨てコンは下端の平らな面の直下だけ（傾斜部は砕石のみ）。
		std::vector<Vec2> lean =
			clipProfileBelow({Vec2{uLeanLow, leanBottom}, Vec2{uLeanHigh, leanBottom},
							  Vec2{uLeanHigh, vBot}, Vec2{uLeanLow, vBot}},
							 topLimit);
		if (!lean.empty())
			beddings.push_back(BeddingPrism{std::move(lean), leanClass, 0.0, prism.depth});

		std::vector<Vec2> gravel = outer;
		gravel.insert(gravel.end(), inner.begin(), inner.end());
		gravel = clipProfileBelow(dedupeRing(gravel), topLimit);
		if (!gravel.empty())
			beddings.push_back(BeddingPrism{std::move(gravel), gravelClass, 0.0, prism.depth});
		return beddings;
	}

	// --- 地中梁の断面のパラメータ化 -----------------------------------------------------

	BeamPrism beamPrism(const FoundationBeam& beam)
	{
		BeamPrism prism;
		const Vec2 delta = beam.end - beam.start;
		const double length = core::length(delta);
		prism.depth = length;
		prism.origin = Vec3{beam.start.x, beam.start.y, beam.top - beam.depth};
		if (length <= 0.0)
			return prism; // 長さの無い地中梁は断面を持たない
		prism.azimuth = std::atan2(delta.y, delta.x) * 180.0 / std::numbers::pi;

		const double half = beam.bottomWidth / 2.0;
		const double haunch = std::clamp(beam.haunchHeight, 0.0, beam.depth);
		const double kink = beam.depth - haunch; // 鉛直部の上端（斜め部の始まり）
		const double left = std::max(beam.haunchLeft, 0.0);
		const double right = std::max(beam.haunchRight, 0.0);

		// 下端 → +u 側の側面 → 天端 → −u 側の側面（CCW）。張り出しの無い側は鉛直 1 本で、
		// 中間頂点を作らない。
		prism.profile.push_back(Vec2{-half, 0.0});
		prism.profile.push_back(Vec2{half, 0.0});
		if (left > 0.0 && kink > 0.0)
			prism.profile.push_back(Vec2{half, kink});
		prism.profile.push_back(Vec2{half + left, beam.depth});
		prism.profile.push_back(Vec2{-half - right, beam.depth});
		if (right > 0.0 && kink > 0.0)
			prism.profile.push_back(Vec2{-half, kink});
		return prism;
	}

	bool fitFoundationBeam(const BeamPrism& prism, FoundationBeam& out)
	{
		if (prism.profile.size() < 3 || prism.depth <= 0.0)
			return false;

		Vec2 axis;
		Vec2 width;
		beamPrismAxes(prism, axis, width);

		double uLow = 0.0;
		double uHigh = 0.0;
		double vTop = 0.0;
		double vBot = 0.0;
		FoundationBeam beam;

		std::vector<Vec2> path;
		std::size_t first = 0;
		std::size_t last = 0;
		if (groundBeamUnderside(prism.profile, path, first, last))
		{
			vTop = path.front().y;
			vBot = path[first].y;
			uLow = path[first].x;
			uHigh = path[last].x;
			const double tLow = path.front().x;
			const double tHigh = path.back().x;
			beam.haunchRight = std::max(uLow - tLow, 0.0);
			beam.haunchLeft = std::max(tHigh - uHigh, 0.0);

			// 斜め部の高さ: 下端の角から天端へ向かう側辺に、下端と同じ u の中間頂点（鉛直部の
			// 上端）があればその高さから上。張り出しのある側で見る（無ければ全高）。
			double haunchHeight = vTop - vBot;
			bool found = false;
			if (beam.haunchRight > kFoundationTol)
			{
				// −u 側（折れ線の先頭〜下端の角）。
				for (std::size_t i = 1; i < first && !found; ++i)
				{
					if (std::abs(path[i].x - uLow) <= kFoundationTol &&
						path[i].y > vBot + kBeddingLevelTol)
					{
						haunchHeight = vTop - path[i].y;
						found = true;
					}
				}
			}
			if (!found && beam.haunchLeft > kFoundationTol)
			{
				// +u 側（下端の角〜折れ線の末尾）。
				for (std::size_t i = last + 1; i + 1 < path.size() && !found; ++i)
				{
					if (std::abs(path[i].x - uHigh) <= kFoundationTol &&
						path[i].y > vBot + kBeddingLevelTol)
					{
						haunchHeight = vTop - path[i].y;
						found = true;
					}
				}
			}
			beam.haunchHeight = haunchHeight;
		}
		else
		{
			// 天端／下端の辺が読めない断面は外接矩形で近似する。
			uLow = prism.profile.front().x;
			uHigh = uLow;
			vTop = prism.profile.front().y;
			vBot = vTop;
			for (const Vec2& p : prism.profile)
			{
				uLow = std::min(uLow, p.x);
				uHigh = std::max(uHigh, p.x);
				vTop = std::max(vTop, p.y);
				vBot = std::min(vBot, p.y);
			}
			if (vTop - vBot <= 0.0 || uHigh - uLow <= 0.0)
				return false;
			beam.haunchHeight = vTop - vBot;
		}

		const double centre = (uLow + uHigh) / 2.0;
		beam.start = Vec2{prism.origin.x + (width.x * centre), prism.origin.y + (width.y * centre)};
		beam.end =
			Vec2{beam.start.x + (axis.x * prism.depth), beam.start.y + (axis.y * prism.depth)};
		beam.bottomWidth = uHigh - uLow;
		beam.depth = vTop - vBot;
		beam.top = prism.origin.z + vTop;
		out = beam;
		return true;
	}

	// --- 代表値とその適用 ------------------------------------------------------------

	FoundationParams foundationBaseParams(const FoundationCommand& command)
	{
		std::vector<std::pair<double, double>> slabTops;
		std::vector<std::pair<double, double>> slabThicknesses;
		for (const FoundationSlab& slab : command.slabs)
		{
			const double area = std::abs(shoelaceSigned(slab.boundary));
			slabTops.emplace_back(slab.top, area);
			slabThicknesses.emplace_back(slab.thickness, area);
		}

		std::vector<std::pair<double, double>> riserWidths;
		std::vector<std::pair<double, double>> riserTops;
		for (const FoundationRiser& riser : command.risers)
		{
			const double length = distance(riser.start, riser.end);
			riserWidths.emplace_back(riser.width, length);
			riserTops.emplace_back(riser.top, length);
		}

		std::vector<std::pair<double, double>> beamDepths;
		std::vector<std::pair<double, double>> haunchWidths;
		std::vector<std::pair<double, double>> haunchHeights;
		for (const FoundationBeam& beam : command.beams)
		{
			const double length = distance(beam.start, beam.end);
			beamDepths.emplace_back(beam.depth, length);
			// 斜め部は張り出しのある側だけを数える（鉛直な面は斜め部を持たない）。
			bool haunched = false;
			for (const double haunch : {beam.haunchLeft, beam.haunchRight})
			{
				if (haunch <= 0.0)
					continue;
				haunchWidths.emplace_back(haunch, length);
				haunched = true;
			}
			if (haunched)
				haunchHeights.emplace_back(beam.haunchHeight, length);
		}

		FoundationParams params;
		params.slabTop = weightedMode(slabTops);
		params.slabThickness = weightedMode(slabThicknesses);
		params.riserWidth = weightedMode(riserWidths);
		params.riserTop = weightedMode(riserTops);
		params.beamDepth = weightedMode(beamDepths);
		params.haunchWidth = weightedMode(haunchWidths);
		params.haunchHeight = weightedMode(haunchHeights);
		return params;
	}

	FoundationCommand applyFoundationParams(const FoundationCommand& imported,
											const FoundationParams& edited)
	{
		const FoundationParams& base = imported.params;
		const double dSlabTop = edited.slabTop - base.slabTop;
		const double dThickness = edited.slabThickness - base.slabThickness;
		const double dSlabBottom = dSlabTop - dThickness; // 底盤の底面の動き
		const double dRiserWidth = edited.riserWidth - base.riserWidth;
		const double dRiserTop = edited.riserTop - base.riserTop;
		const double dBeamDepth = edited.beamDepth - base.beamDepth;
		const double dHaunchWidth = edited.haunchWidth - base.haunchWidth;
		const double dHaunchHeight = edited.haunchHeight - base.haunchHeight;

		FoundationCommand result = imported;
		result.params = edited;
		for (FoundationSlab& slab : result.slabs)
		{
			slab.top += dSlabTop;
			slab.thickness += dThickness;
		}
		for (FoundationRiser& riser : result.risers)
		{
			riser.width += dRiserWidth;
			riser.top += dRiserTop;
			riser.bottom += dSlabBottom;
		}
		for (FoundationBeam& beam : result.beams)
		{
			beam.top += dSlabBottom;
			beam.depth += dBeamDepth;
			if (beam.haunchLeft > 0.0)
				beam.haunchLeft = std::max(beam.haunchLeft + dHaunchWidth, 0.0);
			if (beam.haunchRight > 0.0)
				beam.haunchRight = std::max(beam.haunchRight + dHaunchWidth, 0.0);
			beam.haunchHeight =
				std::clamp(beam.haunchHeight + dHaunchHeight, 0.0, std::max(beam.depth, 0.0));
		}
		return result;
	}

	// --- ソリッドの組み立て ----------------------------------------------------------

	std::vector<std::size_t> attachBeamsToSlabs(const std::vector<FoundationSlab>& slabs,
												const std::vector<BeamPrism>& beams)
	{
		std::vector<std::size_t> result(beams.size(), std::numeric_limits<std::size_t>::max());
		if (slabs.empty())
			return result; // 底盤が 1 枚も無ければ付けられない

		for (std::size_t b = 0; b < beams.size(); ++b)
		{
			const std::vector<Vec2> footprint = beamPrismFootprint(beams[b]);
			if (footprint.empty())
				continue;
			const std::vector<Vec2> samples = footprintSamples(footprint);

			// 代表点が外形内に入る数が最大の底盤へ振り分ける（同数なら添字の小さいほう）。
			std::size_t best = 0;
			std::ptrdiff_t bestCount = 0;
			for (std::size_t i = 0; i < slabs.size(); ++i)
			{
				const auto count =
					std::ranges::count_if(samples, [&slabs, i](const Vec2& sample)
										  { return pointInPolygon(sample, slabs[i].boundary); });
				if (count > bestCount)
				{
					best = i;
					bestCount = count;
				}
			}
			if (bestCount == 0)
			{
				// どの底盤にも入らない（継目・下屋等）: 重心が最も近い底盤へフォールバック
				// して取りこぼさない。
				const Vec2 centroid = polygonCentroid(footprint);
				double bestDistance = 0.0;
				for (std::size_t i = 0; i < slabs.size(); ++i)
				{
					const double d = distance(centroid, polygonCentroid(slabs[i].boundary));
					if (i == 0 || d < bestDistance)
					{
						best = i;
						bestDistance = d;
					}
				}
			}
			result[b] = best;
		}
		return result;
	}

	std::vector<std::vector<BeddingPrism>> foundationBeddings(const FoundationCommand& command)
	{
		std::vector<BeamPrism> beams;
		beams.reserve(command.beams.size());
		for (const FoundationBeam& beam : command.beams)
			beams.push_back(beamPrism(beam));
		const std::vector<std::size_t> slabOf = attachBeamsToSlabs(command.slabs, beams);

		std::vector<std::vector<BeddingPrism>> result(beams.size());
		for (std::size_t index = 0; index < beams.size(); ++index)
		{
			const BeamPrism& prism = beams[index];
			if (prism.profile.empty())
				continue;

			// 付いた底盤の外形（外周部の判定）と、その砕石の底（帯を切り上げる高さ）。底盤に
			// 付かない地中梁は外周部を持たず、帯は天端まで立ち上げる。
			std::vector<Vec2> ring;
			double slabTop = prism.profile.front().y;
			for (const Vec2& p : prism.profile)
				slabTop = std::max(slabTop, p.y);
			if (slabOf[index] < command.slabs.size())
			{
				const FoundationSlab& slab = command.slabs[slabOf[index]];
				ring = slab.boundary;
				const double beddingBottomAbs = slab.top - slab.thickness - kSlabBeddingThickness;
				slabTop = std::max(beddingBottomAbs - prism.origin.z, 0.0); // 下端より下は切らない
			}
			bool lowPerimeter = false;
			bool highPerimeter = false;
			groundBeamPerimeterSides(prism, ring, lowPerimeter, highPerimeter);

			// 帯を含む床付けの幅（区間の判定に使う）。まず全長ぶんを組み立てて幅を測る。
			const std::vector<BeddingPrism> full =
				groundBeamBedding(prism, lowPerimeter, highPerimeter, slabTop,
								  command.leanConcreteClass, command.gravelClass);
			if (full.empty())
				continue;
			double widthLo = full.front().profile.front().x;
			double widthHi = widthLo;
			for (const BeddingPrism& bedding : full)
			{
				for (const Vec2& p : bedding.profile)
				{
					widthLo = std::min(widthLo, p.x);
					widthHi = std::max(widthHi, p.x);
				}
			}

			for (const BeddingSpan& span : beddingSpans(beams, index, slabTop, widthLo, widthHi))
			{
				for (BeddingPrism bedding :
					 groundBeamBedding(prism, lowPerimeter, highPerimeter, span.top,
									   command.leanConcreteClass, command.gravelClass))
				{
					bedding.start = span.start;
					bedding.depth = span.depth;
					appendBedding(result[index], std::move(bedding));
				}
			}
		}
		return result;
	}

	std::vector<FoundationSolid> foundationSolids(const FoundationCommand& command)
	{
		std::vector<FoundationSolid> solids;

		// 底盤のコンクリートと、その下の砕石（kSlabBeddingThickness）。
		for (const FoundationSlab& slab : command.slabs)
		{
			if (slab.boundary.size() < 3 || slab.thickness <= 0.0)
				continue;
			const auto vertical = [&slab](double z, double thickness, FoundationSolid::Kind kind,
										  const std::string& drawClass)
			{
				FoundationSolid solid;
				solid.kind = kind;
				solid.drawClass = drawClass;
				solid.base.reserve(slab.boundary.size());
				for (const Vec2& p : slab.boundary)
					solid.base.push_back(Vec3{p.x, p.y, z});
				solid.extent = Vec3{0.0, 0.0, thickness};
				return solid;
			};
			const double bottom = slab.top - slab.thickness;
			solids.push_back(
				vertical(bottom, slab.thickness, FoundationSolid::Kind::Slab, command.slabClass));
			solids.push_back(vertical(bottom - kSlabBeddingThickness, kSlabBeddingThickness,
									  FoundationSolid::Kind::Bedding, command.gravelClass));
		}

		// 立上り（壁芯の両側へ半幅の矩形を、下端から天端まで）。
		for (const FoundationRiser& riser : command.risers)
		{
			const std::vector<Vec2> footprint = riserFootprint(riser);
			if (footprint.empty() || riser.top - riser.bottom <= 0.0)
				continue;
			FoundationSolid solid;
			solid.kind = FoundationSolid::Kind::Riser;
			solid.drawClass = command.riserClass;
			for (const Vec2& p : footprint)
				solid.base.push_back(Vec3{p.x, p.y, riser.bottom});
			solid.extent = Vec3{0.0, 0.0, riser.top - riser.bottom};
			solids.push_back(std::move(solid));
		}

		// 地中梁（天端を底盤へ呑み込ませる）。
		for (const FoundationBeam& beam : command.beams)
		{
			const BeamPrism prism = beamPrism(beam);
			if (prism.profile.size() < 3 || prism.depth <= 0.0 || beam.depth <= 0.0)
				continue;
			solids.push_back(solidFromPrism(raiseBeamPrismTop(prism, kGroundBeamSlabBite),
											FoundationSolid::Kind::Beam, command.slabClass));
		}

		// 床付け（地中梁と押し出しの向きを共有し、断面と区間だけが違う）。
		const std::vector<std::vector<BeddingPrism>> beddings = foundationBeddings(command);
		for (std::size_t index = 0; index < command.beams.size(); ++index)
		{
			const BeamPrism prism = beamPrism(command.beams[index]);
			Vec2 axis;
			Vec2 width;
			beamPrismAxes(prism, axis, width);
			for (const BeddingPrism& bedding : beddings[index])
			{
				if (bedding.profile.size() < 3 || bedding.depth <= 0.0)
					continue;
				BeamPrism piece = prism;
				piece.profile = bedding.profile;
				piece.origin.x += axis.x * bedding.start;
				piece.origin.y += axis.y * bedding.start;
				piece.depth = bedding.depth;
				solids.push_back(
					solidFromPrism(piece, FoundationSolid::Kind::Bedding, bedding.drawClass));
			}
		}
		return solids;
	}

	std::vector<FoundationPlanShape> foundationPlanShapes(const FoundationCommand& command)
	{
		std::vector<FoundationPlanShape> shapes;
		for (const FoundationSlab& slab : command.slabs)
		{
			if (slab.boundary.size() < 3)
				continue;
			shapes.push_back(
				FoundationPlanShape{FoundationSolid::Kind::Slab, command.slabClass, slab.boundary});
		}
		for (const FoundationRiser& riser : command.risers)
		{
			std::vector<Vec2> footprint = riserFootprint(riser);
			if (footprint.empty())
				continue;
			shapes.push_back(FoundationPlanShape{FoundationSolid::Kind::Riser, command.riserClass,
												 std::move(footprint)});
		}
		for (const FoundationBeam& beam : command.beams)
		{
			// 天端が最も広いので、断面の u 範囲の掃引＝天端幅の矩形になる。
			std::vector<Vec2> footprint = beamPrismFootprint(beamPrism(beam));
			if (footprint.empty())
				continue;
			shapes.push_back(FoundationPlanShape{FoundationSolid::Kind::Beam, command.slabClass,
												 std::move(footprint)});
		}
		return shapes;
	}

	// --- 直列化 ----------------------------------------------------------------------

	std::string encodeFoundation(const FoundationCommand& command)
	{
		std::string out(kEncodingMagic);
		out += ";C ";
		out += command.slabClass;
		out += '|';
		out += command.riserClass;
		out += '|';
		out += command.leanConcreteClass;
		out += '|';
		out += command.gravelClass;
		out += ";P";
		const FoundationParams& p = command.params;
		for (const double value : {p.slabTop, p.slabThickness, p.riserWidth, p.riserTop,
								   p.beamDepth, p.haunchWidth, p.haunchHeight})
			appendNumber(out, value);
		for (const FoundationSlab& slab : command.slabs)
		{
			out += ";S";
			appendNumber(out, slab.top);
			appendNumber(out, slab.thickness);
			appendNumber(out, static_cast<double>(slab.boundary.size()));
			for (const Vec2& point : slab.boundary)
				appendPoint(out, point);
		}
		for (const FoundationRiser& riser : command.risers)
		{
			out += ";R";
			appendPoint(out, riser.start);
			appendPoint(out, riser.end);
			appendNumber(out, riser.width);
			appendNumber(out, riser.bottom);
			appendNumber(out, riser.top);
		}
		for (const FoundationBeam& beam : command.beams)
		{
			out += ";B";
			appendPoint(out, beam.start);
			appendPoint(out, beam.end);
			appendNumber(out, beam.bottomWidth);
			appendNumber(out, beam.haunchLeft);
			appendNumber(out, beam.haunchRight);
			appendNumber(out, beam.haunchHeight);
			appendNumber(out, beam.top);
			appendNumber(out, beam.depth);
		}
		out += ';';
		return out;
	}

	bool decodeFoundation(const std::string& text, FoundationCommand& out)
	{
		// ";" で項目に分ける。先頭は版の印。
		std::vector<std::string_view> items;
		std::string_view rest(text);
		while (!rest.empty())
		{
			const std::size_t cut = rest.find(';');
			items.push_back(rest.substr(0, cut));
			if (cut == std::string_view::npos)
				break;
			rest.remove_prefix(cut + 1);
		}
		if (items.empty() || items.front() != kEncodingMagic)
			return false;

		FoundationCommand command;
		bool classesSeen = false;
		bool paramsSeen = false;
		for (std::size_t index = 1; index < items.size(); ++index)
		{
			const std::string_view item = items[index];
			if (item.empty())
				continue;
			const char code = item.front();
			const std::string_view body = item.substr(1);
			if (code == 'C')
			{
				// "C slab|riser|lean|gravel"
				if (body.empty() || body.front() != ' ')
					return false;
				std::vector<std::string> classes;
				std::string_view names = body.substr(1);
				while (true)
				{
					const std::size_t cut = names.find('|');
					classes.emplace_back(names.substr(0, cut));
					if (cut == std::string_view::npos)
						break;
					names.remove_prefix(cut + 1);
				}
				if (classes.size() != 4)
					return false;
				command.slabClass = classes[0];
				command.riserClass = classes[1];
				command.leanConcreteClass = classes[2];
				command.gravelClass = classes[3];
				classesSeen = true;
				continue;
			}

			NumberReader reader(body);
			if (code == 'P')
			{
				FoundationParams& p = command.params;
				if (!reader.next(p.slabTop) || !reader.next(p.slabThickness) ||
					!reader.next(p.riserWidth) || !reader.next(p.riserTop) ||
					!reader.next(p.beamDepth) || !reader.next(p.haunchWidth) ||
					!reader.next(p.haunchHeight) || !reader.done())
					return false;
				paramsSeen = true;
			}
			else if (code == 'S')
			{
				FoundationSlab slab;
				double count = 0.0;
				if (!reader.next(slab.top) || !reader.next(slab.thickness) || !reader.next(count) ||
					count < 0.0 || count > 1e6 || count != std::floor(count))
					return false;
				const auto vertices = static_cast<std::size_t>(count);
				for (std::size_t i = 0; i < vertices; ++i)
				{
					Vec2 point;
					if (!reader.nextPoint(point))
						return false;
					slab.boundary.push_back(point);
				}
				if (!reader.done())
					return false;
				command.slabs.push_back(std::move(slab));
			}
			else if (code == 'R')
			{
				FoundationRiser riser;
				if (!reader.nextPoint(riser.start) || !reader.nextPoint(riser.end) ||
					!reader.next(riser.width) || !reader.next(riser.bottom) ||
					!reader.next(riser.top) || !reader.done())
					return false;
				command.risers.push_back(riser);
			}
			else if (code == 'B')
			{
				FoundationBeam beam;
				if (!reader.nextPoint(beam.start) || !reader.nextPoint(beam.end) ||
					!reader.next(beam.bottomWidth) || !reader.next(beam.haunchLeft) ||
					!reader.next(beam.haunchRight) || !reader.next(beam.haunchHeight) ||
					!reader.next(beam.top) || !reader.next(beam.depth) || !reader.done())
					return false;
				command.beams.push_back(beam);
			}
			else
			{
				return false; // 知らない項目＝別の版
			}
		}
		if (!classesSeen || !paramsSeen)
			return false;
		out = std::move(command);
		return true;
	}
} // namespace HomeskzIfcImport::core
