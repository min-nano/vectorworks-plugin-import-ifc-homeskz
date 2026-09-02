//
//	core/Foundation.cpp
//
//	基礎の命令（FoundationCommand）から押し出しソリッド群を組み立てる純計算と、PIO の
//	レコードへ保存する直列化の実装。意図は core/Foundation.h を参照。
//	【SDK 非依存】core/ は VectorWorks SDK を一切 include しない。
//
//	【外形多角形からソリッドへ】部品はどれも「平面の外形＋高さ」なので、ソリッドは 2 通りしか
//	要らない:
//	  * **鉛直の押し出し** … 外形をそのまま z の範囲だけ持ち上げる（底盤・立上り・地中梁の
//	    鉛直部・床付けの平らな層）
//	  * **辺に沿う押し出し** … 辺に垂直な鉛直断面を辺の方向へ掃く（地中梁の斜め部と、その
//	    側面を覆う砕石の帯）
//	どちらも「3D 多角形＋押し出しベクトル」で表せるので、描画側は 1 種類の呼び出し
//	（draw/DrawUtil の CreateExtrudedSolid）で描ける。
//
//	【斜め部を辺ごとに作る】斜め部（ハンチ）は外形が高さとともに広がる形で、押し出しでは
//	表せない。そこで**辺ごとに三角形断面のくさびを 1 つずつ**作り、隣り合うくさびは
//	マイター（外側へ広げた外形の頂点＝offsetPolygon の頂点）まで伸ばして角の隙間を埋める。
//	角では下の方が少し詰まる（本来はそこで細るはずが、天端と同じ幅で埋まる）が、**天端の
//	外形からはみ出さない**ので平面には影響しない。コンクリートの入隅が詰まるのは実物でも
//	同じなので、これを approximation として受け入れる（docs/DEV-NOTES.md M21）。
//
//	床付け（捨てコン・砕石）の考え方は M17（docs/DEV-NOTES.md「基礎の床付け」）から引き継ぐ:
//	地中梁の下に捨てコン 30 + 砕石 100、外周部の辺では横へ 50 だけ張り出して終わり、
//	外周でない辺では側面を法線方向に 130 の砕石で覆う（底盤の砕石の底までで打ち切る）。
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

		// 外形の長さ・高さが「ある」とみなす下限（mm）。
		constexpr double kSolidEps = 1e-6;

		// 床付けの断面を組み立てるときの許容値（天端／下端の辺とみなす v の差。mm）。
		constexpr double kBeddingLevelTol = 0.5;
		constexpr double kBeddingEdgeEps = 1e-6;

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

		// 平面外形の代表点（重心・各頂点・各辺の中点）。どの底盤の上／下かを数えるのに使う。
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

		// 許容値で丸めた整数キー（代表値の集計の鍵）。
		long long roundKey(double value, double tolerance)
		{
			return std::llround(value / tolerance);
		}

		// 反時計回りに揃えた外形（外向き法線＝進行方向の右になる）。3 点未満なら空。
		std::vector<Vec2> orientCcw(const std::vector<Vec2>& outline)
		{
			if (outline.size() < 3)
				return {};
			if (shoelaceSigned(outline) >= 0.0)
				return outline;
			return {outline.rbegin(), outline.rend()};
		}

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

		bool inAnyPolygon(const Vec2& point, const PolygonList& polys)
		{
			return std::ranges::any_of(polys, [&point](const std::vector<Vec2>& poly)
									   { return pointInPolygon(point, poly); });
		}

		// --- 外形からソリッドへ ---------------------------------------------------

		// 鉛直の押し出し（外形を zLow から zHigh まで）。高さが無ければ base を空にして返す
		// （呼び出し側は空を捨てる）。
		FoundationSolid verticalSolid(FoundationSolid::Kind kind, const std::string& drawClass,
									  const std::vector<Vec2>& outline, double zLow, double zHigh)
		{
			FoundationSolid solid;
			solid.kind = kind;
			solid.drawClass = drawClass;
			if (outline.size() < 3 || zHigh - zLow <= kSolidEps)
				return solid;
			solid.base.reserve(outline.size());
			for (const Vec2& p : outline)
				solid.base.push_back(Vec3{p.x, p.y, zLow});
			solid.extent = Vec3{0.0, 0.0, zHigh - zLow};
			return solid;
		}

		// 辺に沿う押し出し。section は辺に垂直な鉛直断面の頂点列で、x＝辺の外向き法線方向の
		// 距離・y＝絶対 Z。断面を from に立て、辺の方向へ to まで掃く。
		FoundationSolid edgeSolid(FoundationSolid::Kind kind, const std::string& drawClass,
								  const Vec2& from, const Vec2& to, const Vec2& normal,
								  const std::vector<Vec2>& section)
		{
			FoundationSolid solid;
			solid.kind = kind;
			solid.drawClass = drawClass;
			const Vec2 span{to.x - from.x, to.y - from.y};
			if (section.size() < 3 || length(span) <= kSolidEps)
				return solid;
			solid.base.reserve(section.size());
			for (const Vec2& p : section)
				solid.base.push_back(
					Vec3{from.x + (normal.x * p.x), from.y + (normal.y * p.x), p.y});
			solid.extent = Vec3{span.x, span.y, 0.0};
			return solid;
		}

		void addSolid(std::vector<FoundationSolid>& out, FoundationSolid solid)
		{
			if (solid.base.size() >= 3)
				out.push_back(std::move(solid));
		}

		// 外形の辺 i の始点・終点・単位方向・外向き単位法線・長さ。CCW 前提。
		struct EdgeFrame
		{
			Vec2 from;
			Vec2 to;
			Vec2 dir;
			Vec2 normal;
			double length = 0.0;
		};

		EdgeFrame edgeFrame(const std::vector<Vec2>& ring, std::size_t index)
		{
			EdgeFrame frame;
			frame.from = ring[index];
			frame.to = ring[(index + 1) % ring.size()];
			const Vec2 span{frame.to.x - frame.from.x, frame.to.y - frame.from.y};
			frame.length = length(span);
			if (frame.length <= kSolidEps)
				return frame;
			frame.dir = Vec2{span.x / frame.length, span.y / frame.length};
			frame.normal = Vec2{frame.dir.y, -frame.dir.x}; // CCW なので右が外
			return frame;
		}

		// 辺 i を外へ広げたときの、辺方向の区間 [t0, t1]（マイター点を辺へ射影したもの）。
		// 凸角では辺より外へ、入隅では内へ寄るが、**必ず辺の全長を覆う**ように広げておく
		// （隙間を作らない。重なりは同素材なので無害）。
		void edgeMiterSpan(const EdgeFrame& frame, const std::vector<Vec2>& offset,
						   std::size_t index, double& t0, double& t1)
		{
			const std::size_t n = offset.size();
			const auto project = [&frame](const Vec2& p)
			{ return ((p.x - frame.from.x) * frame.dir.x) + ((p.y - frame.from.y) * frame.dir.y); };
			t0 = std::min(0.0, project(offset[index]));
			t1 = std::max(frame.length, project(offset[(index + 1) % n]));
		}

		// 地中梁の辺の種類。斜め部・側面の砕石を付けるかと、床付けの張り出し量を決める。
		enum class BeamEdgeKind
		{
			Free, // 内側の自由な面（斜め部を付ける・側面を砕石で覆う）
			Perimeter, // 建物の外周に面する（鉛直のまま・床付けは 50 張り出して終わり）
			Joint, // 他の地中梁と取り合う（相手のコンクリートの中なので何も付けない）
		};

		std::vector<BeamEdgeKind> classifyBeamEdges(const std::vector<Vec2>& ring,
													const PolygonList& slabOutlines,
													const PolygonList& others)
		{
			std::vector<BeamEdgeKind> kinds(ring.size(), BeamEdgeKind::Free);
			for (std::size_t i = 0; i < ring.size(); ++i)
			{
				const EdgeFrame frame = edgeFrame(ring, i);
				if (frame.length <= kSolidEps)
					continue;
				const Vec2 probe{
					((frame.from.x + frame.to.x) / 2.0) + (frame.normal.x * kBeddingOutsideProbe),
					((frame.from.y + frame.to.y) / 2.0) + (frame.normal.y * kBeddingOutsideProbe)};
				if (!slabOutlines.empty() && !inAnyPolygon(probe, slabOutlines))
					kinds[i] = BeamEdgeKind::Perimeter;
				else if (inAnyPolygon(probe, others))
					kinds[i] = BeamEdgeKind::Joint;
			}
			return kinds;
		}

		// --- 代表値の集計 ---------------------------------------------------------

		// 重み付きの最頻値（許容 kFoundationTol で丸めた鍵ごとに重みを足し、最大の鍵の値を
		// 返す。同率なら大きい値）。空なら 0。
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

		// グループの外形の面積合計（代表値の重み）。
		double outlinesArea(const PolygonList& outlines)
		{
			double total = 0.0;
			for (const std::vector<Vec2>& outline : outlines)
				total += polygonArea(outline);
			return total;
		}

		// --- 地中梁の断面の当てはめ -------------------------------------------------

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

		// 外形の列を「外形の数 → 各外形の（頂点数, x y …）」の順で書く。
		void appendOutlines(std::string& out, const PolygonList& outlines)
		{
			appendNumber(out, static_cast<double>(outlines.size()));
			for (const std::vector<Vec2>& outline : outlines)
			{
				appendNumber(out, static_cast<double>(outline.size()));
				for (const Vec2& point : outline)
				{
					appendNumber(out, point.x);
					appendNumber(out, point.y);
				}
			}
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

			// 個数として読める（非負の整数で、常識的な上限に収まる）値か。
			bool nextCount(std::size_t& out)
			{
				double value = 0.0;
				if (!next(value) || value < 0.0 || value > 1e6 || value != std::floor(value))
					return false;
				out = static_cast<std::size_t>(value);
				return true;
			}

			bool nextOutlines(PolygonList& out)
			{
				std::size_t count = 0;
				if (!nextCount(count))
					return false;
				PolygonList outlines;
				outlines.reserve(count);
				for (std::size_t i = 0; i < count; ++i)
				{
					std::size_t vertices = 0;
					if (!nextCount(vertices))
						return false;
					std::vector<Vec2> outline;
					outline.reserve(vertices);
					for (std::size_t v = 0; v < vertices; ++v)
					{
						Vec2 point;
						if (!next(point.x) || !next(point.y))
							return false;
						outline.push_back(point);
					}
					outlines.push_back(std::move(outline));
				}
				out = std::move(outlines);
				return true;
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

		// 版の印。**部品の持ち方を変えたら上げる**（古い版は decodeFoundation が拒む）。
		constexpr std::string_view kEncodingMagic = "HF2";
	} // namespace

	// --- 地中梁のプリズム -------------------------------------------------------------

	void beamPrismAxes(const BeamPrism& prism, Vec2& axis, Vec2& width)
	{
		const double phi = prism.azimuth * std::numbers::pi / 180.0;
		axis = Vec2{std::cos(phi), std::sin(phi)};
		width = Vec2{-axis.y, axis.x}; // 幅軸 u（走る向きを +90 度回した水平単位ベクトル）
	}

	bool fitFoundationBeam(const BeamPrism& prism, FoundationBeamFit& out)
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
		FoundationBeamFit beam;

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

	std::vector<Vec2> beamFitOutline(const FoundationBeamFit& fit)
	{
		const Vec2 span{fit.end.x - fit.start.x, fit.end.y - fit.start.y};
		const double len = length(span);
		if (len <= kSolidEps || fit.bottomWidth <= kSolidEps)
			return {};
		const Vec2 dir{span.x / len, span.y / len};
		const Vec2 side{-dir.y * fit.bottomWidth / 2.0, dir.x * fit.bottomWidth / 2.0};
		// 進行方向の右 → 左の順で回ると反時計回りになる。
		return {Vec2{fit.start.x - side.x, fit.start.y - side.y},
				Vec2{fit.end.x - side.x, fit.end.y - side.y},
				Vec2{fit.end.x + side.x, fit.end.y + side.y},
				Vec2{fit.start.x + side.x, fit.start.y + side.y}};
	}

	// --- 取り合いの高さ ----------------------------------------------------------------

	namespace
	{
		// 外形の下（上）に来る底盤のグループを選ぶ。代表点（重心・頂点・辺の中点）が
		// いちばん多く入る底盤を採り、同数なら重心が近い方（決定的）。accept が false を
		// 返すグループは候補にしない。見つからなければ npos。
		template <typename Accept>
		std::size_t slabGroupFor(const FoundationCommand& command, const std::vector<Vec2>& outline,
								 const Accept& accept)
		{
			const std::vector<Vec2> samples = footprintSamples(outline);
			const Vec2 centre = polygonCentroid(outline);
			std::size_t best = command.slabs.size();
			std::size_t bestHits = 0;
			double bestDistance = std::numeric_limits<double>::max();
			for (std::size_t index = 0; index < command.slabs.size(); ++index)
			{
				const FoundationSlabGroup& slab = command.slabs[index];
				if (!accept(slab))
					continue;
				std::size_t hits = 0;
				double nearest = std::numeric_limits<double>::max();
				for (const std::vector<Vec2>& ring : slab.outlines)
				{
					if (ring.size() < 3)
						continue;
					for (const Vec2& sample : samples)
					{
						if (pointInPolygon(sample, ring))
							++hits;
					}
					nearest = std::min(nearest, distance(centre, polygonCentroid(ring)));
				}
				if (best >= command.slabs.size() || hits > bestHits ||
					(hits == bestHits && nearest < bestDistance))
				{
					best = index;
					bestHits = hits;
					bestDistance = nearest;
				}
			}
			return best;
		}
	} // namespace

	double foundationSlabBottom(const FoundationCommand& command, const std::vector<Vec2>& outline)
	{
		const double fallback = command.params.slabTop - command.params.slabThickness;
		if (command.slabs.empty() || outline.empty())
			return fallback;
		const std::size_t best =
			slabGroupFor(command, outline, [](const FoundationSlabGroup&) { return true; });
		if (best >= command.slabs.size())
			return fallback;
		return command.slabs[best].top - command.slabs[best].thickness;
	}

	bool foundationBeamTop(const FoundationCommand& command, const std::vector<Vec2>& outline,
						   double bottom, double& out)
	{
		if (command.slabs.empty() || outline.empty())
		{
			const double fallback = command.params.slabTop - command.params.slabThickness;
			if (fallback <= bottom)
				return false;
			out = fallback;
			return true;
		}
		const std::size_t best =
			slabGroupFor(command, outline, [bottom](const FoundationSlabGroup& slab)
						 { return slab.top - slab.thickness > bottom; });
		if (best >= command.slabs.size())
			return false;
		out = command.slabs[best].top - command.slabs[best].thickness;
		return true;
	}

	// --- 代表値とその適用 ------------------------------------------------------------

	FoundationParams foundationBaseParams(const FoundationCommand& command)
	{
		std::vector<std::pair<double, double>> slabTops;
		std::vector<std::pair<double, double>> slabThicknesses;
		for (const FoundationSlabGroup& slab : command.slabs)
		{
			const double weight = outlinesArea(slab.outlines);
			if (weight <= 0.0)
				continue;
			slabTops.emplace_back(slab.top, weight);
			slabThicknesses.emplace_back(slab.thickness, weight);
		}

		std::vector<std::pair<double, double>> riserTops;
		for (const FoundationRiserGroup& riser : command.risers)
		{
			const double weight = outlinesArea(riser.outlines);
			if (weight > 0.0)
				riserTops.emplace_back(riser.top, weight);
		}

		std::vector<std::pair<double, double>> beamDepths;
		std::vector<std::pair<double, double>> haunchWidths;
		std::vector<std::pair<double, double>> haunchHeights;
		for (const FoundationBeamGroup& beam : command.beams)
		{
			const double weight = outlinesArea(beam.outlines);
			if (weight <= 0.0)
				continue;
			// せいは「真上の底盤の底面 − 底」＝取り合いで決まる値なので、ここでも同じ計算を通す。
			double depth = 0.0;
			for (const std::vector<Vec2>& outline : beam.outlines)
			{
				double top = 0.0;
				if (foundationBeamTop(command, outline, beam.bottom, top))
					depth = std::max(depth, top - beam.bottom);
			}
			beamDepths.emplace_back(depth, weight);
			if (beam.haunchWidth > 0.0)
				haunchWidths.emplace_back(beam.haunchWidth, weight);
			if (beam.haunchHeight > 0.0)
				haunchHeights.emplace_back(beam.haunchHeight, weight);
		}

		FoundationParams params;
		params.slabTop = weightedMode(slabTops);
		params.slabThickness = weightedMode(slabThicknesses);
		params.riserTop = weightedMode(riserTops);
		params.beamDepth = weightedMode(beamDepths);
		params.haunchWidth = weightedMode(haunchWidths);
		params.haunchHeight = weightedMode(haunchHeights);
		return params;
	}

	FoundationCommand applyFoundationParams(const FoundationCommand& imported,
											const FoundationParams& edited)
	{
		FoundationCommand result = imported;
		result.params = edited;

		const FoundationParams& base = imported.params;
		const double dSlabTop = edited.slabTop - base.slabTop;
		const double dSlabThickness = edited.slabThickness - base.slabThickness;
		const double dSlabBottom = dSlabTop - dSlabThickness; // 底盤の底面の動き
		const double dRiserTop = edited.riserTop - base.riserTop;
		const double dBeamDepth = edited.beamDepth - base.beamDepth;
		const double dHaunchWidth = edited.haunchWidth - base.haunchWidth;
		const double dHaunchHeight = edited.haunchHeight - base.haunchHeight;

		for (FoundationSlabGroup& slab : result.slabs)
		{
			slab.top += dSlabTop;
			slab.thickness = std::max(slab.thickness + dSlabThickness, 0.0);
		}
		for (FoundationRiserGroup& riser : result.risers)
			riser.top += dRiserTop;
		for (FoundationBeamGroup& beam : result.beams)
		{
			// 天端は底盤の底面に追随するので、底を「底盤の動き − せいの増分」だけ動かせば
			// せいが Δせいだけ変わる。
			beam.bottom += dSlabBottom - dBeamDepth;
			beam.haunchWidth = std::max(beam.haunchWidth + dHaunchWidth, 0.0);
			beam.haunchHeight = std::max(beam.haunchHeight + dHaunchHeight, 0.0);
		}
		return result;
	}

	// --- ソリッドの組み立て ------------------------------------------------------------

	std::vector<Vec2> beamTopOutline(const std::vector<Vec2>& outline, double haunchWidth,
									 const PolygonList& slabOutlines, const PolygonList& others)
	{
		std::vector<Vec2> ring = orientCcw(outline);
		if (ring.empty() || haunchWidth <= kSolidEps)
			return ring;
		const std::vector<BeamEdgeKind> kinds = classifyBeamEdges(ring, slabOutlines, others);
		std::vector<double> dists(ring.size(), 0.0);
		for (std::size_t i = 0; i < ring.size(); ++i)
		{
			if (kinds[i] == BeamEdgeKind::Free)
				dists[i] = haunchWidth;
		}
		return offsetPolygon(ring, dists);
	}

	namespace
	{
		// 地中梁 1 枚ぶんの外形と、その所属グループ。斜め部・床付けは外形ごとに組み立てる。
		struct BeamOutline
		{
			std::size_t group = 0;
			std::vector<Vec2> ring;
		};

		std::vector<BeamOutline> collectBeamOutlines(const FoundationCommand& command)
		{
			std::vector<BeamOutline> out;
			for (std::size_t g = 0; g < command.beams.size(); ++g)
			{
				for (const std::vector<Vec2>& outline : command.beams[g].outlines)
				{
					std::vector<Vec2> ring = orientCcw(outline);
					if (!ring.empty())
						out.push_back(BeamOutline{g, std::move(ring)});
				}
			}
			return out;
		}

		// 自分以外の地中梁の外形（取り合いの判定に使う）。
		PolygonList otherBeamRings(const std::vector<BeamOutline>& beams, std::size_t self)
		{
			PolygonList others;
			others.reserve(beams.size());
			for (std::size_t i = 0; i < beams.size(); ++i)
			{
				if (i != self)
					others.push_back(beams[i].ring);
			}
			return others;
		}

		// 底盤の砕石の外形（地中梁のコンクリートを抜いたもの）。抜けない（穴が開く・計算に
		// 失敗した）ときは外形をそのまま返す——押し出しソリッドは穴を表せないので、
		// **抜くのをあきらめて重ねる**（core/PolygonBool.h「穴の扱い」）。
		PolygonList slabGravelOutlines(const std::vector<Vec2>& ring, const PolygonList& beamTops)
		{
			if (beamTops.empty())
				return {ring};
			PolygonList pieces;
			if (!polygonDifference({ring}, beamTops, pieces))
				return {ring};
			for (const std::vector<Vec2>& piece : pieces)
			{
				if (shoelaceSigned(piece) < 0.0)
					return {ring}; // 穴（時計回りのループ）が出た
			}
			return pieces;
		}
	} // namespace

	std::vector<FoundationSolid> foundationSolids(const FoundationCommand& command)
	{
		std::vector<FoundationSolid> solids;

		// 底盤の外形（CCW）。取り合いの高さと、外周部の判定に使う。
		PolygonList slabOutlines;
		for (const FoundationSlabGroup& slab : command.slabs)
		{
			for (const std::vector<Vec2>& outline : slab.outlines)
			{
				std::vector<Vec2> ring = orientCcw(outline);
				if (!ring.empty())
					slabOutlines.push_back(std::move(ring));
			}
		}

		// 底盤のコンクリート。
		for (const FoundationSlabGroup& slab : command.slabs)
		{
			for (const std::vector<Vec2>& outline : slab.outlines)
			{
				const std::vector<Vec2> ring = orientCcw(outline);
				if (ring.empty())
					continue;
				addSolid(solids, verticalSolid(FoundationSolid::Kind::Slab, command.slabClass, ring,
											   slab.top - slab.thickness, slab.top));
			}
		}

		const std::vector<BeamOutline> beams = collectBeamOutlines(command);

		// 地中梁の天端の外形（斜め部で広がった形）と、その梁が取り合う底盤の底面。
		// 底盤の砕石を抜くのに使う。
		PolygonList beamTops;
		std::vector<double> beamSlabBottoms;
		beamTops.reserve(beams.size());
		beamSlabBottoms.reserve(beams.size());
		for (std::size_t index = 0; index < beams.size(); ++index)
		{
			beamTops.push_back(beamTopOutline(beams[index].ring,
											  command.beams[beams[index].group].haunchWidth,
											  slabOutlines, otherBeamRings(beams, index)));
			double slabBottom = 0.0;
			if (!foundationBeamTop(command, beams[index].ring,
								   command.beams[beams[index].group].bottom, slabBottom))
				slabBottom = std::numeric_limits<double>::quiet_NaN(); // 描かれない地中梁
			beamSlabBottoms.push_back(slabBottom);
		}

		// 底盤の下の砕石（地中梁のコンクリートと重ならないよう抜く）。抜く相手は**その底盤に
		// 取り合う地中梁だけ**——高さの違う底盤が混在する図面で、隣の底盤にぶら下がる梁まで
		// 抜くと砕石に穴が空く。
		for (const FoundationSlabGroup& slab : command.slabs)
		{
			const double bottom = slab.top - slab.thickness;
			PolygonList clip;
			for (std::size_t index = 0; index < beamTops.size(); ++index)
			{
				if (std::abs(beamSlabBottoms[index] - bottom) <= kFoundationTol)
					clip.push_back(beamTops[index]);
			}
			for (const std::vector<Vec2>& outline : slab.outlines)
			{
				const std::vector<Vec2> ring = orientCcw(outline);
				if (ring.empty())
					continue;
				for (const std::vector<Vec2>& piece : slabGravelOutlines(ring, clip))
					addSolid(solids,
							 verticalSolid(FoundationSolid::Kind::Bedding, command.gravelClass,
										   piece, bottom - kSlabBeddingThickness, bottom));
			}
		}

		// 立上り（天端の面を、真下の底盤の底面まで下ろす）。
		for (const FoundationRiserGroup& riser : command.risers)
		{
			for (const std::vector<Vec2>& outline : riser.outlines)
			{
				const std::vector<Vec2> ring = orientCcw(outline);
				if (ring.empty())
					continue;
				addSolid(solids,
						 verticalSolid(FoundationSolid::Kind::Riser, command.riserClass, ring,
									   foundationSlabBottom(command, ring), riser.top));
			}
		}

		// 地中梁（本体 → 斜め部 → 床付け）。
		for (std::size_t index = 0; index < beams.size(); ++index)
		{
			const std::vector<Vec2>& ring = beams[index].ring;
			const FoundationBeamGroup& group = command.beams[beams[index].group];
			// 天端は**上に来る**底盤の底面。呑み込ませて境界線が不安定に出るのを防ぐ。
			double slabBottom = 0.0;
			if (!foundationBeamTop(command, ring, group.bottom, slabBottom))
				continue; // 上に底盤が無い（せいが決まらない）地中梁は描かない
			const double top = slabBottom + kGroundBeamSlabBite;
			const double height = top - group.bottom;
			const double haunchHeight = std::clamp(group.haunchHeight, 0.0, height);
			const double haunchWidth = std::max(group.haunchWidth, 0.0);
			const double bodyTop = top - haunchHeight;
			const std::vector<BeamEdgeKind> kinds =
				classifyBeamEdges(ring, slabOutlines, otherBeamRings(beams, index));

			addSolid(solids, verticalSolid(FoundationSolid::Kind::Beam, command.slabClass, ring,
										   group.bottom, bodyTop));

			// 斜め部（辺ごとの三角形断面のくさび。マイターで角を埋める）。
			if (haunchWidth > kSolidEps && haunchHeight > kSolidEps)
			{
				std::vector<double> dists(ring.size(), 0.0);
				for (std::size_t i = 0; i < ring.size(); ++i)
				{
					if (kinds[i] == BeamEdgeKind::Free)
						dists[i] = haunchWidth;
				}
				const std::vector<Vec2> offset = offsetPolygon(ring, dists);
				for (std::size_t i = 0; i < ring.size(); ++i)
				{
					if (kinds[i] != BeamEdgeKind::Free)
						continue;
					const EdgeFrame frame = edgeFrame(ring, i);
					if (frame.length <= kSolidEps)
						continue;
					double t0 = 0.0;
					double t1 = 0.0;
					edgeMiterSpan(frame, offset, i, t0, t1);
					const Vec2 from{frame.from.x + (frame.dir.x * t0),
									frame.from.y + (frame.dir.y * t0)};
					const Vec2 to{frame.from.x + (frame.dir.x * t1),
								  frame.from.y + (frame.dir.y * t1)};
					const std::vector<Vec2> section{Vec2{0.0, bodyTop}, Vec2{0.0, top},
													Vec2{haunchWidth, top}};
					addSolid(solids, edgeSolid(FoundationSolid::Kind::Beam, command.slabClass, from,
											   to, frame.normal, section));
				}
			}

			// 床付け: 下は捨てコン 30 + 砕石 100。外周部の辺は 50 張り出して終わり、
			// それ以外の辺は側面の帯（130）へ続くので同じだけ広げておく。
			std::vector<double> beddingDists(ring.size(), kSlabBeddingThickness);
			for (std::size_t i = 0; i < ring.size(); ++i)
			{
				if (kinds[i] == BeamEdgeKind::Perimeter)
					beddingDists[i] = kBeddingPerimeterMargin;
			}
			const std::vector<Vec2> beddingRing = offsetPolygon(ring, beddingDists);
			addSolid(solids,
					 verticalSolid(FoundationSolid::Kind::Bedding, command.leanConcreteClass,
								   beddingRing, group.bottom - kSlabLeanConcreteThickness,
								   group.bottom));
			addSolid(solids, verticalSolid(FoundationSolid::Kind::Bedding, command.gravelClass,
										   beddingRing, group.bottom - kSlabBeddingThickness,
										   group.bottom - kSlabLeanConcreteThickness));

			// 側面の砕石の帯（外周部でも取り合いでもない辺だけ）。底盤の砕石の底で打ち切る
			// ——そこから上は底盤の砕石が受け持つ。
			const double gravelTop = slabBottom - kSlabBeddingThickness;
			std::vector<double> bandDists(ring.size(), 0.0);
			for (std::size_t i = 0; i < ring.size(); ++i)
			{
				if (kinds[i] == BeamEdgeKind::Free)
					bandDists[i] = kSlabBeddingThickness;
			}
			const std::vector<Vec2> bandRing = offsetPolygon(ring, bandDists);
			for (std::size_t i = 0; i < ring.size(); ++i)
			{
				if (kinds[i] != BeamEdgeKind::Free)
					continue;
				const EdgeFrame frame = edgeFrame(ring, i);
				if (frame.length <= kSolidEps)
					continue;
				double t0 = 0.0;
				double t1 = 0.0;
				edgeMiterSpan(frame, bandRing, i, t0, t1);
				const Vec2 from{frame.from.x + (frame.dir.x * t0),
								frame.from.y + (frame.dir.y * t0)};
				const Vec2 to{frame.from.x + (frame.dir.x * t1), frame.from.y + (frame.dir.y * t1)};

				// 鉛直部を覆う帯。
				const double vertTop = std::min(bodyTop, gravelTop);
				if (vertTop > group.bottom + kSolidEps)
				{
					const std::vector<Vec2> section{
						Vec2{0.0, group.bottom}, Vec2{kSlabBeddingThickness, group.bottom},
						Vec2{kSlabBeddingThickness, vertTop}, Vec2{0.0, vertTop}};
					addSolid(solids, edgeSolid(FoundationSolid::Kind::Bedding, command.gravelClass,
											   from, to, frame.normal, section));
				}

				// 斜め部を覆う帯（斜面の法線方向に 130）。
				if (haunchWidth > kSolidEps && haunchHeight > kSolidEps && gravelTop > bodyTop)
				{
					const double slope = std::hypot(haunchWidth, haunchHeight);
					const Vec2 normal{kSlabBeddingThickness * haunchHeight / slope,
									  -kSlabBeddingThickness * haunchWidth / slope};
					std::vector<Vec2> section{Vec2{0.0, bodyTop}, Vec2{haunchWidth, top},
											  Vec2{haunchWidth + normal.x, top + normal.y},
											  Vec2{normal.x, bodyTop + normal.y}};
					section =
						clipPolygonToRect(section, Vec2{-1.0e9, -1.0e9}, Vec2{1.0e9, gravelTop});
					addSolid(solids, edgeSolid(FoundationSolid::Kind::Bedding, command.gravelClass,
											   from, to, frame.normal, section));
				}
			}
		}
		return solids;
	}

	std::vector<FoundationPlanShape> foundationPlanShapes(const FoundationCommand& command)
	{
		std::vector<FoundationPlanShape> shapes;
		PolygonList slabOutlines;
		for (const FoundationSlabGroup& slab : command.slabs)
		{
			for (const std::vector<Vec2>& outline : slab.outlines)
			{
				std::vector<Vec2> ring = orientCcw(outline);
				if (ring.empty())
					continue;
				slabOutlines.push_back(ring);
				shapes.push_back(FoundationPlanShape{FoundationSolid::Kind::Slab, command.slabClass,
													 std::move(ring)});
			}
		}
		for (const FoundationRiserGroup& riser : command.risers)
		{
			for (const std::vector<Vec2>& outline : riser.outlines)
			{
				std::vector<Vec2> ring = orientCcw(outline);
				if (ring.empty())
					continue;
				shapes.push_back(FoundationPlanShape{FoundationSolid::Kind::Riser,
													 command.riserClass, std::move(ring)});
			}
		}
		const std::vector<BeamOutline> beams = collectBeamOutlines(command);
		for (std::size_t index = 0; index < beams.size(); ++index)
		{
			// 天端が最も広いので、平面には斜め部で広がった外形を描く。
			std::vector<Vec2> ring =
				beamTopOutline(beams[index].ring, command.beams[beams[index].group].haunchWidth,
							   slabOutlines, otherBeamRings(beams, index));
			if (ring.size() < 3)
				continue;
			shapes.push_back(FoundationPlanShape{FoundationSolid::Kind::Beam, command.slabClass,
												 std::move(ring)});
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
		for (const double value :
			 {p.slabTop, p.slabThickness, p.riserTop, p.beamDepth, p.haunchWidth, p.haunchHeight})
			appendNumber(out, value);
		for (const FoundationSlabGroup& slab : command.slabs)
		{
			out += ";S";
			appendNumber(out, slab.top);
			appendNumber(out, slab.thickness);
			appendOutlines(out, slab.outlines);
		}
		for (const FoundationRiserGroup& riser : command.risers)
		{
			out += ";R";
			appendNumber(out, riser.top);
			appendOutlines(out, riser.outlines);
		}
		for (const FoundationBeamGroup& beam : command.beams)
		{
			out += ";B";
			appendNumber(out, beam.bottom);
			appendNumber(out, beam.haunchWidth);
			appendNumber(out, beam.haunchHeight);
			appendOutlines(out, beam.outlines);
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
					!reader.next(p.riserTop) || !reader.next(p.beamDepth) ||
					!reader.next(p.haunchWidth) || !reader.next(p.haunchHeight) || !reader.done())
					return false;
				paramsSeen = true;
			}
			else if (code == 'S')
			{
				FoundationSlabGroup slab;
				if (!reader.next(slab.top) || !reader.next(slab.thickness) ||
					!reader.nextOutlines(slab.outlines) || !reader.done())
					return false;
				command.slabs.push_back(std::move(slab));
			}
			else if (code == 'R')
			{
				FoundationRiserGroup riser;
				if (!reader.next(riser.top) || !reader.nextOutlines(riser.outlines) ||
					!reader.done())
					return false;
				command.risers.push_back(std::move(riser));
			}
			else if (code == 'B')
			{
				FoundationBeamGroup beam;
				if (!reader.next(beam.bottom) || !reader.next(beam.haunchWidth) ||
					!reader.next(beam.haunchHeight) || !reader.nextOutlines(beam.outlines) ||
					!reader.done())
					return false;
				command.beams.push_back(std::move(beam));
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
