//
//	parse/Footing.cpp
//
//	基礎解析の実装（基礎ストーリ・立上り・底盤・地中梁の部品と、その統合・外面合わせ、
//	そして 1 つの基礎命令への組み立て）。【SDK 非依存】ここでは VectorWorks SDK を include
//	しない（core/parse のみ依存）。
//
//	M10 で人通口（立上りの分割・切り下げ）・地中梁を足し、M20 で壁結合・端部のキャップ・
//	床付けの計算を落とした（基礎は 1 つの PIO になり、床付けは PIO が描くときに
//	core/Foundation が組み立てる。parse/Footing.h 冒頭）。
//	**配筋は保留**（足すときは wallSectionKey / slabMergeKey にも足す。理由は各キーの doc
//	コメント）。
//

#include "parse/Footing.h"
#include "core/UnionFind.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
#include "parse/IfcGeometry.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::BeamPrism;
	using core::ColumnCommand;
	using core::LevelCommand;
	using core::StoryCommand;
	using core::Vec2;

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

		// 多角形の面積（絶対値）。符号付き面積の絶対値（同じループを 2 つ持たない）。
		double shoelaceArea(const std::vector<Vec2>& pts)
		{
			return std::abs(shoelaceSigned(pts));
		}

		// 許容値で丸めた整数キー（グループ化の鍵）。std::llround は 0.5 を 0 から離れる向きへ
		// 丸める（実座標がちょうど半端に乗ることは無いので、境界の丸め方は結果に影響しない）。
		long long roundKey(double value, double tolerance)
		{
			return std::llround(value / tolerance);
		}

		// --- 立上り（壁）------------------------------------------------------------

		// 立上りの断面形状（統合可否）を表すキー。壁厚・下端・天端がすべて一致する立上り
		// 同士だけを統合対象にする。
		// **配筋（M10）を足すときはこのキーにも足す**（配筋の違う立上りを 1 本へ統合すると片
		// 方の配筋が失われるため）。
		using WallKey = std::tuple<long long, long long, long long>;

		WallKey wallSectionKey(const RiserPiece& wall)
		{
			return WallKey{roundKey(wall.thickness, 1e-3), roundKey(wall.bottom, kWallMergeDistTol),
						   roundKey(wall.top, kWallMergeDistTol)};
		}

		// 立上り b が a と**同一直線上**（平行かつ壁芯が同じ線）にあるか。区間の重なりは
		// 問わない（離れた延長線上の立上りも true）。true のとき、b を a の壁芯方向へ射影した
		// 区間 [outLo, outHi] を返す（a の始点が 0・終点が壁芯長）。
		//
		// **統合（重なり／接触が要る）と、自由端の延長制限（離れた隣も要る）が同じ「同一直線
		// 判定」を共有する**ので、条件はここに 1 つだけ置く。
		bool wallsOnSameLine(const RiserPiece& a, const RiserPiece& b, double& outLo, double& outHi)
		{
			const Vec2 da = a.end - a.start;
			const Vec2 db = b.end - b.start;
			const double la = core::length(da);
			const double lb = core::length(db);
			if (la <= 0.0 || lb <= 0.0)
				return false;
			const double ux = da.x / la;
			const double uy = da.y / la;
			// (1) 単位方向ベクトルの外積（= sin 角）で平行判定
			if (std::abs((ux * (db.y / lb)) - (uy * (db.x / lb))) > kWallMergeAngleTol)
				return false;
			// (2) b の始点の a 直線からの直交距離（平行なら b の全点が同距離）
			if (std::abs((ux * (b.start.y - a.start.y)) - (uy * (b.start.x - a.start.x))) >
				kWallMergeDistTol)
				return false;
			// (3) b を a 方向へ射影した区間
			const double t1 = (ux * (b.start.x - a.start.x)) + (uy * (b.start.y - a.start.y));
			const double t2 = (ux * (b.end.x - a.start.x)) + (uy * (b.end.y - a.start.y));
			outLo = std::min(t1, t2);
			outHi = std::max(t1, t2);
			return true;
		}

		// 立上り a・b が同一直線上にあり、区間が重なる／接触するか。同一直線判定は
		// wallsOnSameLine と共有する。
		bool wallsConnectedCollinear(const RiserPiece& a, const RiserPiece& b)
		{
			double lo = 0.0;
			double hi = 0.0;
			if (!wallsOnSameLine(a, b, lo, hi))
				return false;
			const double la = core::distance(a.start, a.end);
			return hi >= -kWallMergeDistTol && lo <= la + kWallMergeDistTol;
		}

		// 同一断面の立上り群のうち、同一直線上で連続するものを 1 本に統合する。連結成分の
		// 骨格は core/UnionFind の connectedComponents（代表＝最小インデックス・出力は代表
		// 昇順＝列挙順に依存しない決定的な並び。決定性の担保も同所）。
		std::vector<RiserPiece> mergeWallGroup(const std::vector<RiserPiece>& walls)
		{
			const std::vector<std::vector<std::size_t>> components =
				core::connectedComponents(walls.size(), [&walls](std::size_t i, std::size_t j)
										  { return wallsConnectedCollinear(walls[i], walls[j]); });

			std::vector<RiserPiece> merged;
			merged.reserve(components.size());
			for (const std::vector<std::size_t>& indices : components)
			{
				const RiserPiece& base = walls[indices.front()];
				if (indices.size() == 1)
				{
					merged.push_back(base);
					continue;
				}
				// 先頭の壁芯方向へ全端点を射影し、最小〜最大区間の 1 本にする（core/Geometry
				// の collinearSpan。高さ基準・壁厚・クラスは先頭のものを引き継ぐ）。
				RiserPiece cmd = base;
				core::collinearSpan(walls, indices, cmd.start, cmd.end);
				merged.push_back(cmd);
			}
			return merged;
		}

		// 立上り a・b の壁芯の交点と、その交点が各壁芯の端点か内部かを求める。平行（交点が定
		// まらない）・区間外で交わる立上りは false。
		//
		// **端点許容は相手壁の半壁厚を含める**: ホームズ君 IFC の立上りは直交する相手壁の
		// 外面までモデル化されるため、コーナーでは壁芯どうしの交点が壁の端から半壁厚ほど
		// 離れた位置に来る。固定 1mm の許容だと外周コーナーが「両方とも内部で交わる」と
		// 誤判定され、自由端の判定（延長する／しない）を取り違える。
		bool wallIntersection(const RiserPiece& a, const RiserPiece& b, Vec2& outPoint,
							  bool& outAAtEnd, bool& outBAtEnd)
		{
			const Vec2 r = a.end - a.start;
			const Vec2 s = b.end - b.start;
			const double la = core::length(r);
			const double lb = core::length(s);
			if (la <= 0.0 || lb <= 0.0)
				return false;
			const double rxs = (r.x * s.y) - (r.y * s.x);
			// 平行／同一直線: 交点が定まらない（同一直線は mergeWallCommands が扱う）。
			if (std::abs(rxs) <= kWallMergeAngleTol * la * lb)
				return false;
			const Vec2 q = b.start - a.start;
			const double t = ((q.x * s.y) - (q.y * s.x)) / rxs;
			const double u = ((q.x * r.y) - (q.y * r.x)) / rxs;
			// a の端点許容は相手 b の半壁厚（a は b の外面で終端しうる）、b は a の半壁厚。
			const double fracA = ((b.thickness / 2.0) + kWallEndpointTol) / la;
			const double fracB = ((a.thickness / 2.0) + kWallEndpointTol) / lb;
			if (t < -fracA || t > 1.0 + fracA)
				return false;
			if (u < -fracB || u > 1.0 + fracB)
				return false;
			outPoint = Vec2{a.start.x + (t * r.x), a.start.y + (t * r.y)};
			outAAtEnd = (t <= fracA) || (t >= 1.0 - fracA);
			outBAtEnd = (u <= fracB) || (u >= 1.0 - fracB);
			return true;
		}

		// 立上り a の端点が、同一直線上に並ぶ別の立上り b と**突き合わせ**になっているか
		// （outAtStart＝始点側・outAtEnd＝終点側）。b を a の壁芯方向へ射影した区間が a の端に
		// 届いていれば、その端は「何も無い自由端」ではなく collinear な隣と接している。
		//
		// 【なぜ要るか】交点判定（wallIntersection）は平行な立上りを除外するので、**同一直線上で
		// 端どうしが接する立上り**はどちらの端も自由端に見える。そのまま半壁厚ずつ延長すると
		// 互いに食い込み、実データで 75mm（片側）・150mm（両側）の重なりになっていた（統合
		// できない＝上端／下端の違う隣どうしで顕在化する。docs/DEV-NOTES.md M10）。
		void collinearAbutment(const RiserPiece& a, const RiserPiece& b, bool& outAtStart,
							   bool& outAtEnd)
		{
			outAtStart = false;
			outAtEnd = false;
			double lo = 0.0;
			double hi = 0.0;
			if (!wallsOnSameLine(a, b, lo, hi))
				return; // 平行でない／別の線上
			const double la = core::distance(a.start, a.end);
			if (hi < -kWallMergeDistTol || lo > la + kWallMergeDistTol)
				return; // 同一直線上だが離れている（隙間がある）＝突き合わせではない
			// b が a の端へ届いている（接する／越える）側だけを「突き合わせ」とみなす。
			// b が a の内側に完全に収まっている場合はどちらの端も自由端のまま。
			outAtStart = lo <= kWallMergeDistTol;
			outAtEnd = hi >= la - kWallMergeDistTol;
		}

		// 自由端の終端柱（柱芯）を壁芯上に射影した基準点を返す。(ux, uy) は自由端で壁の外側を
		// 向く単位ベクトル。終端柱が見つからなければ自由端そのものを返す（柱芯 =
		// 自由端とみなす）。
		Vec2 terminalColumnBase(const Vec2& point, double ux, double uy, double half,
								const std::vector<ColumnCommand>& columns)
		{
			double bestT = 0.0;
			bool found = false;
			for (const ColumnCommand& column : columns)
			{
				const double dx = column.position.x - point.x;
				const double dy = column.position.y - point.y;
				const double along = (dx * ux) + (dy * uy); // 外向きの沿軸成分（内側は負）
				const double perp = std::abs((dx * -uy) + (dy * ux)); // 壁芯線からの直交距離
				if (perp > half + kFreeEndColumnPerpTol)
					continue;
				// 自由端の内側（along<0）または端点付近（along≈0）にある柱だけを対象にする。
				if (along > kWallEndpointTol || along < -kFreeEndColumnAlongTol)
					continue;
				if (!found || std::abs(along) < std::abs(bestT))
					bestT = along;
				found = true;
			}
			if (!found)
				return point;
			return Vec2{point.x + (ux * bestT), point.y + (uy * bestT)};
		}

		// --- 人通口（立上りの切り下げ）------------------------------------------------

		// 人通口の区間が乗っている立上りの添字を返す。開口が (1) 壁芯と平行で (2)
		// 壁芯線上（直交距離が許容内）にあり (3) 開口の中点が壁芯区間内にある立上りを探す。
		// 側並び（直交距離が半壁厚ある平行壁）には乗らない。見つからなければ walls.size()。
		std::size_t findOpeningWall(const std::vector<RiserPiece>& walls,
									const WallOpening& opening)
		{
			const Vec2 delta = opening.end - opening.start;
			const double openingLength = core::length(delta);
			if (openingLength <= 0.0)
				return walls.size();
			const double oux = delta.x / openingLength;
			const double ouy = delta.y / openingLength;
			const double mx = (opening.start.x + opening.end.x) / 2.0;
			const double my = (opening.start.y + opening.end.y) / 2.0;

			for (std::size_t i = 0; i < walls.size(); ++i)
			{
				const RiserPiece& wall = walls[i];
				const Vec2 axis = wall.end - wall.start;
				const double length = core::length(axis);
				if (length <= 0.0)
					continue;
				const double ux = axis.x / length;
				const double uy = axis.y / length;
				if (std::abs((ux * ouy) - (uy * oux)) > kWallMergeAngleTol)
					continue;
				const double perp =
					std::abs((ux * (my - wall.start.y)) - (uy * (mx - wall.start.x)));
				if (perp > kOpeningMatchTol)
					continue;
				const double t = (ux * (mx - wall.start.x)) + (uy * (my - wall.start.y));
				if (t < -kOpeningMatchTol || t > length + kOpeningMatchTol)
					continue;
				return i;
			}
			return walls.size();
		}

		// 人通口の区間で立上りを分割／切り下げた列を返す。開口の下端が底盤天端以下なら中間区
		// 間を出さず両側だけを、それより高ければ天端を開口下端へ切り下げた中間区間を挟む。
		// 端部の長さ補正はしない（実寸法のまま）。
		std::vector<RiserPiece> carveWallOpening(const RiserPiece& wall, const WallOpening& opening,
												 double slabTopAbs)
		{
			const Vec2 axis = wall.end - wall.start;
			const double length = core::length(axis);
			if (length <= 0.0)
				return {wall};
			const double ux = axis.x / length;
			const double uy = axis.y / length;
			const double ta =
				(ux * (opening.start.x - wall.start.x)) + (uy * (opening.start.y - wall.start.y));
			const double tb =
				(ux * (opening.end.x - wall.start.x)) + (uy * (opening.end.y - wall.start.y));
			const double o0 = std::max(0.0, std::min(ta, tb));
			const double o1 = std::min(length, std::max(ta, tb));
			if (o1 - o0 <= kOpeningMinSegment)
				return {wall};

			const auto segment = [&](double t0, double t1, double top)
			{
				RiserPiece cmd = wall;
				cmd.start = Vec2{wall.start.x + (ux * t0), wall.start.y + (uy * t0)};
				cmd.end = Vec2{wall.start.x + (ux * t1), wall.start.y + (uy * t1)};
				cmd.top = top;
				return cmd;
			};

			std::vector<RiserPiece> segments;
			if (o0 > kOpeningMinSegment)
				segments.push_back(segment(0.0, o0, wall.top));
			// 開口の下端が底盤天端より高ければ、その区間だけ天端を切り下げた立上りを挟む。
			if (opening.zBottom > slabTopAbs + kWallMergeDistTol)
				segments.push_back(segment(o0, o1, opening.zBottom));
			if (length - o1 > kOpeningMinSegment)
				segments.push_back(segment(o1, length, wall.top));
			return segments;
		}

		// --- 地中梁（底盤のモディファイア）----------------------------------------------

		// 地中梁の押し出し方向（方位角）の水平単位ベクトル（core::beamPrismAxes の axis）。
		Vec2 groundBeamAxisDir(const BeamPrism& prism)
		{
			Vec2 axis;
			Vec2 width;
			core::beamPrismAxes(prism, axis, width);
			return axis;
		}

		// 断面形状（統合可否）を表す正規化キー。頂点を許容値で丸め、巻きを CCW に揃えたうえで
		// 辞書順最小の頂点から始まる回転に正規化する。
		// **頂点の絶対 (u, v) 位置は保つ**ので、軸に対する横位置（u オフセット）の違う地中梁は
		// 別キーになり統合されない。
		using GroundBeamProfileKey = std::vector<std::pair<long long, long long>>;

		GroundBeamProfileKey groundBeamProfileKey(const BeamPrism& modifier)
		{
			std::vector<Vec2> pts;
			pts.reserve(modifier.profile.size());
			for (const Vec2& p : modifier.profile)
			{
				pts.push_back(Vec2{static_cast<double>(roundKey(p.x, kGroundBeamProfileTol)),
								   static_cast<double>(roundKey(p.y, kGroundBeamProfileTol))});
			}
			if (shoelaceSigned(pts) < 0.0)
				std::ranges::reverse(pts);

			GroundBeamProfileKey key;
			key.reserve(pts.size());
			for (const Vec2& p : pts)
				key.emplace_back(static_cast<long long>(p.x), static_cast<long long>(p.y));
			if (key.empty())
				return key;
			// 辞書順最小の頂点から始まる回転へ正規化する（同じ形状・同じ位置なら同じ列になる）。
			const auto start = std::distance(key.begin(), std::ranges::min_element(key));
			std::ranges::rotate(key, key.begin() + start);
			return key;
		}

		// 統合対象を粗くまとめるグループキー（高さ＝下端 z・方位角・断面形状）。
		// 実際に同一軸線上かは modifiersCollinear が判定する。
		using GroundBeamKey = std::tuple<long long, long long, GroundBeamProfileKey>;

		GroundBeamKey groundBeamGroupKey(const BeamPrism& modifier)
		{
			return GroundBeamKey{roundKey(modifier.origin.z, kGroundBeamMergeTol),
								 roundKey(modifier.azimuth, kGroundBeamAzimuthTol),
								 groundBeamProfileKey(modifier)};
		}

		// 地中梁 a・b が同一軸線上（同一高さ）にあり区間が連続するか。(1) 高さが一致、(2)
		// 方向が平行、(3) b の原点が a の軸線上、(4) a の区間 [0, depth] と b の射影区間が重
		// なる／接触する。
		bool modifiersCollinear(const BeamPrism& a, const BeamPrism& b)
		{
			if (std::abs(a.origin.z - b.origin.z) > kGroundBeamMergeTol)
				return false;
			const Vec2 da = groundBeamAxisDir(a);
			const Vec2 db = groundBeamAxisDir(b);
			if (std::abs((da.x * db.y) - (da.y * db.x)) > kGroundBeamMergeAngleTol)
				return false;
			const double dx = b.origin.x - a.origin.x;
			const double dy = b.origin.y - a.origin.y;
			if (std::abs((da.x * dy) - (da.y * dx)) > kGroundBeamMergeTol)
				return false;
			const double tb0 = (da.x * dx) + (da.y * dy);
			const double tb1 = tb0 + (b.depth * ((da.x * db.x) + (da.y * db.y)));
			const double lo = std::min(tb0, tb1);
			const double hi = std::max(tb0, tb1);
			return hi >= -kGroundBeamMergeTol && lo <= a.depth + kGroundBeamMergeTol;
		}

		// 同一軸線上の地中梁群を 1 本の台形プリズムへ統合する。先頭の軸方向へ全端点を射影し、
		// 最小〜最大区間を新しい押し出しにする（断面・方位角・高さは先頭を引き継ぐ）。
		BeamPrism mergeGroundBeamComponent(const std::vector<BeamPrism>& members)
		{
			const BeamPrism& base = members.front();
			const Vec2 axis = groundBeamAxisDir(base);
			double lo = 0.0;
			double hi = 0.0;
			bool first = true;
			for (const BeamPrism& modifier : members)
			{
				const double t0 = (axis.x * (modifier.origin.x - base.origin.x)) +
								  (axis.y * (modifier.origin.y - base.origin.y));
				const Vec2 dir = groundBeamAxisDir(modifier);
				const double t1 = t0 + (modifier.depth * ((axis.x * dir.x) + (axis.y * dir.y)));
				for (const double t : {t0, t1})
				{
					if (first || t < lo)
						lo = t;
					if (first || t > hi)
						hi = t;
					first = false;
				}
			}
			BeamPrism merged = base;
			merged.depth = hi - lo;
			merged.origin = core::Vec3{base.origin.x + (axis.x * lo), base.origin.y + (axis.y * lo),
									   base.origin.z};
			return merged;
		}

		// 同一断面・同一向きの地中梁群のうち、同一軸線上で連続するものを統合する。連結成分の
		// 骨格は core/UnionFind の connectedComponents（代表＝最小添字・出力は代表添字昇順）。
		std::vector<BeamPrism> mergeGroundBeamGroup(const std::vector<BeamPrism>& modifiers)
		{
			const std::vector<std::vector<std::size_t>> components = core::connectedComponents(
				modifiers.size(), [&modifiers](std::size_t i, std::size_t j)
				{ return modifiersCollinear(modifiers[i], modifiers[j]); });

			std::vector<BeamPrism> merged;
			merged.reserve(components.size());
			for (const std::vector<std::size_t>& component : components)
			{
				if (component.size() == 1)
				{
					merged.push_back(modifiers[component.front()]);
					continue;
				}
				std::vector<BeamPrism> members;
				members.reserve(component.size());
				for (const std::size_t i : component)
					members.push_back(modifiers[i]);
				merged.push_back(mergeGroundBeamComponent(members));
			}
			return merged;
		}

		// 地中梁 1 本の押し出しソリッドを台形プリズムのモディファイアにする。押し出し方向の水
		// 平成分から方位角を求め、断面頂点を幅軸 u（走る向きを +90 度回した水平単位ベクトル w）
		// ・鉛直軸 v（ワールド Z の差分）へ取り直す。押し出しが水平でない（鉛直）
		// ソリッドは地中梁でないので false。
		bool groundBeamPrism(const WorldSolid& solid, const Vec2& center, BeamPrism& out)
		{
			const double runLength = std::hypot(solid.extrudeDir.x, solid.extrudeDir.y);
			if (runLength <= 0.0)
				return false;
			const double ux = solid.extrudeDir.x / runLength;
			const double uy = solid.extrudeDir.y / runLength;
			// 幅軸 w＝走る向きを +90 度回した水平単位ベクトル（描画側の復元規約と対で決まる）。
			const double wx = -uy;
			const double wy = ux;

			BeamPrism cmd;
			cmd.profile.reserve(solid.profile.size());
			for (const Vec2& p : solid.profile)
			{
				// プロファイル頂点 (u, v) を配置基底でワールドへ写し、断面原点からの
				// 幅方向（w）成分と鉛直（Z）成分に取り直す。
				const core::Vec3 world = solid.origin + (solid.xAxis * p.x) + (solid.yAxis * p.y);
				cmd.profile.push_back(
					Vec2{((world.x - solid.origin.x) * wx) + ((world.y - solid.origin.y) * wy),
						 world.z - solid.origin.z});
			}
			cmd.depth = solid.depth;
			cmd.origin =
				core::Vec3{solid.origin.x - center.x, solid.origin.y - center.y, solid.origin.z};
			cmd.azimuth = std::atan2(uy, ux) * 180.0 / std::numbers::pi;
			out = std::move(cmd);
			return true;
		}

		// --- 底盤（スラブ）----------------------------------------------------------
		//
		// 多角形の和（union）は「頂点を丸めて厳密比較できるようにし、全辺を交点で細分し、
		// すぐ右（外側）がどの多角形にも入らない有向辺だけを境界として残してつなぐ」
		// という手順で求める。丸めた点をキーにするので、集合・辞書は素直な std::set /
		// std::map（辞書順比較）で足りる。

		// 交点計算で頂点を丸める小数桁（1e-4 mm = 0.1 ミクロン）。
		constexpr double kSlabRoundScale = 1e4;

		// 丸めた平面点。std::set / std::map の鍵にするので、Vec2 ではなく比較可能な pair。
		using Pt2 = std::pair<double, double>;
		using Edge = std::pair<Pt2, Pt2>;

		Pt2 roundPt(double x, double y)
		{
			return Pt2{std::round(x * kSlabRoundScale) / kSlabRoundScale,
					   std::round(y * kSlabRoundScale) / kSlabRoundScale};
		}

		Pt2 roundPt(const Vec2& p)
		{
			return roundPt(p.x, p.y);
		}

		// 境界を丸めた頂点列にし、末尾の閉じ重複・連続する同一点を除く。
		std::vector<Pt2> cleanRing(const std::vector<Vec2>& boundary)
		{
			std::vector<Pt2> out;
			out.reserve(boundary.size());
			for (const Vec2& p : boundary)
			{
				const Pt2 rp = roundPt(p);
				if (out.empty() || out.back() != rp)
					out.push_back(rp);
			}
			if (out.size() > 1 && out.front() == out.back())
				out.pop_back();
			return out;
		}

		double shoelaceSigned(const std::vector<Pt2>& pts)
		{
			double total = 0.0;
			const std::size_t n = pts.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Pt2& a = pts[i];
				const Pt2& b = pts[(i + 1) % n];
				total += (a.first * b.second) - (b.first * a.second);
			}
			return total / 2.0;
		}

		// 点 (x, y) が単純多角形の内部（境界は含めない近似）にあるか。水平レイキャスト（半開
		// ルール）。呼び出し側は辺から法線方向へ kSlabSideEps ずらした点を渡すので、
		// 辺ちょうどの縮退は問題にならない。
		bool pointInPoly(double x, double y, const std::vector<Pt2>& poly)
		{
			bool inside = false;
			const std::size_t n = poly.size();
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
			if (std::abs(denom) > kSlabAngleTol * rLen * sLen)
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
				kSlabMergeTol * rLen)
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
		std::vector<Pt2> simplifyRing(const std::vector<Pt2>& ring)
		{
			std::vector<Pt2> pts = ring;
			if (pts.size() > 1 && pts.front() == pts.back())
				pts.pop_back();
			const std::size_t n = pts.size();
			std::vector<Pt2> out;
			for (std::size_t i = 0; i < n; ++i)
			{
				const Pt2& a = pts[(i + n - 1) % n];
				const Pt2& b = pts[i];
				const Pt2& c = pts[(i + 1) % n];
				const double cross = ((b.first - a.first) * (c.second - b.second)) -
									 ((b.second - a.second) * (c.first - b.first));
				if (std::abs(cross) > kSlabMergeTol)
					out.push_back(b);
			}
			return out;
		}

		// 有向境界辺をつないで閉ループのリストにする。開ループが生じたら false（統合できない
		// 成分として呼び出し側が元のまま残す）。
		bool chainBoundary(const std::vector<Edge>& edges, std::vector<std::vector<Pt2>>& out)
		{
			std::map<Pt2, std::vector<Edge>> fromMap;
			for (const Edge& edge : edges)
				fromMap[edge.first].push_back(edge);
			std::set<Edge> remaining(edges.begin(), edges.end());

			std::vector<std::vector<Pt2>> loops;
			while (!remaining.empty())
			{
				const Edge start = *remaining.begin(); // std::set は辞書順なので決定的
				Edge cur = start;
				std::vector<Pt2> ring{cur.first};
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
				std::vector<Pt2> simplified = simplifyRing(ring);
				if (simplified.size() >= 3)
					loops.push_back(std::move(simplified));
			}
			out = std::move(loops);
			return true;
		}

		// 任意向きの単純多角形群の和（union）の境界ループ。各多角形を CCW（内部が左）に揃え、
		// 全辺を他辺との交点で細分し、細分した有向辺のうち
		// **すぐ右（外側）がどの多角形にも含まれない**ものだけを境界として残してつなぐ
		// （共有辺は両隣の多角形が右側に来て打ち消され、外周辺だけ残る）。開ループなら false。
		bool polygonUnion(const std::vector<std::vector<Pt2>>& polys,
						  std::vector<std::vector<Pt2>>& out)
		{
			std::vector<std::vector<Pt2>> oriented;
			oriented.reserve(polys.size());
			for (const std::vector<Pt2>& poly : polys)
			{
				if (shoelaceSigned(poly) >= 0.0)
				{
					oriented.push_back(poly);
				}
				else
				{
					oriented.emplace_back(poly.rbegin(), poly.rend());
				}
			}

			std::vector<Edge> directed;
			for (const std::vector<Pt2>& poly : oriented)
			{
				const std::size_t n = poly.size();
				for (std::size_t i = 0; i < n; ++i)
					directed.emplace_back(poly[i], poly[(i + 1) % n]);
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
			std::set<Edge> seen;
			for (std::size_t i = 0; i < m; ++i)
			{
				for (const Edge& part : splitEdge(directed[i].first, directed[i].second, cuts[i]))
				{
					if (!seen.insert(part).second)
						continue;
					const double mx = (part.first.first + part.second.first) / 2.0;
					const double my = (part.first.second + part.second.second) / 2.0;
					const double ex = part.second.first - part.first.first;
					const double ey = part.second.second - part.first.second;
					const double length = std::hypot(ex, ey);
					if (length <= 0.0)
						continue;
					// 進行方向 p→q の右向き法線 (ey, −ex)/length。外側へはみ出した点。
					const double rx = mx + (kSlabSideEps * ey / length);
					const double ry = my - (kSlabSideEps * ex / length);
					const bool insideAny =
						std::ranges::any_of(oriented, [rx, ry](const std::vector<Pt2>& poly)
											{ return pointInPoly(rx, ry, poly); });
					if (!insideAny)
						boundary.push_back(part);
				}
			}
			return chainBoundary(boundary, out);
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
			if (std::abs((rx * sy) - (ry * sx)) > kSlabAngleTol * rLen * sLen)
				return 0.0;
			if (std::abs(((c.first - a.first) * ry) - ((c.second - a.second) * rx)) >
				kSlabMergeTol * rLen)
				return 0.0;
			const double tc =
				(((c.first - a.first) * rx) + ((c.second - a.second) * ry)) / (rLen * rLen);
			const double td =
				(((d.first - a.first) * rx) + ((d.second - a.second) * ry)) / (rLen * rLen);
			const double lo = std::max(0.0, std::min(tc, td));
			const double hi = std::min(1.0, std::max(tc, td));
			return (hi > lo) ? (hi - lo) * rLen : 0.0;
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

		// 底盤ポリゴン a・b が連続する（境界を共有 or 面で重なる）か。角（点）だけで接する場
		// 合は連続としない。
		bool polysConnected(const std::vector<Pt2>& a, const std::vector<Pt2>& b)
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
					if (collinearOverlap(a1, a2, b1, b2) > kSlabMergeTol)
						return true;
					// 内部で交差（端点を含まない真の交差）
					for (const Pt2& pt : segSplitPoints(a1, a2, b1, b2))
					{
						const double t = paramOn(a1, a2, pt);
						const double u = paramOn(b1, b2, pt);
						if (t > kSlabAngleTol && t < 1.0 - kSlabAngleTol && u > kSlabAngleTol &&
							u < 1.0 - kSlabAngleTol)
							return true;
					}
				}
			}
			if (std::ranges::any_of(a, [&b](const Pt2& p)
									{ return pointInPoly(p.first, p.second, b); }))
				return true;
			return std::ranges::any_of(b, [&a](const Pt2& p)
									   { return pointInPoly(p.first, p.second, a); });
		}

		// 底盤の統合可否を表すキー。コンクリート厚・天端の高さが一致する底盤同士だけを統合
		// 対象にする。**配筋（M10）を足すときはこのキーにも足す**（配筋の違う底盤を 1 枚へ
		// 統合すると片方が失われるため）。
		using SlabKey = std::tuple<long long, long long>;

		SlabKey slabMergeKey(const SlabPiece& slab)
		{
			return SlabKey{roundKey(slab.thickness, 1e-3), roundKey(slab.elevation, kSlabMergeTol)};
		}

		// 連続する底盤（polysConnected）の連結成分をインデックス集合で返す。成分は昇順・
		// 成分内も昇順で決定的。
		std::vector<std::vector<std::size_t>>
		slabComponents(const std::map<std::size_t, std::vector<Pt2>>& polys)
		{
			std::vector<std::size_t> ids;
			ids.reserve(polys.size());
			for (const auto& entry : polys)
				ids.push_back(entry.first);

			std::map<std::size_t, std::size_t> parent;
			for (const std::size_t id : ids)
				parent[id] = id;
			const auto find = [&parent](std::size_t a)
			{
				while (parent[a] != a)
				{
					parent[a] = parent[parent[a]];
					a = parent[a];
				}
				return a;
			};

			for (std::size_t p = 0; p < ids.size(); ++p)
			{
				for (std::size_t q = p + 1; q < ids.size(); ++q)
				{
					if (!polysConnected(polys.at(ids[p]), polys.at(ids[q])))
						continue;
					const std::size_t ra = find(ids[p]);
					const std::size_t rb = find(ids[q]);
					if (ra != rb)
						parent[std::max(ra, rb)] = std::min(ra, rb);
				}
			}

			std::map<std::size_t, std::vector<std::size_t>> comps;
			for (const std::size_t id : ids)
				comps[find(id)].push_back(id);

			std::vector<std::vector<std::size_t>> result;
			result.reserve(comps.size());
			for (auto& comp : comps)
			{
				std::ranges::sort(comp.second);
				result.push_back(comp.second);
			}
			return result;
		}

		// 底盤の辺 a→b に沿う立上りの半壁厚（該当が無ければ 0）。最も重なりの大きい立上りの半
		// 壁厚を採る。
		double wallHalfThicknessForEdge(const Vec2& a, const Vec2& b,
										const std::vector<RiserPiece>& walls)
		{
			const double ex = b.x - a.x;
			const double ey = b.y - a.y;
			const double length = std::hypot(ex, ey);
			if (length <= kSlabMergeTol)
				return 0.0;
			const double ux = ex / length;
			const double uy = ey / length;

			double best = 0.0;
			double bestOverlap = kSlabMergeTol;
			for (const RiserPiece& wall : walls)
			{
				const double dx = wall.end.x - wall.start.x;
				const double dy = wall.end.y - wall.start.y;
				const double wlen = std::hypot(dx, dy);
				if (wlen <= kSlabMergeTol)
					continue;
				// 壁芯が辺と平行かつ同一直線上（端点の直交距離 ≈ 0）か。
				if (std::abs((ux * dy) - (uy * dx)) / wlen > kSlabAngleTol)
					continue;
				if (std::abs((ux * (wall.start.y - a.y)) - (uy * (wall.start.x - a.x))) >
					kSlabMergeTol)
					continue;
				const double t1 = (ux * (wall.start.x - a.x)) + (uy * (wall.start.y - a.y));
				const double t2 = (ux * (wall.end.x - a.x)) + (uy * (wall.end.y - a.y));
				const double overlap =
					std::min(std::max(t1, t2), length) - std::max(std::min(t1, t2), 0.0);
				if (overlap > bestOverlap)
				{
					bestOverlap = overlap;
					best = wall.thickness / 2.0;
				}
			}
			return best;
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

		// CCW ポリゴンの各辺 i を外向きへ dists[i] だけ移動した頂点列。隣接する移動後の辺（直
		// 線）の交点を新しい頂点にするので、凸角は外側へ伸び、凹角（入隅）は詰まる。
		std::vector<Vec2> offsetPolygon(const std::vector<Vec2>& pts,
										const std::vector<double>& dists)
		{
			const std::size_t n = pts.size();
			std::vector<std::pair<Vec2, Vec2>> lines; // (点, 方向)
			lines.reserve(n);
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& a = pts[i];
				const Vec2& b = pts[(i + 1) % n];
				const double ex = b.x - a.x;
				const double ey = b.y - a.y;
				const double length = std::hypot(ex, ey);
				if (length <= 0.0)
				{
					lines.emplace_back(a, Vec2{1.0, 0.0});
					continue;
				}
				const double ux = ex / length;
				const double uy = ey / length;
				// CCW ポリゴンの外向き法線（進行方向の右）。
				lines.emplace_back(Vec2{a.x + (dists[i] * uy), a.y - (dists[i] * ux)},
								   Vec2{ux, uy});
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
					vertex = Vec2{pts[i].x + (dists[i] * d2.y), pts[i].y - (dists[i] * d2.x)};
				}
				out.push_back(vertex);
			}
			return out;
		}

		// 底盤外形の各辺を、沿っている立上りの外面まで外側へ広げた外形。立上りに沿う辺が
		// 1 つも無ければ false（呼び出し側は元の外形をそのまま使う）。
		bool offsetBoundaryToWalls(const std::vector<Vec2>& boundary,
								   const std::vector<RiserPiece>& walls, std::vector<Vec2>& out)
		{
			std::vector<Vec2> pts = boundary;
			if (pts.size() > 1 && core::samePoint(pts.front(), pts.back()))
				pts.pop_back();
			if (pts.size() < 3)
				return false;
			if (shoelaceSigned(pts) < 0.0)
				std::ranges::reverse(pts);

			const std::size_t n = pts.size();
			std::vector<double> dists;
			dists.reserve(n);
			for (std::size_t i = 0; i < n; ++i)
				dists.push_back(wallHalfThicknessForEdge(pts[i], pts[(i + 1) % n], walls));
			if (std::ranges::none_of(dists, [](double d) { return d > 0.0; }))
				return false;

			out = offsetPolygon(pts, dists);
			return true;
		}

	} // namespace

	// --- 公開 API ------------------------------------------------------------------

	bool isFoundationWall(const std::string& name)
	{
		return name.starts_with(kFoundationWallPrefix);
	}

	bool isGroundBeam(const std::string& name)
	{
		return name.find(kGroundBeamToken) != std::string::npos;
	}

	bool isBaseSlab(const std::string& name)
	{
		return name.find(kBaseSlabToken) != std::string::npos;
	}

	std::vector<int> collectFootingElements(const Model& model)
	{
		std::vector<int> elements = model.byType("IFCFOOTING");
		for (const int id : model.byType("IFCSLAB"))
		{
			const Entity* element = model.entity(id);
			if (element != nullptr && isBaseSlab(entityName(*element)))
				elements.push_back(id);
		}
		return elements;
	}

	bool resolveSlabTopElevation(const Model& model, double& out)
	{
		// 天端 Z（1e-3 で丸めたキー）ごとに平面面積を合計し、合計面積が最大の天端 Z を採る。
		std::map<long long, std::pair<double, double>> areas; // キー → (面積合計, 天端 Z)
		for (const int id : collectFootingElements(model))
		{
			const Entity* element = model.entity(id);
			if (element == nullptr || !isBaseSlab(entityName(*element)))
				continue;
			WorldSolid solid;
			if (!resolveElementWorldSolid(model, element, solid))
				continue;
			double top = 0.0;
			double thickness = 0.0;
			zTopAndThickness(solid, top, thickness);
			auto& entry = areas[roundKey(top, 1e-3)];
			entry.first += shoelaceArea(footprint(solid));
			entry.second = top;
		}
		if (areas.empty())
			return false;

		// 面積が最大の天端。同一面積なら高い方（キーは昇順なので > で更新すれば高い方が残る）。
		double bestArea = 0.0;
		double bestTop = 0.0;
		bool found = false;
		for (const auto& entry : areas)
		{
			const double area = entry.second.first;
			const double top = entry.second.second;
			if (!found || area > bestArea || (area >= bestArea && top > bestTop))
			{
				bestArea = area;
				bestTop = top;
				found = true;
			}
		}
		out = bestTop;
		return true;
	}

	bool resolveFoundationTopElevation(const Model& model, double& out)
	{
		// 立上り（基礎梁）の天端 Z のうち最大値。collectFootingElements は #id 昇順なので、
		// 最大値を採ることと合わせてエンティティ列挙順に依存しない決定的な高さになる。
		bool found = false;
		double best = 0.0;
		for (const int id : collectFootingElements(model))
		{
			const Entity* element = model.entity(id);
			if (element == nullptr || !isFoundationWall(entityName(*element)))
				continue;
			WorldSolid solid;
			if (!resolveElementWorldSolid(model, element, solid))
				continue;
			double top = 0.0;
			double thickness = 0.0;
			zTopAndThickness(solid, top, thickness);
			if (!found || top > best)
			{
				best = top;
				found = true;
			}
		}
		if (!found)
			return false;
		out = best;
		return true;
	}

	bool hasFoundation(const Model& model)
	{
		const std::vector<int> elements = collectFootingElements(model);
		return std::ranges::any_of(elements,
								   [&model](int id)
								   {
									   const Entity* element = model.entity(id);
									   if (element == nullptr)
										   return false;
									   const std::string name = entityName(*element);
									   return isFoundationWall(name) || isGroundBeam(name) ||
											  isBaseSlab(name);
								   });
	}

	bool buildFoundationStoryCommand(const Model& model, StoryCommand& out)
	{
		if (!hasFoundation(model))
			return false;

		double slabTop = 0.0;
		if (!resolveSlabTopElevation(model, slabTop))
			slabTop = 0.0; // 底盤の無い基礎（立上りのみ）は GL に揃える

		// 基礎天端はアンカーボルト（M11）の高さ基準＝立上りの天端。立上りが無い基礎（底盤のみ）
		// は底盤天端へフォールバックする。
		double foundationTop = slabTop;
		if (!resolveFoundationTopElevation(model, foundationTop))
			foundationTop = slabTop;

		StoryCommand cmd;
		cmd.name = kStoryFoundation;
		cmd.suffix = kFoundationSuffix;
		cmd.elevation = 0.0; // GL は常に 0
		// levels の並びは希望するデザインレイヤのスタック順（上→下）。上から
		// 基礎天端（アンカーボルト）→ GL（基礎の PIO）→ 床束。
		// 床束は基礎底盤の上端に立つので、高さは底盤天端に揃える（レベルは分ける——
		// 基礎のレイヤに床束を混ぜないため）。**GL のレイヤは高さ 0 でなければならない**
		// （基礎の PIO は部品の Z を GL 基準の絶対値で持つ。core/Foundation.h）。
		cmd.levels.push_back(
			LevelCommand{kLevelFoundationTop, foundationTop, kLayerFoundationAnchor});
		cmd.levels.push_back(LevelCommand{kLevelGL, 0.0, kLayerFoundation});
		cmd.levels.push_back(LevelCommand{kLevelFloorPost, slabTop, kLayerFoundationFloorPost});
		out = std::move(cmd);
		return true;
	}

	std::vector<RiserPiece> mergeWallCommands(const std::vector<RiserPiece>& walls)
	{
		// 断面キーごとにグループ化する（グループの並びは最初に現れた順＝入力順に決定的）。
		std::map<WallKey, std::size_t> index;
		std::vector<std::vector<RiserPiece>> groups;
		for (const RiserPiece& wall : walls)
		{
			const WallKey key = wallSectionKey(wall);
			const auto found = index.find(key);
			if (found == index.end())
			{
				index.emplace(key, groups.size());
				groups.emplace_back();
				groups.back().push_back(wall);
			}
			else
			{
				groups[found->second].push_back(wall);
			}
		}

		std::vector<RiserPiece> result;
		for (const std::vector<RiserPiece>& group : groups)
		{
			for (const RiserPiece& merged : mergeWallGroup(group))
				result.push_back(merged);
		}
		return result;
	}

	std::vector<RiserPiece> extendFreeWallEnds(const std::vector<RiserPiece>& walls,
											   const std::vector<ColumnCommand>& columns)
	{
		const std::size_t n = walls.size();
		// 各壁の始点・終点が他の立上りとの交点に関与するか。
		std::vector<bool> startJoined(n, false);
		std::vector<bool> endJoined(n, false);
		const auto mark = [&](std::size_t index, const Vec2& point)
		{
			const RiserPiece& wall = walls[index];
			const double toStart = core::distance(point, wall.start);
			const double toEnd = core::distance(point, wall.end);
			if (toStart <= toEnd)
				startJoined[index] = true;
			else
				endJoined[index] = true;
		};

		for (std::size_t i = 0; i < n; ++i)
		{
			for (std::size_t j = i + 1; j < n; ++j)
			{
				// 同一直線上で突き合わせになっている端は自由端ではない（延長すると隣へ
				// 食い込む。collinearAbutment 参照）。**統合できなかった隣**——上端／下端の
				// 違う立上り——との突き合わせがこれに当たる。
				bool atStart = false;
				bool atEnd = false;
				collinearAbutment(walls[i], walls[j], atStart, atEnd);
				startJoined[i] = startJoined[i] || atStart;
				endJoined[i] = endJoined[i] || atEnd;
				collinearAbutment(walls[j], walls[i], atStart, atEnd);
				startJoined[j] = startJoined[j] || atStart;
				endJoined[j] = endJoined[j] || atEnd;

				Vec2 point;
				bool aAtEnd = false;
				bool bAtEnd = false;
				if (!wallIntersection(walls[i], walls[j], point, aAtEnd, bAtEnd))
					continue;
				if (aAtEnd)
					mark(i, point);
				if (bAtEnd)
					mark(j, point);
			}
		}

		std::vector<RiserPiece> extended;
		extended.reserve(n);
		for (std::size_t i = 0; i < n; ++i)
		{
			RiserPiece wall = walls[i];
			const double length = core::distance(wall.start, wall.end);
			if (length <= 0.0)
			{
				extended.push_back(wall);
				continue;
			}
			const double half = wall.thickness / 2.0;
			const double ux = (wall.end.x - wall.start.x) / length;
			const double uy = (wall.end.y - wall.start.y) / length;

			if (!startJoined[i])
			{
				// 始端の自由端: 外向きは −軸方向。柱芯へ寄せてから半壁厚延長する。
				const Vec2 base = terminalColumnBase(wall.start, -ux, -uy, half, columns);
				wall.start = Vec2{base.x - (ux * half), base.y - (uy * half)};
			}
			if (!endJoined[i])
			{
				// 終端の自由端: 外向きは +軸方向。
				const Vec2 base = terminalColumnBase(wall.end, ux, uy, half, columns);
				wall.end = Vec2{base.x + (ux * half), base.y + (uy * half)};
			}
			extended.push_back(wall);
		}
		return extended;
	}

	std::vector<RiserPiece> extendDeeperCollinearEnds(const std::vector<RiserPiece>& walls)
	{
		const std::size_t n = walls.size();
		std::vector<RiserPiece> result = walls;
		for (std::size_t i = 0; i < n; ++i)
		{
			const RiserPiece& wall = walls[i];
			const double length = core::distance(wall.start, wall.end);
			if (length <= 0.0)
				continue;
			const double ux = (wall.end.x - wall.start.x) / length;
			const double uy = (wall.end.y - wall.start.y) / length;

			// 端ごとに「そこで一直線の線が続いていて、自分が深いほう」かを見る。
			for (int side = 0; side < 2; ++side)
			{
				const bool atStart = (side == 0);
				const Vec2 tip = atStart ? wall.start : wall.end;
				// 端から外側を向く単位ベクトル。
				const double ox = atStart ? -ux : ux;
				const double oy = atStart ? -uy : uy;

				// (1) この端が同一直線上の隣に「越えられている」か（線が続いているか）を見て、
				//     続いているなら自分が深いほう（下端が低い。同値なら添字が小さいほう）か。
				bool lineContinues = false;
				bool deepest = true;
				for (std::size_t j = 0; j < n && deepest; ++j)
				{
					if (j == i)
						continue;
					double lo = 0.0;
					double hi = 0.0;
					if (!wallsOnSameLine(wall, walls[j], lo, hi))
						continue;
					// 隣が端を越えて続いているか（始端側は 0 より手前・終端側は壁芯長より先）。
					const bool beyond =
						atStart ? (lo < -kWallMergeDistTol) : (hi > length + kWallMergeDistTol);
					if (!beyond)
						continue;
					// 天端が違う隣は「1 本に見せたい線」ではない（低い側の端部は閉じる）。
					if (std::abs(wall.top - walls[j].top) > kWallMergeDistTol)
						continue;
					lineContinues = true;
					const double mine = wall.bottom;
					const double theirs = walls[j].bottom;
					if (theirs < mine - kWallMergeDistTol ||
						(std::abs(theirs - mine) <= kWallMergeDistTol && j < i))
						deepest = false;
				}
				if (!lineContinues || !deepest)
					continue;

				// (2) その端が直交する立上りの**壁芯上**にあるか（＝そこで止まっている）。
				//     あれば相手の半壁厚のうち最大ぶん伸ばして、壁芯を越えさせる。
				double reach = 0.0;
				for (std::size_t j = 0; j < n; ++j)
				{
					if (j == i)
						continue;
					Vec2 point;
					bool aAtEnd = false;
					bool bAtEnd = false;
					if (!wallIntersection(wall, walls[j], point, aAtEnd, bAtEnd))
						continue; // 平行（同一直線の隣）はここで落ちる
					// 端そのもの（丸め誤差ぶん）で交わっている場合だけが対象。半壁厚の許容
					// （wallPointAtEnd）ではなく厳密に見る——相手の外面まで伸びている立上りを
					// さらに伸ばさないため。
					if (core::distance(tip, point) > kWallEndpointTol)
						continue;
					reach = std::max(reach, walls[j].thickness / 2.0);
				}
				if (reach <= 0.0)
					continue;

				const Vec2 moved{tip.x + (ox * reach), tip.y + (oy * reach)};
				if (atStart)
					result[i].start = moved;
				else
					result[i].end = moved;
			}
		}
		return result;
	}

	std::vector<WallOpening> collectWallOpenings(const Model& model, const Vec2& center)
	{
		std::vector<WallOpening> openings;
		for (const int id : model.byType("IFCFOOTING"))
		{
			const Entity* element = model.entity(id);
			if (element == nullptr || !isFoundationWall(entityName(*element)))
				continue;
			const std::vector<const Entity*> voids = elementVoidSolids(model, element);
			if (voids.empty())
				continue;
			// 素の立上り（差演算で削られる前）の天端・底面。人通口かどうかの判定に使う。
			WorldSolid base;
			if (!resolveElementWorldSolid(model, element, base))
				continue;
			double wallTopAbs = 0.0;
			double wallHeight = 0.0;
			zTopAndThickness(base, wallTopAbs, wallHeight);
			const double wallBottomAbs = wallTopAbs - wallHeight;

			const Mat4 placement = resolveObjectPlacement(model, element);
			for (const Entity* voidSolid : voids)
			{
				WorldSolid solid;
				if (!resolveExtrudedAreaSolid(model, voidSolid, placement, solid))
					continue;
				// 鉛直押し出しの削りは立上りの人通口ではない（壁芯が水平にならない）。
				if (std::abs(solid.extrudeDir.z) > kVerticalExtrudeTol)
					continue;
				double topAbs = 0.0;
				double thickness = 0.0;
				zTopAndThickness(solid, topAbs, thickness);
				const double bottomAbs = topAbs - thickness;
				// 人通口は「天端まで届き、底面には届かない」削り。端部が他材で削られた
				// 全高の差演算（素のソリッドを辿って無視するもの）を誤認しないための関門。
				if (topAbs < wallTopAbs - kWallMergeDistTol)
					continue;
				if (bottomAbs <= wallBottomAbs + kWallMergeDistTol)
					continue;

				WallOpening opening;
				opening.start = Vec2{solid.origin.x - center.x, solid.origin.y - center.y};
				opening.end = Vec2{opening.start.x + (solid.extrudeDir.x * solid.depth),
								   opening.start.y + (solid.extrudeDir.y * solid.depth)};
				opening.zBottom = bottomAbs;
				opening.zTop = topAbs;
				openings.push_back(opening);
			}
		}
		return openings;
	}

	std::vector<RiserPiece> applyWallOpenings(const std::vector<RiserPiece>& walls,
											  const std::vector<WallOpening>& openings,
											  double slabTopAbs)
	{
		std::vector<RiserPiece> result = walls;
		for (const WallOpening& opening : openings)
		{
			const std::size_t index = findOpeningWall(result, opening);
			if (index >= result.size())
				continue; // 乗る立上りが無い開口は無視する
			const std::vector<RiserPiece> carved =
				carveWallOpening(result[index], opening, slabTopAbs);
			result.erase(result.begin() + static_cast<std::ptrdiff_t>(index));
			result.insert(result.begin() + static_cast<std::ptrdiff_t>(index), carved.begin(),
						  carved.end());
		}
		return result;
	}

	std::vector<RiserPiece> buildWallCommands(Context& context,
											  const std::vector<ColumnCommand>& columns)
	{
		const Model& model = context.model();
		const Vec2 center = context.gridCenter();

		std::vector<RiserPiece> commands;
		for (const int id : model.byType("IFCFOOTING"))
		{
			const Entity* element = model.entity(id);
			if (element == nullptr || !isFoundationWall(entityName(*element)))
				continue;
			WorldSolid solid;
			if (!resolveElementWorldSolid(model, element, solid))
				continue; // 押し出しを解決できない立上りはスキップ
			// 立上りは矩形断面（幅=壁厚・背=壁高）を前提とする。非矩形断面は壁厚が定まらない。
			if (!solid.rectangle)
				continue;
			const double thickness = solid.xDim;
			const double height = solid.yDim;

			// 壁芯は配置原点から押し出し方向へ depth 伸ばした線（グリッド中心オフセット済み）。
			const Vec2 start{solid.origin.x - center.x, solid.origin.y - center.y};
			const Vec2 end{start.x + (solid.extrudeDir.x * solid.depth),
						   start.y + (solid.extrudeDir.y * solid.depth)};

			double topAbs = 0.0;
			double zThickness = 0.0;
			zTopAndThickness(solid, topAbs, zThickness);
			const double bottomAbs = topAbs - height;

			RiserPiece cmd;
			cmd.start = start;
			cmd.end = end;
			cmd.thickness = thickness;
			// 下端は IFC 実形状のまま（呑み込みはしない。parse/Footing.h「下端は IFC 実形状の
			// まま」参照）。天端も絶対 Z のまま持つ（M20 で高さ基準のレベルは無くなった）。
			cmd.bottom = bottomAbs;
			cmd.top = topAbs;
			commands.push_back(cmd);
		}
		// 統合 → 自由端の延長 → 深いほうの延長 → 人通口の当てはめ（docs/DEV-NOTES.md M10）。
		// **人通口は統合・延長の後**に当てはめるので、開口を跨いで統合された立上りも開口位置で
		// 正しく分割され、開口境界の端は実寸法のまま（延長しない）になる。深いほうの延長は
		// 自由端の延長の**後**（自由端ではない端＝直交する立上りの壁芯で止まっている端が対象で、
		// 自由端の延長とは対象が重ならない。parse/Footing.h 参照）。
		std::vector<RiserPiece> walls =
			extendDeeperCollinearEnds(extendFreeWallEnds(mergeWallCommands(commands), columns));
		const std::vector<WallOpening> openings = collectWallOpenings(model, center);
		if (openings.empty())
			return walls;

		// 開口下端が底盤天端以下なら「その区間に立上りは生じない」。底盤が 1 枚も無い基礎（立
		// 上りだけ）では比較相手が無いので、常に「開口下端の方が高い」＝切り下げになるよう十
		// 分小さい値を使う。
		double slabTopAbs = -std::numeric_limits<double>::infinity();
		double resolved = 0.0;
		if (resolveSlabTopElevation(model, resolved))
			slabTopAbs = resolved;
		return applyWallOpenings(walls, openings, slabTopAbs);
	}

	std::vector<RiserPiece> buildWallCommands(const Model& model)
	{
		Context context(model);
		return buildWallCommands(context, context.columns());
	}

	std::vector<SlabPiece> mergeSlabCommands(const std::vector<SlabPiece>& slabs)
	{
		// 断面キーごとにグループ化する（グループの並びは最初に現れた順＝入力順に決定的）。
		std::map<SlabKey, std::size_t> index;
		std::vector<std::vector<std::size_t>> groups;
		for (std::size_t i = 0; i < slabs.size(); ++i)
		{
			const SlabKey key = slabMergeKey(slabs[i]);
			const auto found = index.find(key);
			if (found == index.end())
			{
				index.emplace(key, groups.size());
				groups.emplace_back();
				groups.back().push_back(i);
			}
			else
			{
				groups[found->second].push_back(i);
			}
		}

		std::set<std::size_t> dropped;
		std::map<std::size_t, std::vector<SlabPiece>> mergedAt;
		for (const std::vector<std::size_t>& group : groups)
		{
			std::map<std::size_t, std::vector<Pt2>> polys;
			for (const std::size_t i : group)
			{
				std::vector<Pt2> ring = cleanRing(slabs[i].boundary);
				if (ring.size() >= 3)
					polys.emplace(i, std::move(ring));
			}
			for (const std::vector<std::size_t>& comp : slabComponents(polys))
			{
				if (comp.size() < 2)
					continue;
				std::vector<std::vector<Pt2>> parts;
				parts.reserve(comp.size());
				for (const std::size_t i : comp)
					parts.push_back(polys.at(i));

				std::vector<std::vector<Pt2>> loops;
				if (!polygonUnion(parts, loops))
					continue; // 開ループ（和を作れない成分）はそのまま残す

				std::vector<std::vector<Pt2>> outer;
				bool hasHole = false;
				for (const std::vector<Pt2>& loop : loops)
				{
					const double area = shoelaceSigned(loop);
					if (area > 0.0)
						outer.push_back(loop);
					else if (area < 0.0)
						hasHole = true;
				}
				// 単一の外形・穴無しの成分だけ 1 枚に統合する（穴があると単一境界で表せず、
				// 部屋の下までコンクリートで埋めると誤りになるため見送る）。
				if (outer.size() != 1 || hasHole)
					continue;

				SlabPiece merged = slabs[comp.front()];
				merged.boundary.clear();
				merged.boundary.reserve(outer.front().size());
				for (const Pt2& p : outer.front())
					merged.boundary.push_back(Vec2{p.first, p.second});
				mergedAt[comp.front()].push_back(std::move(merged));
				dropped.insert(comp.begin(), comp.end());
			}
		}

		std::vector<SlabPiece> result;
		for (std::size_t i = 0; i < slabs.size(); ++i)
		{
			const auto found = mergedAt.find(i);
			if (found != mergedAt.end())
				result.insert(result.end(), found->second.begin(), found->second.end());
			if (dropped.contains(i))
				continue;
			result.push_back(slabs[i]);
		}
		return result;
	}

	std::vector<SlabPiece> alignSlabsToWallFaces(const std::vector<SlabPiece>& slabs,
												 const std::vector<RiserPiece>& walls)
	{
		if (walls.empty())
			return slabs;

		std::vector<SlabPiece> result;
		result.reserve(slabs.size());
		for (const SlabPiece& slab : slabs)
		{
			std::vector<Vec2> boundary;
			if (!offsetBoundaryToWalls(slab.boundary, walls, boundary))
			{
				result.push_back(slab);
				continue;
			}
			SlabPiece adjusted = slab;
			adjusted.boundary = std::move(boundary);
			result.push_back(std::move(adjusted));
		}
		return result;
	}

	std::vector<BeamPrism> buildGroundBeamPrisms(const Model& model, const Vec2& center)
	{
		std::vector<BeamPrism> modifiers;
		for (const int id : model.byType("IFCFOOTING"))
		{
			const Entity* element = model.entity(id);
			if (element == nullptr || !isGroundBeam(entityName(*element)))
				continue;
			WorldSolid solid;
			if (!resolveElementWorldSolid(model, element, solid))
				continue;
			BeamPrism modifier;
			if (!groundBeamPrism(solid, center, modifier))
				continue;
			modifiers.push_back(std::move(modifier));
		}
		return mergeGroundBeamPrisms(modifiers);
	}

	std::vector<BeamPrism> mergeGroundBeamPrisms(const std::vector<BeamPrism>& prisms)
	{
		// グループキーごとにまとめる（グループの並びは最初に現れた順＝入力順に決定的）。
		std::map<GroundBeamKey, std::size_t> index;
		std::vector<std::vector<BeamPrism>> groups;
		for (const BeamPrism& modifier : prisms)
		{
			const GroundBeamKey key = groundBeamGroupKey(modifier);
			const auto found = index.find(key);
			if (found == index.end())
			{
				index.emplace(key, groups.size());
				groups.emplace_back();
				groups.back().push_back(modifier);
			}
			else
			{
				groups[found->second].push_back(modifier);
			}
		}

		std::vector<BeamPrism> result;
		for (const std::vector<BeamPrism>& group : groups)
		{
			for (BeamPrism& merged : mergeGroundBeamGroup(group))
				result.push_back(std::move(merged));
		}
		return result;
	}

	std::vector<SlabPiece> buildSlabCommands(Context& context, const std::vector<RiserPiece>& walls)
	{
		const Model& model = context.model();
		const Vec2 center = context.gridCenter();

		std::vector<SlabPiece> commands;
		for (const int id : collectFootingElements(model))
		{
			const Entity* element = model.entity(id);
			if (element == nullptr || !isBaseSlab(entityName(*element)))
				continue;
			WorldSolid solid;
			if (!resolveElementWorldSolid(model, element, solid))
				continue; // 押し出しを解決できない底盤はスキップ

			double topAbs = 0.0;
			double thickness = 0.0;
			zTopAndThickness(solid, topAbs, thickness);
			// スラブスタイルのコンクリート厚は整数 mm に丸める（同厚の底盤が別スタイルへ散ら
			// ないようにする）。
			const double concrete = std::round(thickness);

			std::vector<Vec2> boundary = footprint(solid);
			for (Vec2& p : boundary)
			{
				p.x -= center.x;
				p.y -= center.y;
			}

			SlabPiece cmd;
			cmd.boundary = std::move(boundary);
			cmd.thickness = concrete;
			cmd.elevation = topAbs;
			commands.push_back(std::move(cmd));
		}
		// 統合 → 外面合わせ（docs/DEV-NOTES.md M9）。地中梁の振り分けと床付け（M10 / M17）は
		// M20 で PIO 側（core/Foundation）へ移った——パラメータの変更のたびに描き直すため。
		return alignSlabsToWallFaces(mergeSlabCommands(commands), walls);
	}

	std::vector<SlabPiece> buildSlabCommands(const Model& model)
	{
		Context context(model);
		return buildSlabCommands(context, context.walls());
	}

	std::optional<core::FoundationCommand> buildFoundationCommand(Context& context)
	{
		const Model& model = context.model();
		if (!hasFoundation(model))
			return std::nullopt;

		// 立上り（統合・自由端の延長・人通口まで済んだもの）→ 底盤（統合・外面合わせ）→
		// 地中梁（統合済みのプリズム）。立上りは床束（parse/FloorPost）も使うので Context が
		// 1 回だけ組み立てる。
		const std::vector<RiserPiece>& walls = context.walls();
		const std::vector<SlabPiece> slabs = buildSlabCommands(context, walls);
		const std::vector<BeamPrism> beams = buildGroundBeamPrisms(model, context.gridCenter());

		core::FoundationCommand cmd;
		cmd.layer = kLayerFoundation;
		cmd.drawClass = CLASS_FOUNDATION_SLAB;
		cmd.slabClass = CLASS_FOUNDATION_SLAB;
		cmd.riserClass = CLASS_FOUNDATION_WALL;
		cmd.leanConcreteClass = CLASS_COMPONENT_LEAN_CONCRETE;
		cmd.gravelClass = CLASS_COMPONENT_GRAVEL;

		for (const SlabPiece& slab : slabs)
			cmd.slabs.push_back(
				core::FoundationSlab{slab.boundary, slab.elevation, slab.thickness});
		for (const RiserPiece& wall : walls)
		{
			cmd.risers.push_back(
				core::FoundationRiser{wall.start, wall.end, wall.thickness, wall.bottom, wall.top});
		}
		for (const BeamPrism& prism : beams)
		{
			// 断面をパラメータ（下端幅・張り出し・せい）へ当てはめる。当てはまらない断面
			// （3 点未満・押し出し長 0）は落とす（1 本の異常で全体を止めない）。
			core::FoundationBeam beam;
			if (core::fitFoundationBeam(prism, beam))
				cmd.beams.push_back(beam);
		}

		// 名前だけ基礎で実体（押し出し）を解決できない IFC は命令にしない（部品の無い PIO を
		// 置かない＝空のものを先に作らない方針）。
		if (cmd.slabs.empty() && cmd.risers.empty() && cmd.beams.empty())
			return std::nullopt;

		// OIP に最初に出る代表値は部品から求める（core::foundationBaseParams）。
		cmd.params = core::foundationBaseParams(cmd);
		return cmd;
	}

	std::optional<core::FoundationCommand> buildFoundationCommand(const Model& model)
	{
		Context context(model);
		return buildFoundationCommand(context);
	}
} // namespace HomeskzIfcImport::parse
