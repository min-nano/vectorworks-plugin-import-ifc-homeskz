//
//	parse/Footing.cpp
//
//	基礎解析の実装。Python 版 ifc/footing.py の build_foundation_story_command /
//	build_wall_commands / build_slab_commands（および統合・外面合わせ）に対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//
//	M10 で人通口（立上りの分割・切り下げ）・壁結合・地中梁（底盤のモディファイア）を足した。
//	**配筋は保留**（Python 版 ifc/reinforcement.py。足すときは wallSectionKey / slabMergeKey
//	にも足す。理由は各キーの doc コメント）。
//

#include "parse/Footing.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
#include "parse/IfcGeometry.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::ColumnCommand;
	using core::ComponentCommand;
	using core::LevelCommand;
	using core::SlabCommand;
	using core::StoryBoundCommand;
	using core::StoryCommand;
	using core::Vec2;
	using core::WallCommand;

	namespace
	{
		// --- 小さな共通ヘルパー ---------------------------------------------------

		// 名前が prefix で始まるか。
		bool startsWith(const std::string& name, const std::string& prefix)
		{
			return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
		}

		// 多角形の面積（絶対値。Python 版 _shoelace_area）。
		double shoelaceArea(const std::vector<Vec2>& pts)
		{
			double total = 0.0;
			const std::size_t n = pts.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& a = pts[i];
				const Vec2& b = pts[(i + 1) % n];
				total += (a.x * b.y) - (b.x * a.y);
			}
			return std::abs(total) / 2.0;
		}

		// 符号付き面積（CCW で正・CW で負。Python 版 _shoelace_signed）。
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

		// 許容値で丸めた整数キー（グループ化の鍵。Python 版 round(v / tol) / round(v, 3)）。
		// std::llround は 0.5 を 0 から離れる向きへ丸める（Python の銀行家丸めとは境界だけ
		// 挙動が違うが、実座標がちょうど半端に乗ることは無いので実害は無い）。
		long long roundKey(double value, double tolerance)
		{
			return std::llround(value / tolerance);
		}

		// --- 立上り（壁）------------------------------------------------------------

		// 立上りの断面形状（統合可否）を表すキー（Python 版 _wall_section_key）。レイヤ・
		// クラス・壁厚・上下端の高さ基準がすべて一致する立上り同士だけを統合対象にする。
		// **配筋（M10）を足すときはこのキーにも足す**（配筋の違う立上りを 1 本へ統合すると
		// 片方の配筋が失われるため。Python 版はキーに含めている）。
		using WallKey = std::tuple<std::string, std::string, long long, int, std::string, long long,
								   int, std::string, long long>;

		WallKey wallSectionKey(const WallCommand& wall)
		{
			return WallKey{wall.layer,
						   wall.drawClass,
						   roundKey(wall.thickness, 1e-3),
						   wall.bottomBound.storyOffset,
						   wall.bottomBound.level,
						   roundKey(wall.bottomBound.offset, kWallMergeDistTol),
						   wall.topBound.storyOffset,
						   wall.topBound.level,
						   roundKey(wall.topBound.offset, kWallMergeDistTol)};
		}

		// 立上り b が a と**同一直線上**（平行かつ壁芯が同じ線）にあるか。区間の重なりは
		// 問わない（離れた延長線上の立上りも true）。true のとき、b を a の壁芯方向へ射影した
		// 区間 [outLo, outHi] を返す（a の始点が 0・終点が壁芯長）。
		//
		// **統合（重なり／接触が要る）と、自由端の延長制限（離れた隣も要る）が同じ「同一直線
		// 判定」を共有する**ので、条件はここに 1 つだけ置く。
		bool wallsOnSameLine(const WallCommand& a, const WallCommand& b, double& outLo,
							 double& outHi)
		{
			const Vec2 da = a.end - a.start;
			const Vec2 db = b.end - b.start;
			const double la = std::hypot(da.x, da.y);
			const double lb = std::hypot(db.x, db.y);
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

		// 立上り a・b が同一直線上にあり、区間が重なる／接触するか（Python 版
		// _walls_connected_collinear）。同一直線判定は wallsOnSameLine と共有する。
		bool wallsConnectedCollinear(const WallCommand& a, const WallCommand& b)
		{
			double lo = 0.0;
			double hi = 0.0;
			if (!wallsOnSameLine(a, b, lo, hi))
				return false;
			const double la = std::hypot(a.end.x - a.start.x, a.end.y - a.start.y);
			return hi >= -kWallMergeDistTol && lo <= la + kWallMergeDistTol;
		}

		// Union-Find の根を返す（経路圧縮つき）。
		std::size_t findRoot(std::vector<std::size_t>& parent, std::size_t a)
		{
			while (parent[a] != a)
			{
				parent[a] = parent[parent[a]];
				a = parent[a];
			}
			return a;
		}

		// 同一断面の立上り群のうち、同一直線上で連続するものを 1 本に統合する
		// （Python 版 _merge_wall_group）。成分の代表は最小インデックスで、出力は代表
		// インデックス昇順＝入力順に準ずる（列挙順に依存しない決定的な並び）。
		std::vector<WallCommand> mergeWallGroup(const std::vector<WallCommand>& walls)
		{
			const std::size_t n = walls.size();
			std::vector<std::size_t> parent(n);
			for (std::size_t i = 0; i < n; ++i)
				parent[i] = i;

			for (std::size_t i = 0; i < n; ++i)
			{
				for (std::size_t j = i + 1; j < n; ++j)
				{
					if (!wallsConnectedCollinear(walls[i], walls[j]))
						continue;
					const std::size_t ri = findRoot(parent, i);
					const std::size_t rj = findRoot(parent, j);
					if (ri != rj)
						parent[std::max(ri, rj)] = std::min(ri, rj);
				}
			}

			std::map<std::size_t, std::vector<std::size_t>> components;
			for (std::size_t i = 0; i < n; ++i)
				components[findRoot(parent, i)].push_back(i);

			std::vector<WallCommand> merged;
			merged.reserve(components.size());
			for (const auto& component : components)
			{
				const std::vector<std::size_t>& indices = component.second;
				const WallCommand& base = walls[indices.front()];
				if (indices.size() == 1)
				{
					merged.push_back(base);
					continue;
				}
				// 先頭の壁芯方向へ全端点を射影し、最小〜最大区間の 1 本にする
				// （高さ基準・壁厚・クラスは先頭のものを引き継ぐ）。
				const Vec2 axis = base.end - base.start;
				const double la = std::hypot(axis.x, axis.y);
				const double ux = axis.x / la;
				const double uy = axis.y / la;
				double lo = 0.0;
				double hi = 0.0;
				bool first = true;
				for (const std::size_t index : indices)
				{
					for (const Vec2& p : {walls[index].start, walls[index].end})
					{
						const double t = (ux * (p.x - base.start.x)) + (uy * (p.y - base.start.y));
						if (first || t < lo)
							lo = t;
						if (first || t > hi)
							hi = t;
						first = false;
					}
				}
				WallCommand cmd = base;
				cmd.start = Vec2{base.start.x + (ux * lo), base.start.y + (uy * lo)};
				cmd.end = Vec2{base.start.x + (ux * hi), base.start.y + (uy * hi)};
				merged.push_back(cmd);
			}
			return merged;
		}

		// 立上り a・b の壁芯の交点と、その交点が各壁芯の端点か内部かを求める（Python 版
		// _wall_intersection）。平行（交点が定まらない）・区間外で交わる立上りは false。
		//
		// **端点許容は相手壁の半壁厚を含める**: ホームズ君 IFC の立上りは直交する相手壁の
		// 外面までモデル化されるため、コーナーでは壁芯どうしの交点が壁の端から半壁厚ほど
		// 離れた位置に来る。固定 1mm の許容だと外周コーナーが「両方とも内部で交わる」と
		// 誤判定され、自由端の判定（延長する／しない）を取り違える。
		bool wallIntersection(const WallCommand& a, const WallCommand& b, Vec2& outPoint,
							  bool& outAAtEnd, bool& outBAtEnd)
		{
			const Vec2 r = a.end - a.start;
			const Vec2 s = b.end - b.start;
			const double la = std::hypot(r.x, r.y);
			const double lb = std::hypot(s.x, s.y);
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
		// できない＝上端／下端の違う隣どうしで顕在化する。ROADMAP.md M10）。
		void collinearAbutment(const WallCommand& a, const WallCommand& b, bool& outAtStart,
							   bool& outAtEnd)
		{
			outAtStart = false;
			outAtEnd = false;
			double lo = 0.0;
			double hi = 0.0;
			if (!wallsOnSameLine(a, b, lo, hi))
				return; // 平行でない／別の線上
			const double la = std::hypot(a.end.x - a.start.x, a.end.y - a.start.y);
			if (hi < -kWallMergeDistTol || lo > la + kWallMergeDistTol)
				return; // 同一直線上だが離れている（隙間がある）＝突き合わせではない
			// b が a の端へ届いている（接する／越える）側だけを「突き合わせ」とみなす。
			// b が a の内側に完全に収まっている場合はどちらの端も自由端のまま。
			outAtStart = lo <= kWallMergeDistTol;
			outAtEnd = hi >= la - kWallMergeDistTol;
		}

		// 自由端の終端柱（柱芯）を壁芯上に射影した基準点を返す（Python 版
		// _terminal_column_base）。(ux, uy) は自由端で壁の外側を向く単位ベクトル。終端柱が
		// 見つからなければ自由端そのものを返す（柱芯 = 自由端とみなす）。
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

		// 人通口の区間が乗っている立上りの添字を返す（Python 版 _find_opening_wall）。
		// 開口が (1) 壁芯と平行で (2) 壁芯線上（直交距離が許容内）にあり (3) 開口の中点が
		// 壁芯区間内にある立上りを探す。側並び（直交距離が半壁厚ある平行壁）には乗らない。
		// 見つからなければ walls.size()。
		std::size_t findOpeningWall(const std::vector<WallCommand>& walls,
									const WallOpening& opening)
		{
			const Vec2 delta = opening.end - opening.start;
			const double openingLength = std::hypot(delta.x, delta.y);
			if (openingLength <= 0.0)
				return walls.size();
			const double oux = delta.x / openingLength;
			const double ouy = delta.y / openingLength;
			const double mx = (opening.start.x + opening.end.x) / 2.0;
			const double my = (opening.start.y + opening.end.y) / 2.0;

			for (std::size_t i = 0; i < walls.size(); ++i)
			{
				const WallCommand& wall = walls[i];
				const Vec2 axis = wall.end - wall.start;
				const double length = std::hypot(axis.x, axis.y);
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

		// 人通口の区間で立上りを分割／切り下げた列を返す（Python 版 _carve_wall_opening）。
		// 開口の下端が底盤天端以下なら中間区間を出さず両側だけを、それより高ければ天端を
		// 開口下端へ切り下げた中間区間を挟む。端部の長さ補正はしない（実寸法のまま）。
		std::vector<WallCommand> carveWallOpening(const WallCommand& wall,
												  const WallOpening& opening, double slabTopAbs,
												  double beamTopAbs)
		{
			const Vec2 axis = wall.end - wall.start;
			const double length = std::hypot(axis.x, axis.y);
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

			const auto segment = [&](double t0, double t1, const StoryBoundCommand& topBound)
			{
				WallCommand cmd = wall;
				cmd.start = Vec2{wall.start.x + (ux * t0), wall.start.y + (uy * t0)};
				cmd.end = Vec2{wall.start.x + (ux * t1), wall.start.y + (uy * t1)};
				cmd.topBound = topBound;
				return cmd;
			};

			std::vector<WallCommand> segments;
			if (o0 > kOpeningMinSegment)
				segments.push_back(segment(0.0, o0, wall.topBound));
			// 開口の下端が底盤天端より高ければ、その区間だけ天端を切り下げた立上りを挟む。
			if (opening.zBottom > slabTopAbs + kWallMergeDistTol)
			{
				StoryBoundCommand lowered = wall.topBound;
				lowered.offset = opening.zBottom - beamTopAbs;
				segments.push_back(segment(o0, o1, lowered));
			}
			if (length - o1 > kOpeningMinSegment)
				segments.push_back(segment(o1, length, wall.topBound));
			return segments;
		}

		// --- 壁結合 --------------------------------------------------------------------

		// 立上りの天端高さの比較値（Python 版 _wall_top）。基礎の立上りはすべて同じレベル
		// （1 階の横架材天端）へ上端をバインドするので、offset だけで高低を比べられる。
		double wallTop(const WallCommand& wall)
		{
			return wall.topBound.offset;
		}

		// 壁芯上の点が立上りの端点とみなせるか（Python 版 _wall_point_at_end）。端からの距離が
		// 半壁厚 + kWallEndpointTol 以内なら端点（立上りは相手壁の外面まで伸びるため、コーナーの
		// 交点が壁の端から半壁厚離れる。wallIntersection と同じ考え）。
		bool wallPointAtEnd(const WallCommand& wall, const Vec2& point)
		{
			const Vec2 axis = wall.end - wall.start;
			const double length = std::hypot(axis.x, axis.y);
			if (length <= 0.0)
				return true;
			const double t =
				(((point.x - wall.start.x) * axis.x) + ((point.y - wall.start.y) * axis.y)) /
				(length * length);
			const double frac = ((wall.thickness / 2.0) + kWallEndpointTol) / length;
			return (t <= frac) || (t >= 1.0 - frac);
		}

		// 交点から wall の指定した端点の方向へ寄せた壁芯上の点を返す。寄せ量は offset を
		// 「交点〜その端点の距離 × kPickOffsetFrac」でクランプした値。端点が交点と同じ位置なら
		// 交点をそのまま返す。keptSidePick と、通し壁のピック点をずらす側の指定で共有する。
		Vec2 sidePick(const WallCommand& wall, const Vec2& junction, double offset, bool towardEnd)
		{
			const Vec2 target = towardEnd ? wall.end : wall.start;
			const Vec2 delta = target - junction;
			const double length = std::hypot(delta.x, delta.y);
			if (length <= 0.0)
				return junction;
			const double step = std::min(offset, length * kPickOffsetFrac) / length;
			return Vec2{junction.x + (delta.x * step), junction.y + (delta.y * step)};
		}

		// 交点から「残す側」（＝交点から遠い端点の方向）へ寄せたピック点を返す（Python 版
		// _kept_side_pick）。壁芯長 0 の壁は交点をそのまま返す。
		Vec2 keptSidePick(const WallCommand& wall, const Vec2& junction, double offset)
		{
			const double d1 = std::hypot(wall.start.x - junction.x, wall.start.y - junction.y);
			const double d2 = std::hypot(wall.end.x - junction.x, wall.end.y - junction.y);
			return sidePick(wall, junction, offset, d2 >= d1);
		}

		// 立上りの壁芯方向ベクトルと長さ（Python 版 _line_dir）。
		void wallDirection(const WallCommand& wall, double& outDx, double& outDy, double& outLength)
		{
			outDx = wall.end.x - wall.start.x;
			outDy = wall.end.y - wall.start.y;
			outLength = std::hypot(outDx, outDy);
		}

		// 同一交点に集まる立上りの集合（ジャンクション）。point は代表エッジの交点。
		struct Junction
		{
			Vec2 point;
			std::vector<std::size_t> walls; // 添字昇順
		};

		// 交差する立上りのペアから、同一交点に集まる立上りの集合を作る（Python 版
		// _wall_junctions）。全ペアの壁芯交点を求め、kJoinClusterTol 以内で近い交点を
		// Union-Find で 1 つのジャンクションへ束ねる（3 本以上が 1 点に集まる場合、その全ペアの
		// 交点は数学的に同一点になるので 1 つに束ねられる）。並びは代表エッジの添字昇順。
		std::vector<Junction> wallJunctions(const std::vector<WallCommand>& walls)
		{
			struct JoinEdge
			{
				std::size_t a = 0;
				std::size_t b = 0;
				Vec2 point;
			};

			std::vector<JoinEdge> edges;
			const std::size_t n = walls.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				for (std::size_t j = i + 1; j < n; ++j)
				{
					if (walls[i].layer != walls[j].layer)
						continue;
					Vec2 point;
					bool aAtEnd = false;
					bool bAtEnd = false;
					if (!wallIntersection(walls[i], walls[j], point, aAtEnd, bAtEnd))
						continue;
					edges.push_back(JoinEdge{i, j, point});
				}
			}

			const std::size_t m = edges.size();
			std::vector<std::size_t> parent(m);
			for (std::size_t i = 0; i < m; ++i)
				parent[i] = i;
			for (std::size_t p = 0; p < m; ++p)
			{
				for (std::size_t q = p + 1; q < m; ++q)
				{
					if (std::hypot(edges[p].point.x - edges[q].point.x,
								   edges[p].point.y - edges[q].point.y) > kJoinClusterTol)
						continue;
					const std::size_t rp = findRoot(parent, p);
					const std::size_t rq = findRoot(parent, q);
					if (rp != rq)
						parent[std::max(rp, rq)] = std::min(rp, rq);
				}
			}

			std::map<std::size_t, std::vector<std::size_t>> clusters;
			for (std::size_t p = 0; p < m; ++p)
				clusters[findRoot(parent, p)].push_back(p);

			std::vector<Junction> junctions;
			junctions.reserve(clusters.size());
			for (const auto& cluster : clusters)
			{
				std::set<std::size_t> indices;
				for (const std::size_t edgeIndex : cluster.second)
				{
					indices.insert(edges[edgeIndex].a);
					indices.insert(edges[edgeIndex].b);
				}
				Junction junction;
				junction.point = edges[cluster.second.front()].point; // 代表＝最小エッジ添字
				junction.walls.assign(indices.begin(), indices.end());
				junctions.push_back(std::move(junction));
			}
			return junctions;
		}

		// --- 地中梁（底盤のモディファイア）----------------------------------------------

		// 地中梁の押し出し方向（方位角）の水平単位ベクトル（Python 版 _ground_beam_axis_dir）。
		Vec2 groundBeamAxisDir(const core::ModifierCommand& modifier)
		{
			const double phi = modifier.azimuth * std::numbers::pi / 180.0;
			return Vec2{std::cos(phi), std::sin(phi)};
		}

		// 断面形状（統合可否）を表す正規化キー（Python 版 _ground_beam_profile_key）。頂点を
		// 許容値で丸め、巻きを CCW に揃えたうえで辞書順最小の頂点から始まる回転に正規化する。
		// **頂点の絶対 (u, v) 位置は保つ**ので、軸に対する横位置（u オフセット）の違う地中梁は
		// 別キーになり統合されない。
		using GroundBeamProfileKey = std::vector<std::pair<long long, long long>>;

		GroundBeamProfileKey groundBeamProfileKey(const core::ModifierCommand& modifier)
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

		// 統合対象を粗くまとめるグループキー（高さ＝下端 z・方位角・断面形状。Python 版
		// _ground_beam_group_key）。実際に同一軸線上かは modifiersCollinear が判定する。
		using GroundBeamKey = std::tuple<long long, long long, GroundBeamProfileKey>;

		GroundBeamKey groundBeamGroupKey(const core::ModifierCommand& modifier)
		{
			return GroundBeamKey{roundKey(modifier.origin.z, kGroundBeamMergeTol),
								 roundKey(modifier.azimuth, kGroundBeamAzimuthTol),
								 groundBeamProfileKey(modifier)};
		}

		// 地中梁 a・b が同一軸線上（同一高さ）にあり区間が連続するか（Python 版
		// _modifiers_collinear）。(1) 高さが一致、(2) 方向が平行、(3) b の原点が a の軸線上、
		// (4) a の区間 [0, depth] と b の射影区間が重なる／接触する。
		bool modifiersCollinear(const core::ModifierCommand& a, const core::ModifierCommand& b)
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

		// 同一軸線上の地中梁群を 1 本の台形プリズムへ統合する（Python 版
		// _merge_ground_beam_component）。先頭の軸方向へ全端点を射影し、最小〜最大区間を
		// 新しい押し出しにする（断面・方位角・高さは先頭を引き継ぐ）。
		core::ModifierCommand
		mergeGroundBeamComponent(const std::vector<core::ModifierCommand>& members)
		{
			const core::ModifierCommand& base = members.front();
			const Vec2 axis = groundBeamAxisDir(base);
			double lo = 0.0;
			double hi = 0.0;
			bool first = true;
			for (const core::ModifierCommand& modifier : members)
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
			core::ModifierCommand merged = base;
			merged.depth = hi - lo;
			merged.origin = core::Vec3{base.origin.x + (axis.x * lo), base.origin.y + (axis.y * lo),
									   base.origin.z};
			return merged;
		}

		// 同一断面・同一向きの地中梁群のうち、同一軸線上で連続するものを統合する（Python 版
		// _merge_ground_beam_group）。成分の代表は最小添字で、出力は代表添字昇順。
		std::vector<core::ModifierCommand>
		mergeGroundBeamGroup(const std::vector<core::ModifierCommand>& modifiers)
		{
			const std::size_t n = modifiers.size();
			std::vector<std::size_t> parent(n);
			for (std::size_t i = 0; i < n; ++i)
				parent[i] = i;
			for (std::size_t i = 0; i < n; ++i)
			{
				for (std::size_t j = i + 1; j < n; ++j)
				{
					if (!modifiersCollinear(modifiers[i], modifiers[j]))
						continue;
					const std::size_t ri = findRoot(parent, i);
					const std::size_t rj = findRoot(parent, j);
					if (ri != rj)
						parent[std::max(ri, rj)] = std::min(ri, rj);
				}
			}

			std::map<std::size_t, std::vector<std::size_t>> components;
			for (std::size_t i = 0; i < n; ++i)
				components[findRoot(parent, i)].push_back(i);

			std::vector<core::ModifierCommand> merged;
			merged.reserve(components.size());
			for (const auto& component : components)
			{
				if (component.second.size() == 1)
				{
					merged.push_back(modifiers[component.second.front()]);
					continue;
				}
				std::vector<core::ModifierCommand> members;
				members.reserve(component.second.size());
				for (const std::size_t i : component.second)
					members.push_back(modifiers[i]);
				merged.push_back(mergeGroundBeamComponent(members));
			}
			return merged;
		}

		// 地中梁 1 本の押し出しソリッドを台形プリズムのモディファイアにする（Python 版
		// _ground_beam_modifier）。押し出し方向の水平成分から方位角を求め、断面頂点を
		// 幅軸 u（走る向きを +90 度回した水平単位ベクトル w）・鉛直軸 v（ワールド Z の差分）へ
		// 取り直す。押し出しが水平でない（鉛直）ソリッドは地中梁でないので false。
		bool groundBeamModifier(const WorldSolid& solid, const Vec2& center,
								core::ModifierCommand& out)
		{
			const double runLength = std::hypot(solid.extrudeDir.x, solid.extrudeDir.y);
			if (runLength <= 0.0)
				return false;
			const double ux = solid.extrudeDir.x / runLength;
			const double uy = solid.extrudeDir.y / runLength;
			// 幅軸 w＝走る向きを +90 度回した水平単位ベクトル（描画側の復元規約と対で決まる）。
			const double wx = -uy;
			const double wy = ux;

			core::ModifierCommand cmd;
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

		// 多角形の重心（頂点の相加平均。Python 版 _polygon_centroid）。
		Vec2 polygonCentroid(const std::vector<Vec2>& pts)
		{
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

		// 平面外形の代表点（重心・各頂点・各辺の中点。Python 版 _footprint_samples）。
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

		// --- 底盤（スラブ）----------------------------------------------------------
		//
		// 多角形の和（union）は「頂点を丸めて厳密比較できるようにし、全辺を交点で細分し、
		// すぐ右（外側）がどの多角形にも入らない有向辺だけを境界として残してつなぐ」という
		// 手順で求める（Python 版 _polygon_union）。丸めた点をキーにするので、集合・辞書は
		// 素直な std::set / std::map（辞書順比較）で足りる。

		// 交点計算で頂点を丸める小数桁（1e-4 mm = 0.1 ミクロン。Python 版 _SLAB_ROUND）。
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

		// 境界を丸めた頂点列にし、末尾の閉じ重複・連続する同一点を除く（Python 版 _clean_ring）。
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

		// 点 (x, y) が単純多角形の内部（境界は含めない近似）にあるか（Python 版
		// _point_in_poly）。水平レイキャスト（半開ルール）。呼び出し側は辺から法線方向へ
		// kSlabSideEps ずらした点を渡すので、辺ちょうどの縮退は問題にならない。
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

		// 線分 ab を分割すべき点（線分 cd との交点）を ab 上の点として返す（Python 版
		// _seg_split_points）。非平行なら区間内の交点、共線なら cd の端点を ab 上へ射影した
		// 点（区間内）。これで交差・T 字接合・共線オーバーラップの分割点をすべて拾う。
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

		// 有向辺 a→b を分割点 cuts で細分した有向部分辺のリスト（Python 版 _split_edge）。
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

		// 境界追跡で分岐点に来たとき、内側を左に保つ次の辺（最も時計回り）を選ぶ
		// （Python 版 _next_boundary_edge）。
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

		// 閉リングから共線の中間点を除いた頂点列（Python 版 _simplify_ring）。
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

		// 有向境界辺をつないで閉ループのリストにする（Python 版 _chain_boundary）。
		// 開ループが生じたら false（統合できない成分として呼び出し側が元のまま残す）。
		bool chainBoundary(const std::vector<Edge>& edges, std::vector<std::vector<Pt2>>& out)
		{
			std::map<Pt2, std::vector<Edge>> fromMap;
			for (const Edge& edge : edges)
				fromMap[edge.first].push_back(edge);
			std::set<Edge> remaining(edges.begin(), edges.end());

			std::vector<std::vector<Pt2>> loops;
			while (!remaining.empty())
			{
				const Edge start = *remaining.begin(); // std::set は辞書順＝Python の min
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

		// 任意向きの単純多角形群の和（union）の境界ループ（Python 版 _polygon_union）。
		// 各多角形を CCW（内部が左）に揃え、全辺を他辺との交点で細分し、細分した有向辺のうち
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

		// 共線の線分 ab・cd の重なり長さ（共線でなければ 0。Python 版 _collinear_overlap）。
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

		// 線分 a→b 上の点 p の正規化パラメータ（0=a, 1=b。Python 版 _param_on）。
		double paramOn(const Pt2& a, const Pt2& b, const Pt2& p)
		{
			const double rx = b.first - a.first;
			const double ry = b.second - a.second;
			const double length2 = (rx * rx) + (ry * ry);
			if (length2 <= 0.0)
				return 0.0;
			return (((p.first - a.first) * rx) + ((p.second - a.second) * ry)) / length2;
		}

		// 底盤ポリゴン a・b が連続する（境界を共有 or 面で重なる）か（Python 版
		// _polys_connected）。角（点）だけで接する場合は連続としない。
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

		// 底盤の統合可否を表すキー（Python 版 _slab_merge_key）。レイヤ・クラス・コンクリート
		// 厚・高さ基準がすべて一致する底盤同士だけを統合対象にする。**配筋（M10）を足すときは
		// このキーにも足す**（配筋の違う底盤を 1 枚へ統合すると片方が失われるため）。
		using SlabKey =
			std::tuple<std::string, std::string, long long, int, std::string, long long>;

		SlabKey slabMergeKey(const SlabCommand& slab)
		{
			return SlabKey{slab.layer,
						   slab.drawClass,
						   roundKey(slab.thickness, 1e-3),
						   slab.bound.storyOffset,
						   slab.bound.level,
						   roundKey(slab.bound.offset, kSlabMergeTol)};
		}

		// 連続する底盤（polysConnected）の連結成分をインデックス集合で返す（Python 版
		// _slab_components）。成分は昇順・成分内も昇順で決定的。
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

		// 底盤の辺 a→b に沿う立上りの半壁厚（該当が無ければ 0。Python 版
		// _wall_half_thickness_for_edge）。最も重なりの大きい立上りの半壁厚を採る。
		double wallHalfThicknessForEdge(const Vec2& a, const Vec2& b,
										const std::vector<WallCommand>& walls)
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
			for (const WallCommand& wall : walls)
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

		// 点 p・方向 d の 2 直線の交点（平行なら false。Python 版 _line_intersection）。
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

		// CCW ポリゴンの各辺 i を外向きへ dists[i] だけ移動した頂点列（Python 版
		// _offset_polygon）。隣接する移動後の辺（直線）の交点を新しい頂点にするので、凸角は
		// 外側へ伸び、凹角（入隅）は詰まる。
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

		// 底盤外形の各辺を、沿っている立上りの外面まで外側へ広げた外形（Python 版
		// _offset_boundary_to_walls）。立上りに沿う辺が 1 つも無ければ false（呼び出し側は
		// 元の外形をそのまま使う）。
		bool offsetBoundaryToWalls(const std::vector<Vec2>& boundary,
								   const std::vector<WallCommand>& walls, std::vector<Vec2>& out)
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
		return startsWith(name, kFoundationWallPrefix);
	}

	bool isGroundBeam(const std::string& name)
	{
		return name.find(kGroundBeamToken) != std::string::npos;
	}

	bool isBaseSlab(const std::string& name)
	{
		return name.find(kBaseSlabToken) != std::string::npos;
	}

	std::string foundationSlabStyleName(double concreteThickness)
	{
		// "基礎スラブ - コンクリート 150mm / 捨てコン 30mm / 砕石 100mm"。厚みは整数 mm
		// （命令の thickness は解析側で丸め済みだが、名前を作るここでも整数化して揺れを断つ）。
		const auto mm = [](double value) { return std::to_string(std::llround(value)) + "mm"; };
		return std::string("基礎スラブ - ") + kConcreteComponentName + " " + mm(concreteThickness) +
			   " / " + kSlabLeanConcreteName + " " + mm(kSlabLeanConcreteThickness) + " / " +
			   kSlabGravelName + " " + mm(kSlabGravelThickness);
	}

	std::string foundationWallStyleName(double thickness)
	{
		// "基礎立上り - コンクリート 150mm"。底盤のスタイル名と同じ流儀（厚みは整数 mm）。
		return std::string("基礎立上り - ") + kConcreteComponentName + " " +
			   std::to_string(std::llround(thickness)) + "mm";
	}

	std::vector<ComponentCommand> foundationWallComponents(double thickness)
	{
		// 立上りはコンクリート 1 層（総厚＝壁厚）。
		return {ComponentCommand{kConcreteComponentName, thickness}};
	}

	std::vector<ComponentCommand> foundationSlabComponents(double concreteThickness)
	{
		return {ComponentCommand{kConcreteComponentName, concreteThickness},
				ComponentCommand{kSlabLeanConcreteName, kSlabLeanConcreteThickness},
				ComponentCommand{kSlabGravelName, kSlabGravelThickness}};
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

		StoryCommand cmd;
		cmd.name = kStoryFoundation;
		cmd.suffix = kFoundationSuffix;
		cmd.elevation = 0.0; // GL は常に 0
		// levels の並びは希望するデザインレイヤのスタック順（上→下）。立上り（GL）を
		// 底盤（底盤天端）の上段に積む。基礎天端・床束は M11（ヘッダ冒頭参照）。
		cmd.levels.push_back(LevelCommand{kLevelGL, 0.0, kLayerFoundationWall});
		cmd.levels.push_back(LevelCommand{kLevelSlabTop, slabTop, kLayerFoundationSlab});
		out = std::move(cmd);
		return true;
	}

	std::vector<WallCommand> mergeWallCommands(const std::vector<WallCommand>& walls)
	{
		// 断面キーごとにグループ化する（グループの並びは最初に現れた順＝入力順に決定的）。
		std::map<WallKey, std::size_t> index;
		std::vector<std::vector<WallCommand>> groups;
		for (const WallCommand& wall : walls)
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

		std::vector<WallCommand> result;
		for (const std::vector<WallCommand>& group : groups)
		{
			for (WallCommand& merged : mergeWallGroup(group))
				result.push_back(std::move(merged));
		}
		return result;
	}

	std::vector<WallCommand> extendFreeWallEnds(const std::vector<WallCommand>& walls,
												const std::vector<ColumnCommand>& columns)
	{
		const std::size_t n = walls.size();
		// 各壁の始点・終点が他の立上りとの交点に関与するか。
		std::vector<bool> startJoined(n, false);
		std::vector<bool> endJoined(n, false);
		const auto mark = [&](std::size_t index, const Vec2& point)
		{
			const WallCommand& wall = walls[index];
			const double toStart = std::hypot(wall.start.x - point.x, wall.start.y - point.y);
			const double toEnd = std::hypot(wall.end.x - point.x, wall.end.y - point.y);
			if (toStart <= toEnd)
				startJoined[index] = true;
			else
				endJoined[index] = true;
		};

		for (std::size_t i = 0; i < n; ++i)
		{
			for (std::size_t j = i + 1; j < n; ++j)
			{
				if (walls[i].layer != walls[j].layer)
					continue;

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

		std::vector<WallCommand> extended;
		extended.reserve(n);
		for (std::size_t i = 0; i < n; ++i)
		{
			WallCommand wall = walls[i];
			const double length = std::hypot(wall.end.x - wall.start.x, wall.end.y - wall.start.y);
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

	std::vector<WallCommand> extendDeeperCollinearEnds(const std::vector<WallCommand>& walls)
	{
		const std::size_t n = walls.size();
		std::vector<WallCommand> result = walls;
		for (std::size_t i = 0; i < n; ++i)
		{
			const WallCommand& wall = walls[i];
			const double length = std::hypot(wall.end.x - wall.start.x, wall.end.y - wall.start.y);
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
					if (j == i || walls[j].layer != wall.layer)
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
					if (std::abs(wall.topBound.offset - walls[j].topBound.offset) >
						kWallMergeDistTol)
						continue;
					lineContinues = true;
					const double mine = wall.bottomBound.offset;
					const double theirs = walls[j].bottomBound.offset;
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
					if (j == i || walls[j].layer != wall.layer)
						continue;
					Vec2 point;
					bool aAtEnd = false;
					bool bAtEnd = false;
					if (!wallIntersection(wall, walls[j], point, aAtEnd, bAtEnd))
						continue; // 平行（同一直線の隣）はここで落ちる
					// 端そのもの（丸め誤差ぶん）で交わっている場合だけが対象。半壁厚の許容
					// （wallPointAtEnd）ではなく厳密に見る——相手の外面まで伸びている立上りを
					// さらに伸ばさないため。
					if (std::hypot(point.x - tip.x, point.y - tip.y) > kWallEndpointTol)
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

	std::vector<WallCommand> applyWallOpenings(const std::vector<WallCommand>& walls,
											   const std::vector<WallOpening>& openings,
											   double slabTopAbs, double beamTopAbs)
	{
		std::vector<WallCommand> result = walls;
		for (const WallOpening& opening : openings)
		{
			const std::size_t index = findOpeningWall(result, opening);
			if (index >= result.size())
				continue; // 乗る立上りが無い開口は無視する
			const std::vector<WallCommand> carved =
				carveWallOpening(result[index], opening, slabTopAbs, beamTopAbs);
			result.erase(result.begin() + static_cast<std::ptrdiff_t>(index));
			result.insert(result.begin() + static_cast<std::ptrdiff_t>(index), carved.begin(),
						  carved.end());
		}
		return result;
	}

	std::vector<core::WallJoinCommand> buildWallJoinCommands(const std::vector<WallCommand>& walls)
	{
		std::vector<core::WallJoinCommand> commands;
		for (const Junction& junction : wallJunctions(walls))
		{
			const std::vector<std::size_t>& indices = junction.walls;
			if (indices.size() < 2)
				continue;
			const Vec2 point = junction.point;

			// 交点が各壁芯の端点か内部か・天端高さ・ピック点の寄せ量（交点に集まる立上りの
			// 最大壁厚。相手壁の footprint＝半壁厚を確実に越えて残す側へ寄せる）。
			std::map<std::size_t, bool> atEnd;
			std::map<std::size_t, double> tops;
			double pickOffset = 0.0;
			for (const std::size_t index : indices)
			{
				atEnd[index] = wallPointAtEnd(walls[index], point);
				tops[index] = wallTop(walls[index]);
				pickOffset = std::max(pickOffset, walls[index].thickness);
			}

			std::vector<std::size_t> interiors;
			std::vector<std::size_t> ends;
			for (const std::size_t index : indices)
			{
				if (atEnd.at(index))
					ends.push_back(index);
				else
					interiors.push_back(index);
			}

			// 天端高さ降順・添字昇順（バックボーンに最も高い立上りを選ぶ）。
			const auto byHeight = [&tops](std::size_t lhs, std::size_t rhs)
			{
				if (tops.at(lhs) != tops.at(rhs))
					return tops.at(lhs) > tops.at(rhs);
				return lhs < rhs;
			};

			const auto makeCommand =
				[&](std::size_t a, std::size_t b, core::WallJoinType type, bool capped)
			{
				core::WallJoinCommand cmd;
				cmd.a = a;
				cmd.b = b;
				cmd.point = point;
				// ピック点は**種別に関係なく**「残す側」へ寄せた壁芯上の点にする（Python 版
				// `_kept_side_pick` と同じ。X 結合では VW が壁を詰めないので寄せは無害、という
				// のが Python 版の実証済みの結論）。
				//
				// 一度「X 結合だけ交点そのものを渡す」ことを試した（交差では四方すべてが残るので
				// 「残す側」に意味が無く、片側を指すせいで VW が交点で壁を切って別の立上りを
				// 作っているのではないかと疑った）が、**実機で症状は変わらなかった**ので、
				// 根拠の無い Python 版との差異を残さないためにこちらへ戻した（ROADMAP.md M10）。
				cmd.pickA = keptSidePick(walls[a], point, pickOffset);
				cmd.pickB = keptSidePick(walls[b], point, pickOffset);
				cmd.joinType = type;
				cmd.capped = capped;
				return cmd;
			};

			// L / X 結合。天端高さが違えば**低いほうを a**（高いほうへ結合）にして端部を
			// 閉じる。同じ高さならルート（root）を a にして閉じない。
			//
			// **ただし X 結合（交差結合）だけは常に `other` を a にする。** VW の X 結合は
			// 「**1 本目の壁を交点で 2 本に分割し、2 本目（load bearing wall）へ結合する**」
			// という仕様（VW ヘルプ「X wall joins」: 両壁の長さは変わらない＝すでに交差して
			// いる必要があり、1 本目が 2 本の壁に分かれる）。したがって a に渡した壁が分割され、
			// b に渡した壁が丸ごと残る。**丸ごと残すべきはバックボーン**（天端が最も高い通し壁）
			// なので、a＝other・b＝root にする。root は最も高いので、天端が違うときの
			// 「低いほうを a」も同時に満たす（ROADMAP.md M10）。
			const auto makeLX = [&](std::size_t other, std::size_t root, core::WallJoinType type)
			{
				const bool capped = std::abs(tops.at(other) - tops.at(root)) > kWallMergeDistTol;
				std::size_t a = root;
				std::size_t b = other;
				if (type == core::WallJoinType::X || (capped && tops.at(other) < tops.at(root)))
				{
					a = other;
					b = root;
				}
				return makeCommand(a, b, type, capped);
			};

			// T 結合。stem（端点側＝延長される壁）を a、through（通し壁）を b にする。
			// **2 本目以降の stem を Auto にする判定は pushJoin 側**（実際に出す命令だけを
			// 数えるため。同一直線で落とす stem を数えてしまうと 1 本目が Auto になる）。
			const auto makeT = [&](std::size_t stem, std::size_t through)
			{
				const bool capped = std::abs(tops.at(stem) - tops.at(through)) > kWallMergeDistTol;
				return makeCommand(stem, through, core::WallJoinType::T, capped);
			};

			// stem が T 結合する通し壁を選ぶ（最も直交する壁。同点なら天端が高いほう、
			// さらに同点なら添字の小さいほう）。
			const auto pickThrough =
				[&](std::size_t stem, const std::vector<std::size_t>& candidates)
			{
				double sdx = 0.0;
				double sdy = 0.0;
				double slen = 0.0;
				wallDirection(walls[stem], sdx, sdy, slen);
				std::size_t best = candidates.front();
				double bestPerp = -1.0;
				for (const std::size_t candidate : candidates)
				{
					double cdx = 0.0;
					double cdy = 0.0;
					double clen = 0.0;
					wallDirection(walls[candidate], cdx, cdy, clen);
					const double perp = (slen > 0.0 && clen > 0.0)
											? std::abs((sdx * cdy) - (sdy * cdx)) / (slen * clen)
											: 0.0;
					if (bestPerp < 0.0 || perp > bestPerp ||
						(perp == bestPerp &&
						 (tops.at(candidate) > tops.at(best) ||
						  (tops.at(candidate) == tops.at(best) && candidate < best))))
					{
						best = candidate;
						bestPerp = perp;
					}
				}
				return best;
			};

			// **同一直線上の 2 本には結合を出さない。** 平行な立上りは wallIntersection が
			// 交点を作らないので普段は候補にならないが、**第 3 の壁が作った交点**には同じ
			// ジャンクションとして入ってくる（例: 一直線に並ぶ 2 本の突き合わせ位置を別の
			// 立上りが横切る）。そこへ L / T 結合を出すと、コーナーにならないので VW が
			// 拒否する——実データで「壁結合: 1 件を VW が拒否しました (T:1): (6370,1820)」の
			// 正体がこれだった（ROADMAP.md M10）。同一直線上の突き合わせは結合ではなく
			// 端部のキャップ（applyWallCaps の collinearAbutment）で 1 本に見せる。
			//
			// あわせて、**同じ通し壁の同じ交点へ 2 本目以降の stem が取り付く T 結合は Auto へ
			// 落とす**。明示的な T では実機で **JoinWalls が両方 true を返すのに、図面では先に
			// 実行した 1 本だけが結合されて見えた**（拒否件数は増えない）。通し壁側のピック点を
			// 反対側へ寄せて区別させる案は実機で描画が変わらず外れたので、`kAutoWallJoin`
			// （ピック点を無視して VW に種別を判断させる）で通す。1 本目は従来どおり T なので、
			// **交点に stem が 1 本だけの既存の T 結合の引数は変わらない**（ROADMAP.md M10）。
			// 数えるのは**実際に出した命令だけ**（同一直線で落とす stem を数えると 1 本目が
			// Auto になってしまう）。
			std::map<std::size_t, std::size_t> stemsPerThrough;
			const auto pushJoin =
				[&](std::vector<core::WallJoinCommand>& into, core::WallJoinCommand cmd)
			{
				double lo = 0.0;
				double hi = 0.0;
				if (wallsOnSameLine(walls[cmd.a], walls[cmd.b], lo, hi))
					return;
				if (cmd.joinType == core::WallJoinType::T && stemsPerThrough[cmd.b]++ > 0)
					cmd.joinType = core::WallJoinType::Auto;
				into.push_back(cmd);
			};

			std::vector<core::WallJoinCommand> junctionCommands;
			if (!interiors.empty())
			{
				// 交点に通し壁（内部で交わる壁）がある: 天端の最も高い通し壁をバックボーンに、
				// 他の通し壁を X 結合、端点で突き当たる壁を T 結合にする。
				std::vector<std::size_t> ordered = interiors;
				std::ranges::sort(ordered, byHeight);
				const std::size_t root = ordered.front();
				for (const std::size_t other : ordered | std::views::drop(1))
					pushJoin(junctionCommands, makeLX(other, root, core::WallJoinType::X));
				std::vector<std::size_t> stems = ends;
				std::ranges::sort(stems, byHeight);
				for (const std::size_t stem : stems)
					pushJoin(junctionCommands, makeT(stem, pickThrough(stem, interiors)));
			}
			else
			{
				// 通し壁の無い端点コーナー: 天端高さ降順ではじめの 2 本を L、それ以降を T
				// （はじめの 2 本＝バックボーンへ突き当てる）。
				std::vector<std::size_t> ordered = ends;
				std::ranges::sort(ordered, byHeight);

				// **ただし同一直線の 2 本がこの交点に集まっているなら、そこにコーナーは無い。**
				// 上端が同じで下端だけ違って統合できなかった立上りが、直交する立上りの位置で
				// 突き合わさるとこうなる（底盤厚が違う箇所。extendDeeperCollinearEnds 参照）。
				// このとき「天端降順の先頭 2 本を L」にすると同一直線の 2 本が選ばれてしまい、
				// コーナーにならないので VW が拒否する——実データの拒否 1 件 (6370,1820)。
				//
				// **深いほう（下端が低い。同値なら添字が小さいほう）を通し壁にして、直交する
				// 立上りをそこへ T 結合する。** その通し壁は extendDeeperCollinearEnds が相手の
				// 半壁厚だけ伸ばして交点を越えているので、T 結合が成立する。同一直線の隣とは
				// 結合せず、端部のキャップ（applyWallCaps）で 1 本に見せる。
				std::vector<std::size_t> collinearGroup;
				for (const std::size_t index : ordered)
				{
					const auto sameLineAsIndex = [&](std::size_t other)
					{
						double lo = 0.0;
						double hi = 0.0;
						return other != index &&
							   wallsOnSameLine(walls[index], walls[other], lo, hi);
					};
					if (std::ranges::any_of(ordered, sameLineAsIndex))
						collinearGroup.push_back(index);
				}

				if (!collinearGroup.empty())
				{
					std::size_t through = collinearGroup.front();
					for (const std::size_t index : collinearGroup)
					{
						const double mine = walls[index].bottomBound.offset;
						const double best = walls[through].bottomBound.offset;
						if (mine < best - kWallMergeDistTol ||
							(std::abs(mine - best) <= kWallMergeDistTol && index < through))
							through = index;
					}
					for (const std::size_t stem : ordered)
					{
						if (stem == through)
							continue;
						// 同一直線の隣は pushJoin が落とす（結合ではなくキャップで見せる）。
						pushJoin(junctionCommands, makeT(stem, through));
					}
				}
				else
				{
					const std::size_t root = ordered.front();
					if (ordered.size() >= 2)
						pushJoin(junctionCommands, makeLX(ordered[1], root, core::WallJoinType::L));
					const std::vector<std::size_t> backbone(ordered.begin(), ordered.begin() + 2);
					for (const std::size_t stem : ordered | std::views::drop(2))
						pushJoin(junctionCommands, makeT(stem, pickThrough(stem, backbone)));
				}
			}

			// capped=false（天端の高い立上りどうし）を先に、capped=true を後に並べる
			// （高い者どうしを先に繋いでから低い者を突き当てる）。安定ソートで同順の
			// 並びを保ち、入力順に対して決定的にする。
			std::ranges::stable_sort(
				junctionCommands,
				[](const core::WallJoinCommand& lhs, const core::WallJoinCommand& rhs)
				{ return static_cast<int>(lhs.capped) < static_cast<int>(rhs.capped); });
			commands.insert(commands.end(), junctionCommands.begin(), junctionCommands.end());
		}

		// **X 結合（交差結合）はすべて最後に回す。** VW の X 結合は 1 本目の壁を交点で 2 本に
		// 分割する仕様なので（makeLX の doc コメント）、分割された壁の**ハンドルが古くなる**。
		// 描画側は「命令インデックス → 壁ハンドル」の対応表で壁を引くため、X 結合より後に
		// その壁を使う結合が残っていると、**分割された片方だけを相手にしてしまう**。実機で
		// まさにこれが起きた: 交差する横の立上りは両端に T 結合を持ち、X 結合（交点）→
		// T 結合（端）の順に実行されたため、**分割後の半分の壁が T 結合されて全長の壁は
		// 結合されないまま**になった（ROADMAP.md M10）。X を最後に回せば、分割の時点で
		// 他の結合はすべて全長の壁に対して済んでいる。
		//
		// 安定ソートなので X 以外の並び（ジャンクション順・capped 順）は変わらない。
		std::ranges::stable_sort(
			commands,
			[](const core::WallJoinCommand& lhs, const core::WallJoinCommand& rhs)
			{
				const int lx = (lhs.joinType == core::WallJoinType::X) ? 1 : 0;
				const int rx = (rhs.joinType == core::WallJoinType::X) ? 1 : 0;
				return lx < rx;
			});
		return commands;
	}

	std::vector<WallCommand> buildWallCommands(Context& context,
											   const std::vector<ColumnCommand>& columns)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo>& stories = context.stories();
		if (stories.empty())
			return {}; // 上端のバインド先（1 階の横架材天端）が決まらない

		// 立上りの上端は 1 階（＝最下階の FL ストーリ）の横架材天端へバインドする。
		const StoryInfo& first = stories.front();
		const double beamOffset =
			first.isTop ? resolveBeamTopOffset(context, first.id) : first.beamOffset;
		const double beamTopAbs = first.elevation + beamOffset;

		const Vec2 center = context.gridCenter();

		std::vector<WallCommand> commands;
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

			WallCommand cmd;
			cmd.layer = kLayerFoundationWall;
			cmd.drawClass = CLASS_FOUNDATION_WALL;
			cmd.start = start;
			cmd.end = end;
			cmd.thickness = thickness;
			// 壁スタイルは壁厚ごとに 1 つ（構成はコンクリート 1 層＝壁厚）。描画側はこの名前で
			// **新しいスタイルを作る**（既存リソースには触れない。draw/Footing.cpp 参照）。
			cmd.styleName = foundationWallStyleName(thickness);
			cmd.components = foundationWallComponents(thickness);
			// 下端は IFC 実形状のまま（呑み込みはしない。parse/Footing.h「下端は IFC 実形状の
			// まま」参照）。
			cmd.bottomBound = StoryBoundCommand{0, kLevelGL, bottomAbs};
			cmd.topBound = StoryBoundCommand{1, kLevelBeamTop, topAbs - beamTopAbs};
			commands.push_back(std::move(cmd));
		}
		// 統合 → 自由端の延長 → 深いほうの延長 → 人通口の当てはめ（ROADMAP.md M10）。
		// **人通口は統合・延長の後**に当てはめるので、開口を跨いで統合された立上りも開口位置で
		// 正しく分割され、開口境界の端は実寸法のまま（延長しない）になる。深いほうの延長は
		// 自由端の延長の**後**（自由端ではない端＝直交する立上りの壁芯で止まっている端が対象で、
		// 自由端の延長とは対象が重ならない。parse/Footing.h 参照）。
		std::vector<WallCommand> walls =
			extendDeeperCollinearEnds(extendFreeWallEnds(mergeWallCommands(commands), columns));
		const std::vector<WallOpening> openings = collectWallOpenings(model, center);
		if (openings.empty())
			return walls;

		// 開口下端が底盤天端以下なら「その区間に立上りは生じない」。底盤が 1 枚も無い基礎
		// （立上りだけ）では比較相手が無いので、常に「開口下端の方が高い」＝切り下げに
		// なるよう十分小さい値を使う（Python 版が -inf を渡すのと同じ意図）。
		double slabTopAbs = -std::numeric_limits<double>::infinity();
		double resolved = 0.0;
		if (resolveSlabTopElevation(model, resolved))
			slabTopAbs = resolved;
		return applyWallOpenings(walls, openings, slabTopAbs, beamTopAbs);
	}

	std::vector<WallCommand> buildWallCommands(const Model& model)
	{
		Context context(model);
		return buildWallCommands(context, context.columns());
	}

	std::vector<SlabCommand> mergeSlabCommands(const std::vector<SlabCommand>& slabs)
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
		std::map<std::size_t, std::vector<SlabCommand>> mergedAt;
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

				SlabCommand merged = slabs[comp.front()];
				merged.boundary.clear();
				merged.boundary.reserve(outer.front().size());
				for (const Pt2& p : outer.front())
					merged.boundary.push_back(Vec2{p.first, p.second});
				mergedAt[comp.front()].push_back(std::move(merged));
				dropped.insert(comp.begin(), comp.end());
			}
		}

		std::vector<SlabCommand> result;
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

	std::vector<SlabCommand> alignSlabsToWallFaces(const std::vector<SlabCommand>& slabs,
												   const std::vector<WallCommand>& walls)
	{
		if (walls.empty())
			return slabs;

		std::vector<SlabCommand> result;
		result.reserve(slabs.size());
		for (const SlabCommand& slab : slabs)
		{
			std::vector<Vec2> boundary;
			if (!offsetBoundaryToWalls(slab.boundary, walls, boundary))
			{
				result.push_back(slab);
				continue;
			}
			SlabCommand adjusted = slab;
			adjusted.boundary = std::move(boundary);
			result.push_back(std::move(adjusted));
		}
		return result;
	}

	void applyWallCaps(std::vector<WallCommand>& walls,
					   const std::vector<core::WallJoinCommand>& joins)
	{
		// 既定は「閉じる」。閉じない結合（capped=false）が当たった端だけを開ける。
		for (WallCommand& wall : walls)
		{
			wall.capStart = true;
			wall.capEnd = true;
		}

		// 交点が壁芯のどちら側の端に当たるかは、端点までの距離が近いほうで決める
		// （交点が壁の内部にある通し壁は端部を持たないので触らない）。
		const auto openEnd = [&walls](std::size_t index, const Vec2& point)
		{
			if (index >= walls.size())
				return;
			WallCommand& wall = walls[index];
			if (!wallPointAtEnd(wall, point))
				return; // 内部で交わる通し壁の端部は、この交点では閉じ方が決まらない
			const double toStart = std::hypot(wall.start.x - point.x, wall.start.y - point.y);
			const double toEnd = std::hypot(wall.end.x - point.x, wall.end.y - point.y);
			if (toStart <= toEnd)
				wall.capStart = false;
			else
				wall.capEnd = false;
		};

		for (const core::WallJoinCommand& join : joins)
		{
			if (join.capped)
				continue; // 天端の違う相手との結合は端部を閉じたままにする
			openEnd(join.a, join.point);
			openEnd(join.b, join.point);
		}

		// **同一直線上の突き合わせ**（交点判定に掛からない平行な隣）も、天端が同じなら
		// コンクリートは連続しているので端部を閉じない。統合できなかった隣——上端／下端の
		// 違う立上り——のうち、**下端だけが違うもの**は平面では 1 本に見えるべきで、天端の
		// 違うものは段差が実在するので閉じたままにする（結合の capped と同じ判断）。
		const std::size_t count = walls.size();
		for (std::size_t i = 0; i < count; ++i)
		{
			for (std::size_t j = 0; j < count; ++j)
			{
				if (i == j || walls[i].layer != walls[j].layer)
					continue;
				if (std::abs(wallTop(walls[i]) - wallTop(walls[j])) > kWallMergeDistTol)
					continue; // 天端が違う＝段差があるので閉じる
				bool atStart = false;
				bool atEnd = false;
				collinearAbutment(walls[i], walls[j], atStart, atEnd);
				if (atStart)
					walls[i].capStart = false;
				if (atEnd)
					walls[i].capEnd = false;
			}
		}
	}

	std::vector<core::ModifierCommand> buildGroundBeamModifiers(const Model& model,
																const Vec2& center)
	{
		std::vector<core::ModifierCommand> modifiers;
		for (const int id : model.byType("IFCFOOTING"))
		{
			const Entity* element = model.entity(id);
			if (element == nullptr || !isGroundBeam(entityName(*element)))
				continue;
			WorldSolid solid;
			if (!resolveElementWorldSolid(model, element, solid))
				continue;
			core::ModifierCommand modifier;
			if (!groundBeamModifier(solid, center, modifier))
				continue;
			modifiers.push_back(std::move(modifier));
		}
		return mergeGroundBeamModifiers(modifiers);
	}

	std::vector<core::ModifierCommand>
	mergeGroundBeamModifiers(const std::vector<core::ModifierCommand>& modifiers)
	{
		// グループキーごとにまとめる（グループの並びは最初に現れた順＝入力順に決定的）。
		std::map<GroundBeamKey, std::size_t> index;
		std::vector<std::vector<core::ModifierCommand>> groups;
		for (const core::ModifierCommand& modifier : modifiers)
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

		std::vector<core::ModifierCommand> result;
		for (const std::vector<core::ModifierCommand>& group : groups)
		{
			for (core::ModifierCommand& merged : mergeGroundBeamGroup(group))
				result.push_back(std::move(merged));
		}
		return result;
	}

	std::vector<Vec2> modifierFootprint(const core::ModifierCommand& modifier)
	{
		if (modifier.profile.empty())
			return {};
		const Vec2 axis = groundBeamAxisDir(modifier);
		const Vec2 width{-axis.y, axis.x}; // 幅軸 w（groundBeamModifier の取り方と一致）
		double uLo = modifier.profile.front().x;
		double uHi = modifier.profile.front().x;
		for (const Vec2& p : modifier.profile)
		{
			uLo = std::min(uLo, p.x);
			uHi = std::max(uHi, p.x);
		}
		const Vec2 start{modifier.origin.x, modifier.origin.y};
		const Vec2 end{start.x + (axis.x * modifier.depth), start.y + (axis.y * modifier.depth)};
		return {Vec2{start.x + (width.x * uLo), start.y + (width.y * uLo)},
				Vec2{start.x + (width.x * uHi), start.y + (width.y * uHi)},
				Vec2{end.x + (width.x * uHi), end.y + (width.y * uHi)},
				Vec2{end.x + (width.x * uLo), end.y + (width.y * uLo)}};
	}

	core::ModifierCommand extendGroundBeamEnds(const core::ModifierCommand& modifier,
											   const std::vector<WallCommand>& walls,
											   const std::vector<core::ModifierCommand>& others)
	{
		core::ModifierCommand result = modifier;
		if (modifier.profile.empty() || modifier.depth <= 0.0)
			return result;

		const Vec2 axis = groundBeamAxisDir(modifier);
		const Vec2 start{modifier.origin.x, modifier.origin.y};
		const Vec2 end{start.x + (axis.x * modifier.depth), start.y + (axis.y * modifier.depth)};

		// 端 tip から外向き (dx, dy) へ**別の地中梁が続いているか**。続いているならその端は
		// 伸ばさない（伸ばすと隣の梁へ食い込む。立上りの collinearAbutment と同じ考え方で、
		// 実機では 75mm ぶん隣の梁と重なった。ROADMAP.md M10）。
		const auto abuttingBeam = [&](const Vec2& tip, double dx, double dy)
		{
			const auto continuesPast = [&](const core::ModifierCommand& other)
			{
				if (other.depth <= 0.0)
					return false;
				const Vec2 otherAxis = groundBeamAxisDir(other);
				const Vec2 otherStart{other.origin.x, other.origin.y};
				const Vec2 otherEnd{otherStart.x + (otherAxis.x * other.depth),
									otherStart.y + (otherAxis.y * other.depth)};
				// 自分自身は飛ばす（統合済みなので同じ軸線・同じ区間の重複は無い）。
				if (samePoint(otherStart, start) && samePoint(otherEnd, end))
					return false;
				// tip を相手の軸線へ射影する。軸線上に無い相手は関係ない。
				const Vec2 delta = tip - otherStart;
				const double along = (delta.x * otherAxis.x) + (delta.y * otherAxis.y);
				const double across = std::abs((delta.x * -otherAxis.y) + (delta.y * otherAxis.x));
				if (across > kGroundBeamMergeTol)
					return false;
				// 直交する相手は「続いている」のではなく取り合う相手。
				const double heading = (dx * otherAxis.x) + (dy * otherAxis.y);
				if (std::abs(heading) <= kGroundBeamMergeAngleTol)
					return false;
				// tip から外向きへ進んだ先が相手の区間 [0, depth] に入っているか
				// （相手が tip を越えて続いている＝この端は伸ばさない）。
				return (heading > 0.0) ? (along > -kGroundBeamMergeTol &&
										  along < other.depth - kGroundBeamMergeTol)
									   : (along > kGroundBeamMergeTol &&
										  along < other.depth + kGroundBeamMergeTol);
			};
			return std::ranges::any_of(others, continuesPast);
		};

		// 端 tip が**直交する立上りの壁芯上**にあるなら、その立上りの半壁厚を返す（無ければ 0）。
		// 複数見つかったら最も厚い立上りに合わせる。
		const auto reachAtWall = [&walls, &axis](const Vec2& tip)
		{
			double reach = 0.0;
			for (const WallCommand& wall : walls)
			{
				const Vec2 delta = wall.end - wall.start;
				const double length = std::hypot(delta.x, delta.y);
				if (length <= 0.0)
					continue;
				const Vec2 dir{delta.x / length, delta.y / length};
				// 梁と平行な立上りは「越える相手」ではない（沿って走っている）。
				if (std::abs((dir.x * axis.y) - (dir.y * axis.x)) <= kGroundBeamMergeAngleTol)
					continue;
				// tip が壁芯の線上（直交距離）かつ区間内にあるか。
				const Vec2 offset = tip - wall.start;
				const double across = std::abs((offset.x * -dir.y) + (offset.y * dir.x));
				const double along = (offset.x * dir.x) + (offset.y * dir.y);
				if (across > kGroundBeamMergeTol || along < -kGroundBeamMergeTol ||
					along > length + kGroundBeamMergeTol)
					continue;
				reach = std::max(reach, wall.thickness / 2.0);
			}
			return reach;
		};

		const double back = abuttingBeam(start, -axis.x, -axis.y) ? 0.0 : reachAtWall(start);
		const double forward = abuttingBeam(end, axis.x, axis.y) ? 0.0 : reachAtWall(end);
		if (back <= 0.0 && forward <= 0.0)
			return result;

		result.origin =
			core::Vec3{start.x - (axis.x * back), start.y - (axis.y * back), modifier.origin.z};
		result.depth = modifier.depth + back + forward;
		return result;
	}

	void attachGroundBeamModifiers(std::vector<SlabCommand>& slabs,
								   const std::vector<core::ModifierCommand>& modifiers,
								   const std::vector<WallCommand>& walls)
	{
		if (slabs.empty())
			return; // 底盤が 1 枚も無ければ付けられない（地中梁だけの基礎は稀）

		// 底盤の外形は pointInPoly（丸めた頂点列）で判定する。判定用の写しは 1 回だけ作る。
		std::vector<std::vector<Pt2>> polys;
		polys.reserve(slabs.size());
		for (const SlabCommand& slab : slabs)
		{
			std::vector<Pt2> ring;
			ring.reserve(slab.boundary.size());
			for (const Vec2& p : slab.boundary)
				ring.emplace_back(p.x, p.y);
			polys.push_back(std::move(ring));
		}

		for (const core::ModifierCommand& modifier : modifiers)
		{
			const std::vector<Vec2> footprintPoly = modifierFootprint(modifier);
			if (footprintPoly.empty())
				continue;
			const std::vector<Vec2> samples = footprintSamples(footprintPoly);

			// 代表点が外形内に入る数が最大の底盤へ振り分ける（同数なら添字の小さいほう）。
			std::size_t best = 0;
			std::ptrdiff_t bestCount = 0;
			for (std::size_t i = 0; i < polys.size(); ++i)
			{
				const auto count =
					std::ranges::count_if(samples, [&polys, i](const Vec2& sample)
										  { return pointInPoly(sample.x, sample.y, polys[i]); });
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
				const Vec2 centroid = polygonCentroid(footprintPoly);
				double bestDistance = 0.0;
				for (std::size_t i = 0; i < slabs.size(); ++i)
				{
					const Vec2 slabCentroid = polygonCentroid(slabs[i].boundary);
					const double distance =
						std::hypot(slabCentroid.x - centroid.x, slabCentroid.y - centroid.y);
					if (i == 0 || distance < bestDistance)
					{
						best = i;
						bestDistance = distance;
					}
				}
			}
			slabs[best].modifiers.push_back(extendGroundBeamEnds(modifier, walls, modifiers));
		}
	}

	std::vector<SlabCommand> buildSlabCommands(Context& context,
											   const std::vector<WallCommand>& walls)
	{
		const Model& model = context.model();
		double slabTopAbs = 0.0;
		if (!resolveSlabTopElevation(model, slabTopAbs))
			slabTopAbs = 0.0;

		const Vec2 center = context.gridCenter();

		std::vector<SlabCommand> commands;
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
			// スラブスタイルのコンクリート厚は整数 mm に丸める（同厚の底盤が別スタイルへ
			// 散らないようにする。Python 版 float(round(thickness)) と同値）。
			const double concrete = std::round(thickness);

			std::vector<Vec2> boundary = footprint(solid);
			for (Vec2& p : boundary)
			{
				p.x -= center.x;
				p.y -= center.y;
			}

			SlabCommand cmd;
			cmd.layer = kLayerFoundationSlab;
			cmd.drawClass = CLASS_FOUNDATION_SLAB;
			cmd.boundary = std::move(boundary);
			cmd.styleName = foundationSlabStyleName(concrete);
			cmd.components = foundationSlabComponents(concrete);
			cmd.datum = core::SlabDatum::Top; // 基準面はコンクリート天端
			cmd.thickness = concrete;
			cmd.elevation = topAbs;
			cmd.bound = StoryBoundCommand{0, kLevelSlabTop, topAbs - slabTopAbs};
			commands.push_back(std::move(cmd));
		}
		// 統合 → 外面合わせ → 地中梁の振り分け（ROADMAP.md M10）。地中梁は**単独のスラブ命令に
		// せず**、外形の確定した底盤の modifiers へ付ける（台形断面は単一のスラブで描けない）。
		std::vector<SlabCommand> slabs = alignSlabsToWallFaces(mergeSlabCommands(commands), walls);
		attachGroundBeamModifiers(slabs, buildGroundBeamModifiers(model, center), walls);
		return slabs;
	}

	std::vector<SlabCommand> buildSlabCommands(const Model& model)
	{
		Context context(model);
		return buildSlabCommands(context, context.walls());
	}
} // namespace HomeskzIfcImport::parse
