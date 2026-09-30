//
//	parse/Dimension.cpp
//
//	寸法の解析（docs/DEV-NOTES.md M31）。【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	寸法は命令セットだけから導出する（parse/Dimension.h 冒頭）。並びは入力の命令の並びに依らず、
//	測点を昇順にまとめ、列を「外周（X を測る列→Y を測る列）→ 立上り・横架材に沿う列（東西の
//	通り→南北の通り、それぞれ座標の昇順）」の順に出すので決定的になる。
//

#include "parse/Dimension.h"
#include "core/Document.h"
#include "parse/Section.h"
#include "parse/Story.h"
#include "parse/Tag.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		using core::DimensionAxis;
		using core::DimensionChainCommand;

		DimensionChainCommand makeChain(DimensionAxis axis, std::vector<double> stops, double base,
										int side, int tier)
		{
			DimensionChainCommand chain;
			chain.axis = axis;
			chain.stops = std::move(stops);
			chain.base = base;
			chain.side = side;
			chain.tier = tier;
			return chain;
		}

		// 線分が東西（y 一定）に走るか・南北（x 一定）に走るか。どちらでもなければ斜め。
		bool runsEastWest(const core::Vec2& start, const core::Vec2& end)
		{
			return std::abs(end.y - start.y) <= kDimensionAxisTol &&
				   std::abs(end.x - start.x) > kDimensionAxisTol;
		}

		bool runsNorthSouth(const core::Vec2& start, const core::Vec2& end)
		{
			return std::abs(end.x - start.x) <= kDimensionAxisTol &&
				   std::abs(end.y - start.y) > kDimensionAxisTol;
		}

		bool onLayers(const std::vector<std::string>& layers, const std::string& layer)
		{
			return std::ranges::find(layers, layer) != layers.end();
		}

		// 2 つの測点の列が（許容の範囲で）同じか。同じ列を 2 段に重ねて出さないために見る。
		bool sameStops(const std::vector<double>& a, const std::vector<double>& b)
		{
			if (a.size() != b.size())
				return false;
			for (std::size_t i = 0; i < a.size(); ++i)
			{
				if (std::abs(a[i] - b[i]) > kDimensionMergeTol)
					return false;
			}
			return true;
		}

		bool nearAny(const std::vector<double>& values, double value)
		{
			return std::ranges::any_of(values, [value](double v)
									   { return std::abs(v - value) <= kDimensionMergeTol; });
		}

		// 1 つの向き（X を測る列か Y を測る列）の外周の列を積む（ヘッダ冒頭「伏図の外周の
		// 列」）。detail 側に部材の位置の列を置き、全長は overallOnDetailSide なら同じ側の
		// 1 つ外の段、そうでなければ反対側（overallBase / overallSide）の最も内側の段へ置く。
		void addPerimeterAxis(std::vector<DimensionChainCommand>& out, DimensionAxis axis,
							  const std::vector<double>& elements, const std::vector<double>& grids,
							  double detailBase, int detailSide, bool overallOnDetailSide,
							  double overallBase, int overallSide, int firstTier)
		{
			const std::vector<double> detail = unionStops(mergeStops(grids), elements);
			if (detail.size() < 2)
				return;
			out.push_back(makeChain(axis, detail, detailBase, detailSide, firstTier));
			const std::vector<double> overall{detail.front(), detail.back()};
			if (!overallOnDetailSide)
				out.push_back(makeChain(axis, overall, overallBase, overallSide, firstTier));
			else if (detail.size() >= 3)
				// 測点が 2 つなら部材の位置の列がそのまま全長になる（同じ列を 2 段重ねない）。
				out.push_back(makeChain(axis, overall, detailBase, detailSide, firstTier + 1));
		}

		// 通りに乗る線材 1 本（基礎伏図の立上り・床伏図／小屋伏図の横架材）。thickness は
		// 立上りの厚み・横架材の幅（通りに乗る点・取り合いを拾う幅）。
		struct LineSegment
		{
			core::Vec2 start;
			core::Vec2 end;
			double thickness = 0.0;
		};

		// 直線に乗る線材の群（基礎伏図の立上りの「通り」1 本。床伏図・小屋伏図では横架材）。
		struct WallLine
		{
			double coord = 0.0; // 直交座標（東西の通りなら Y）
			std::vector<std::pair<double, double>> spans; // 通りに沿った区間（昇順）
			double halfThickness = 0.0; // 群の中で最も厚い立上りの半分
		};

		// 立上りを東西の通り（eastWest=true）または南北の通りへまとめる。座標の昇順。
		std::vector<WallLine> collectWallLines(const std::vector<LineSegment>& walls, bool eastWest)
		{
			std::vector<WallLine> lines;
			std::vector<const LineSegment*> picked;
			for (const LineSegment& wall : walls)
			{
				if (eastWest ? runsEastWest(wall.start, wall.end)
							 : runsNorthSouth(wall.start, wall.end))
					picked.push_back(&wall);
			}
			const auto coordOf = [eastWest](const LineSegment& wall) {
				return eastWest ? (wall.start.y + wall.end.y) / 2.0
								: (wall.start.x + wall.end.x) / 2.0;
			};
			const auto spanOf = [eastWest](const LineSegment& wall)
			{
				const double a = eastWest ? wall.start.x : wall.start.y;
				const double b = eastWest ? wall.end.x : wall.end.y;
				return std::pair<double, double>{std::min(a, b), std::max(a, b)};
			};
			// 座標 → 区間の順に並べてから群にする（入力の並びに依らない）。
			std::ranges::sort(picked,
							  [&](const LineSegment* a, const LineSegment* b)
							  {
								  const double ca = coordOf(*a);
								  const double cb = coordOf(*b);
								  if (ca != cb)
									  return ca < cb;
								  return spanOf(*a) < spanOf(*b);
							  });
			for (const LineSegment* wall : picked)
			{
				const double coord = coordOf(*wall);
				if (lines.empty() || std::abs(coord - lines.back().coord) > kDimensionMergeTol)
				{
					lines.push_back(WallLine{});
					lines.back().coord = coord;
				}
				WallLine& line = lines.back();
				line.spans.push_back(spanOf(*wall));
				line.halfThickness = std::max(line.halfThickness, wall->thickness / 2.0);
			}
			for (WallLine& line : lines)
				std::ranges::sort(line.spans);
			return lines;
		}

		// 立上りの通り 1 本の測点（ヘッダ冒頭「基礎伏図は立上りに沿う列も持つ」）。
		struct WallLineStops
		{
			const WallLine* line = nullptr;
			std::vector<double> cores; // 端（芯で押さえ直したもの）と直交する立上りの芯
			std::vector<double> all; // cores ＋ その通りに乗る点（アンカーボルト・柱）
			// 両端（cores の最初と最後）が何とも取り合わない自由端か。自由端を持つ一続き
			// （半島状・独立した立上り）は長さも押さえる。
			bool freeFront = false;
			bool freeBack = false;
			std::vector<double> points; // all のうち、その通りに乗る点（アンカーボルト・柱）
		};

		// 直交する立上りが通りと取り合う位置（その立上りの芯）と、その立上りの厚みの半分。
		struct Junction
		{
			double coord = 0.0;
			double halfThickness = 0.0;
		};

		bool inSpans(const std::vector<std::pair<double, double>>& spans, double value,
					 double margin)
		{
			return std::ranges::any_of(
				spans, [value, margin](const std::pair<double, double>& span)
				{ return value >= span.first - margin && value <= span.second + margin; });
		}

		// line に取り合う（突き当たる・交わる・隅で出会う）直交する立上りの芯。直交する立上りの
		// 区間が line の厚みの中まで届き、かつ line の区間がその立上りの芯まで届くもの。
		std::vector<Junction> junctionsOf(const WallLine& line, const std::vector<WallLine>& others)
		{
			std::vector<Junction> out;
			for (const WallLine& other : others)
			{
				const bool reaches = std::ranges::any_of(
					other.spans,
					[&line](const std::pair<double, double>& span)
					{
						return span.first - kDimensionMergeTol <= line.coord + line.halfThickness &&
							   span.second + kDimensionMergeTol >= line.coord - line.halfThickness;
					});
				if (reaches && inSpans(line.spans, other.coord, other.halfThickness))
					out.push_back(Junction{other.coord, other.halfThickness});
			}
			return out;
		}

		// 通り 1 本の測点を決める。
		//   * 直交する立上りと取り合うところは**その立上りの芯**で押さえる（ご要望:
		//     外周の隅を立上りの外面から測らない）。取り合う立上りの厚みの中で終わる端は
		//     その芯へ押さえ直す（端そのものは測点にしない）。
		//   * 何とも取り合わない端（自由端）は端そのもの（立上りの面）で押さえる。
		//   * 同じ通りの別の立上りへ続く端は切れ目ではないので押さえない。
		//   * 通り芯は、取り合う立上りの芯などの測点と重なるときだけ値を借りる（重なれば
		//     寸法の数字が通り芯の間隔ちょうどになる）。**立上りの無い通り芯は測点にしない**
		//     ——現場に通り芯の墨は無く、そこは距離でしか分からない（ご要望）。
		WallLineStops lineStops(const WallLine& line, const std::vector<WallLine>& others,
								const std::vector<core::Vec2>& points,
								const std::vector<double>& grid, bool eastWest)
		{
			const std::vector<Junction> junctions = junctionsOf(line, others);
			std::vector<double> cores;
			cores.reserve(junctions.size() + (line.spans.size() * 2));
			for (const Junction& junction : junctions)
				cores.push_back(junction.coord);
			for (std::size_t i = 0; i < line.spans.size(); ++i)
			{
				for (const double end : {line.spans[i].first, line.spans[i].second})
				{
					bool continued = false;
					for (std::size_t k = 0; k < line.spans.size() && !continued; ++k)
					{
						continued = k != i && end >= line.spans[k].first - kDimensionMergeTol &&
									end <= line.spans[k].second + kDimensionMergeTol;
					}
					const bool atJunction =
						std::ranges::any_of(junctions,
											[end](const Junction& junction) {
												return std::abs(end - junction.coord) <=
													   junction.halfThickness + kDimensionMergeTol;
											});
					if (!continued && !atJunction)
						cores.push_back(end);
				}
			}

			// その通りの線材の上に乗る点（アンカーボルト・柱。芯からのずれが線材の厚みの半分
			// 以内で、どれかの区間の中にあるもの）。
			std::vector<double> onLine = cores;
			std::vector<double> rawPoints;
			for (const core::Vec2& point : points)
			{
				const double across = eastWest ? point.y : point.x;
				const double along = eastWest ? point.x : point.y;
				if (std::abs(across - line.coord) > line.halfThickness + kDimensionMergeTol)
					continue;
				if (inSpans(line.spans, along, kDimensionMergeTol))
				{
					onLine.push_back(along);
					rawPoints.push_back(along);
				}
			}

			const auto snapToGrid = [&grid](const std::vector<double>& values)
			{
				std::vector<double> onGrid;
				std::ranges::copy_if(grid, std::back_inserter(onGrid),
									 [&values](double g) { return nearAny(values, g); });
				return unionStops(onGrid, values);
			};
			WallLineStops result{&line, snapToGrid(cores), snapToGrid(onLine), false, false, {}};
			const auto isFree = [&junctions](double value)
			{
				return std::ranges::none_of(
					junctions, [value](const Junction& junction)
					{ return std::abs(value - junction.coord) <= kDimensionMergeTol; });
			};
			if (!result.cores.empty())
			{
				result.freeFront = isFree(result.cores.front());
				result.freeBack = isFree(result.cores.back());
			}
			// 通り芯へ寄せた後の値で持つ（all と同じ値で突き合わせられるように）。
			std::ranges::copy_if(result.all, std::back_inserter(result.points),
								 [&rawPoints](double value)
								 {
									 return std::ranges::any_of(
										 rawPoints, [value](double raw)
										 { return std::abs(raw - value) <= kDimensionMergeTol; });
								 });
			return result;
		}

		// 東西の通り（eastWest=true）または南北の通りの測点。座標の昇順。
		std::vector<WallLineStops> wallLineStops(const std::vector<WallLine>& lines,
												 const std::vector<WallLine>& others,
												 const std::vector<core::Vec2>& points,
												 const std::vector<core::GridCommand>& grids,
												 bool eastWest)
		{
			const std::vector<double> grid =
				gridStops(grids, eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical);
			std::vector<WallLineStops> out;
			out.reserve(lines.size());
			for (const WallLine& line : lines)
				out.push_back(lineStops(line, others, points, grid, eastWest));
			return out;
		}

		// line の low〜high（途切れた区間）の中を、直交する立上りが横切っているか。横切って
		// いれば、途切れの両側は別の部屋の立上りで、間は開口ではない。
		bool crossedBetween(const WallLine& line, const std::vector<WallLine>& others, double low,
							double high)
		{
			return std::ranges::any_of(
				others,
				[&](const WallLine& other)
				{
					if (other.coord <= low + other.halfThickness + kDimensionMergeTol ||
						other.coord >= high - other.halfThickness - kDimensionMergeTol)
						return false;
					return std::ranges::any_of(other.spans,
											   [&line](const std::pair<double, double>& span)
											   {
												   return span.first - kDimensionMergeTol <=
															  line.coord + line.halfThickness &&
														  span.second + kDimensionMergeTol >=
															  line.coord - line.halfThickness;
											   });
				});
		}

		// line の value が、直交する立上りと取り合う（その立上りの厚みの中にある）か。
		bool meetsCrossWall(const WallLine& line, const std::vector<WallLine>& others, double value)
		{
			return std::ranges::any_of(
				others,
				[&](const WallLine& other)
				{
					if (std::abs(other.coord - value) > other.halfThickness + kDimensionMergeTol)
						return false;
					return std::ranges::any_of(other.spans,
											   [&line](const std::pair<double, double>& span)
											   {
												   return span.first - kDimensionMergeTol <=
															  line.coord + line.halfThickness &&
														  span.second + kDimensionMergeTol >=
															  line.coord - line.halfThickness;
											   });
				});
		}

		// line の low〜high の途切れが開口（玄関・人通口など）か。両側が自由端で終わり、
		// 途切れの中を直交する立上りが横切らないもの。両側のどちらかが直交する立上りと
		// 取り合って終わる途切れ（隅と隅の間。段違いの外周の外など）は開口ではない。
		bool isOpening(const WallLine& line, const std::vector<WallLine>& others, double low,
					   double high)
		{
			return !meetsCrossWall(line, others, low) && !meetsCrossWall(line, others, high) &&
				   !crossedBetween(line, others, low, high);
		}

		// 通り 1 本を、区間が途切れるところで「一続きの立上り」ごとに割る（ご要望: 離れた
		// 立上りの間を寸法でまたがない。y3 通りの 7220 のように、別の立上りを横切って何も
		// 無い区間を測っても意味が無い）。**開口（isOpening）だけは割らずに**その幅を押さえる
		// （ご要望）。
		// また直交する立上りと 1 つも取り合わない一続き（位置がどこからも決まらない）は、
		// 近いほうの隣と 1 本にまとめたまま（間の寸法がその位置を押さえる）。
		std::vector<WallLine> splitIntoRuns(const std::vector<WallLine>& lines,
											const std::vector<WallLine>& others)
		{
			std::vector<WallLine> out;
			for (const WallLine& line : lines)
			{
				std::vector<WallLine> runs;
				double reach = 0.0;
				for (const std::pair<double, double>& span : line.spans)
				{
					if (runs.empty() || (span.first > reach + kDimensionMergeTol &&
										 !isOpening(line, others, reach, span.first)))
					{
						runs.push_back(WallLine{line.coord, {}, line.halfThickness});
						reach = span.second;
					}
					runs.back().spans.push_back(span);
					reach = std::max(reach, span.second);
				}
				const auto floating = [&others](const WallLine& run)
				{ return junctionsOf(run, others).empty(); };
				const auto gapAfter = [&runs](std::size_t i)
				{
					const double end =
						std::ranges::max(runs[i].spans, {}, &std::pair<double, double>::second)
							.second;
					return runs[i + 1].spans.front().first - end;
				};
				for (std::size_t i = 0; i < runs.size() && runs.size() > 1;)
				{
					if (!floating(runs[i]))
					{
						++i;
						continue;
					}
					// 近いほうの隣へまとめる（同じなら後ろ）。
					const bool intoNext =
						i + 1 < runs.size() && (i == 0 || gapAfter(i) <= gapAfter(i - 1));
					const std::size_t keep = intoNext ? i : i - 1;
					WallLine& kept = runs[keep];
					kept.spans.insert(kept.spans.end(), runs[keep + 1].spans.begin(),
									  runs[keep + 1].spans.end());
					std::ranges::sort(kept.spans);
					runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(keep) + 1);
					i = keep;
				}
				out.insert(out.end(), runs.begin(), runs.end());
			}
			return out;
		}

		// 基礎伏図の立上り 1 続きの列と、それを図のどこへ出すか。
		struct PlacedRun
		{
			WallLineStops stops;
			bool eastWest = true;
			// 図の外側に面する向き（+1 / −1。どちらにも面さなければ 0）。外側に面する
			// 立上りの列は、図の外形（min / max）を根元にして外周の列の 1 段目に並べる。
			int exteriorSide = 0;
		};

		// 基礎伏図の立上りを一続きに割り、測点と置き場所を決める。東西→南北、それぞれ座標の
		// 昇順→通りに沿った位置の昇順。
		std::vector<PlacedRun> placeFoundationRuns(const std::vector<WallLine>& eastWestRuns,
												   const std::vector<WallLine>& northSouthRuns,
												   const std::vector<core::Vec2>& points,
												   const std::vector<core::GridCommand>& grids,
												   const core::Vec2& center)
		{
			std::vector<PlacedRun> out;
			for (const bool eastWest : {true, false})
			{
				const std::vector<WallLine>& runs = eastWest ? eastWestRuns : northSouthRuns;
				const std::vector<WallLineStops> stops = wallLineStops(
					runs, eastWest ? northSouthRuns : eastWestRuns, points, grids, eastWest);
				// 外側に面するか: 外向きの側（座標の大きい側／小さい側）に、芯の範囲が
				// 重なる同じ向きの立上りが無い。範囲は芯で押さえ直した測点で見る（隅で
				// 外面まで伸びた端どうしを重なりと取らない）。
				const auto covered = [&stops](std::size_t i, int side)
				{
					const std::vector<double>& mine = stops[i].cores;
					if (mine.size() < 2)
						return true;
					return std::ranges::any_of(
						stops,
						[&](const WallLineStops& other)
						{
							if (&other == &stops[i] || other.cores.size() < 2)
								return false;
							const double delta = other.line->coord - stops[i].line->coord;
							if (delta * side <= kDimensionMergeTol)
								return false;
							return std::min(mine.back(), other.cores.back()) -
									   std::max(mine.front(), other.cores.front()) >
								   kDimensionMergeTol;
						});
				};
				for (std::size_t i = 0; i < stops.size(); ++i)
				{
					const bool up = !covered(i, 1);
					const bool down = !covered(i, -1);
					const double middle = eastWest ? center.y : center.x;
					const int outward = stops[i].line->coord >= middle ? 1 : -1;
					int exterior = 0;
					if (up && down)
						exterior = outward;
					else if (up)
						exterior = 1;
					else if (down)
						exterior = -1;
					out.push_back(PlacedRun{stops[i], eastWest, exterior});
				}
			}
			return out;
		}

		// ストーリレベルのレイヤ → (レベル種別, 絶対 Z)。横架材の「標準の天端」を引くのに使う。
		struct LayerLevel
		{
			std::string type;
			double z = 0.0;
		};

		std::map<std::string, LayerLevel>
		layerLevels(const std::vector<core::StoryCommand>& stories)
		{
			std::map<std::string, LayerLevel> levels;
			for (const core::StoryCommand& story : stories)
			{
				for (const core::LevelCommand& level : story.levels)
					levels.emplace(level.layer,
								   LayerLevel{level.type, story.elevation + level.offset});
			}
			return levels;
		}

		bool hasLevel(const core::StoryCommand& story, const char* type)
		{
			return std::ranges::any_of(story.levels, [type](const core::LevelCommand& level)
									   { return level.type == type; });
		}

		// 軸組図の高さの基準（GL・各階の FL・軒高）と、各階の FL から標準の横架材天端まで
		// （{FL, 横架材天端}。FL と同じ高さのもの＝軒高は持たない）。
		struct SectionHeights
		{
			std::vector<core::LevelMarkCommand> marks; // x は未設定（呼び出し側が入れる）
			std::vector<std::pair<double, double>> floorBeamTops;
		};

		// レベルに紐づくレイヤ → 何階か（1 始まり。柱の span レイヤの from と同じ数え方）。
		// 基礎ストーリのレイヤは 0 階、軒高ストーリは最上階の 1 つ上。軸組図で、横架材を
		// その上に立つ柱と同じ階として扱う（"2-横架材天端" の梁は 2 階の柱の足元）。
		std::map<std::string, int> layerFloors(const std::vector<core::StoryCommand>& stories)
		{
			std::map<std::string, int> floors;
			int floor = 0;
			for (const core::StoryCommand& story : stories)
			{
				const bool foundation = hasLevel(story, core::kLevelGL);
				if (!foundation)
					++floor;
				for (const core::LevelCommand& level : story.levels)
					floors.emplace(level.layer, foundation ? 0 : floor);
			}
			return floors;
		}

		SectionHeights sectionHeights(const std::vector<core::StoryCommand>& stories)
		{
			SectionHeights heights;
			int floor = 0;
			for (const core::StoryCommand& story : stories)
			{
				if (hasLevel(story, core::kLevelGL))
				{
					// 基礎ストーリ。GL は基礎ストーリの原点（常に 0。core/Document.h）。
					heights.marks.push_back(core::LevelMarkCommand{
						kLevelMarkGL, story.elevation, 0.0, story.name, core::kLevelGL});
					continue;
				}
				if (hasLevel(story, core::kLevelEaves))
				{
					// 最上階＝屋根。ストーリ原点が軒高で、横架材（軒桁）の天端もそこ
					// （parse/Story の beamTopElevation）。
					heights.marks.push_back(core::LevelMarkCommand{
						kLevelMarkEaves, story.elevation, 0.0, story.name, core::kLevelEaves});
					continue;
				}
				++floor;
				heights.marks.push_back(
					core::LevelMarkCommand{std::to_string(floor) + kLevelMarkFLSuffix,
										   story.elevation, 0.0, story.name, core::kLevelFL});
				for (const core::LevelCommand& level : story.levels)
				{
					if (level.type == core::kLevelBeamTop &&
						std::abs(level.offset) > kDimensionMergeTol)
						heights.floorBeamTops.emplace_back(story.elevation,
														   story.elevation + level.offset);
				}
			}
			std::ranges::stable_sort(heights.marks, {}, &core::LevelMarkCommand::elevation);
			return heights;
		}

		// 軸組図の左に出す高さの列。内側の段（0）に**各階の FL から横架材天端まで**を 1 本
		// ずつ、その外の段に **GL・FL・軒高の間隔**を置く（利用者の指定）。GL から土台天端・
		// FL から上階の横架材天端のような「基準の間をまたぐ」寸法は出さない——全体の高さは
		// 外の段の GL〜FL〜軒高が押さえ、横架材天端は各階の FL からの下がりで読めば足りる。
		// left は図の左端（補助線の根元）。レベル記号の起点をこの列より外へ出すので、寸法の
		// 命令とレベル記号の両方がここから段を知る（最も外の段は outermostTier）。
		std::vector<DimensionChainCommand> sectionHeightChains(const SectionHeights& heights,
															   double left)
		{
			std::vector<double> levelZ;
			levelZ.reserve(heights.marks.size());
			for (const core::LevelMarkCommand& mark : heights.marks)
				levelZ.push_back(mark.elevation);
			levelZ = mergeStops(std::move(levelZ));
			std::vector<DimensionChainCommand> out;
			// FL と横架材天端の組は高さの昇順（ストーリが Elevation 昇順なので並びも決まる）。
			std::vector<std::vector<double>> drops;
			for (const auto& [floor, beamTop] : heights.floorBeamTops)
			{
				std::vector<double> stops = mergeStops({floor, beamTop});
				if (stops.size() >= 2 &&
					std::ranges::find_if(drops, [&stops](const auto& d)
										 { return sameStops(d, stops); }) == drops.end())
					drops.push_back(std::move(stops));
			}
			out.reserve(drops.size() + 1);
			for (std::vector<double>& stops : drops)
				out.push_back(makeChain(DimensionAxis::Vertical, std::move(stops), left, -1, 0));
			if (levelZ.size() >= 2)
				out.push_back(
					makeChain(DimensionAxis::Vertical, levelZ, left, -1, drops.empty() ? 0 : 1));
			return out;
		}

		// 列の中で最も外の段（列が無ければ -1）。
		int outermostTier(const std::vector<DimensionChainCommand>& chains)
		{
			int tier = -1;
			for (const DimensionChainCommand& chain : chains)
				tier = std::max(tier, chain.tier);
			return tier;
		}

		// 切断面に乗る柱・横架材の、注釈空間の横の範囲。どちらも無ければ false。
		//
		// withCrossings なら**切断面を横切る横架材の切り口**も範囲へ入れる（材幅の半分ずつ
		// 広げる）。直交する横架材は断面に切り口として描かれ、通りに沿う材より外に出ることが
		// ある（片側だけ跳ね出した架構など）。レベル記号と高さの列はこの範囲の左端から外へ
		// 出すので、数えないと記号と寸法が切り口へ食い込む。**範囲を決めるのは切り口の有無
		// だけ**で、切り口しか無い断面（柱も沿う材も無い）には寸法を作らない——そこは従来
		// どおり（記号を置く根拠になる架構が無い）。柱の位置の列（下の横の列）が拾う通り芯の
		// 範囲は変えない（押さえるのは柱・束の位置で、切り口ではない）。
		bool sectionAlongRange(const core::Document& document, const core::SectionCommand& section,
							   double& low, double& high, bool withCrossings)
		{
			const double origin = core::sectionAlongOrigin(section);
			bool any = false;
			const auto takeRange = [&](const core::Vec2& plan, double halfWidth)
			{
				const double along =
					core::sectionAnnotationPoint(plan, 0.0, section.direction, origin).x;
				low = any ? std::min(low, along - halfWidth) : along - halfWidth;
				high = any ? std::max(high, along + halfWidth) : along + halfWidth;
				any = true;
			};
			const auto take = [&](const core::Vec2& plan) { takeRange(plan, 0.0); };
			for (const core::ColumnCommand& column : document.columns)
			{
				if (columnOnCutPlane(column, section))
					take(column.position);
			}
			for (const core::MemberCommand& member : document.members)
			{
				if (!memberOnCutPlane(member, section))
					continue;
				take(member.start);
				take(member.end);
			}
			if (!any || !withCrossings)
				return any;
			for (const core::MemberCommand& member : document.members)
			{
				core::Vec2 crossing;
				if (memberCrossesCutPlane(member, section, crossing))
					takeRange(crossing, member.width / 2.0);
			}
			return true;
		}
	} // namespace

	std::vector<double> mergeStops(std::vector<double> values)
	{
		std::ranges::sort(values);
		std::vector<double> out;
		for (const double value : values)
		{
			if (!std::isfinite(value))
				continue;
			if (out.empty() || value - out.back() > kDimensionMergeTol)
				out.push_back(value);
		}
		return out;
	}

	std::vector<double> unionStops(const std::vector<double>& primary,
								   const std::vector<double>& secondary)
	{
		std::vector<double> values = mergeStops(primary);
		const std::vector<double> kept = values;
		for (const double value : secondary)
		{
			if (!nearAny(kept, value))
				values.push_back(value);
		}
		return mergeStops(std::move(values));
	}

	std::vector<double> gridStops(const std::vector<core::GridCommand>& grids,
								  core::DimensionAxis axis)
	{
		std::vector<double> values;
		for (const core::GridCommand& grid : grids)
		{
			if (axis == DimensionAxis::Horizontal && runsNorthSouth(grid.start, grid.end))
				values.push_back((grid.start.x + grid.end.x) / 2.0);
			else if (axis == DimensionAxis::Vertical && runsEastWest(grid.start, grid.end))
				values.push_back((grid.start.y + grid.end.y) / 2.0);
		}
		return mergeStops(std::move(values));
	}

	std::vector<core::DimensionChainCommand>
	perimeterDimensionChains(const std::vector<double>& elementX,
							 const std::vector<double>& elementY, const std::vector<double>& gridX,
							 const std::vector<double>& gridY, const core::Vec2& min,
							 const core::Vec2& max, int firstTier)
	{
		std::vector<DimensionChainCommand> out;
		// X を測る列: 部材の位置も全長も上（+Y）。下には置かない。
		addPerimeterAxis(out, DimensionAxis::Horizontal, elementX, gridX, max.y, 1, true, max.y, 1,
						 firstTier);
		// Y を測る列: 部材の位置は左（−X）、全長は右（+X）。
		addPerimeterAxis(out, DimensionAxis::Vertical, elementY, gridY, min.x, -1, false, max.x, 1,
						 firstTier);
		return out;
	}

	namespace
	{
		// 外側の列に既に書いた芯・端どうしの寸法（同じ 2 点の間）。内部の列に同じ寸法を
		// 重ねない（ご要望: 外側を優先）ために覚えておく。**点（アンカーボルト・柱）の絡む
		// 寸法は覚えない**（別の通りの点がたまたま同じ位置にあるだけで、重なりではない）。
		class WrittenSegments
		{
		public:
			void remember(DimensionAxis axis, const std::vector<double>& stops,
						  const std::vector<double>& cores)
			{
				for (std::size_t i = 0; i + 1 < stops.size(); ++i)
				{
					if (nearAny(cores, stops[i]) && nearAny(cores, stops[i + 1]))
						segments_.at(static_cast<std::size_t>(axis))
							.emplace_back(stops[i], stops[i + 1]);
				}
			}

			[[nodiscard]] bool contains(DimensionAxis axis, double a, double b) const
			{
				return std::ranges::any_of(
					segments_.at(static_cast<std::size_t>(axis)),
					[a, b](const std::pair<double, double>& segment)
					{
						return std::abs(segment.first - a) <= kDimensionMergeTol &&
							   std::abs(segment.second - b) <= kDimensionMergeTol;
					});
			}

		private:
			std::array<std::vector<std::pair<double, double>>, 2> segments_;
		};

		// values の隣り合う 2 点のうち skip(a, b) が真の区間を抜き、残りを続いている区間
		// ごとの列（測点 2 つ以上）にする。
		template <class Skip>
		std::vector<std::vector<double>> splitWhere(const std::vector<double>& values, Skip skip)
		{
			std::vector<std::vector<double>> pieces;
			std::vector<double> piece;
			const auto flush = [&]()
			{
				if (piece.size() >= 2)
					pieces.push_back(piece);
				piece.clear();
			};
			for (std::size_t i = 0; i + 1 < values.size(); ++i)
			{
				if (skip(values[i], values[i + 1]))
				{
					flush();
					continue;
				}
				if (piece.empty())
					piece.push_back(values[i]);
				piece.push_back(values[i + 1]);
			}
			flush();
			return pieces;
		}

		// values を順に見て、芯・端どうしで既に書いた寸法を抜き、残りを続いている区間ごとの
		// 列にする（内部の列用）。書いた寸法は覚える。
		void emitUnwritten(std::vector<DimensionChainCommand>& out, WrittenSegments& written,
						   DimensionAxis axis, const std::vector<double>& values,
						   const std::vector<double>& cores, double base, int side, int tier)
		{
			for (std::vector<double>&piece : splitWhere(values,
														[&](double a, double b) {
															return nearAny(cores, a) &&
																   nearAny(cores, b) &&
																   written.contains(axis, a, b);
														}))
				out.push_back(makeChain(axis, std::move(piece), base, side, tier));
			written.remember(axis, values, cores);
		}

		// 一続きに割った通りと、その置き場所。placed の WallLineStops::line は eastWest /
		// northSouth の要素を指すので、**同じ入れ物で生かす**（ムーブしても要素の番地は
		// 変わらない）。
		struct RunLayout
		{
			std::vector<WallLine> eastWest;
			std::vector<WallLine> northSouth;
			std::vector<PlacedRun> placed;
		};

		// 線材を通りに集め、一続きに割って置き場所を決める（基礎伏図・床伏図・小屋伏図に共通）。
		RunLayout placeRuns(const std::vector<LineSegment>& segments,
							const std::vector<core::Vec2>& points,
							const std::vector<core::GridCommand>& grids, const core::Vec2& center)
		{
			const std::vector<WallLine> eastWestLines = collectWallLines(segments, true);
			const std::vector<WallLine> northSouthLines = collectWallLines(segments, false);
			RunLayout layout;
			layout.eastWest = splitIntoRuns(eastWestLines, northSouthLines);
			layout.northSouth = splitIntoRuns(northSouthLines, eastWestLines);
			layout.placed =
				placeFoundationRuns(layout.eastWest, layout.northSouth, points, grids, center);
			return layout;
		}

		// 内部の通りを、図の中心から遠い（外側の）順に並べる。
		std::vector<const PlacedRun*> interiorRuns(const std::vector<PlacedRun>& runs,
												   const core::Vec2& center)
		{
			std::vector<const PlacedRun*> interior;
			for (const PlacedRun& run : runs)
			{
				if (run.exteriorSide == 0 && run.stops.all.size() >= 2)
					interior.push_back(&run);
			}
			const auto distance = [&center](const PlacedRun* run)
			{
				const double middle = run->eastWest ? center.y : center.x;
				return std::abs(run->stops.line->coord - middle);
			};
			std::ranges::stable_sort(interior, std::ranges::greater{}, distance);
			return interior;
		}

		// 全長は通りの芯の端から端（L 字などで最も外の通りが建物の全体に渡らなくても
		// 全体を測る）。
		std::vector<double> overallOf(const std::vector<PlacedRun>& runs, bool eastWest)
		{
			std::vector<double> ends;
			for (const PlacedRun& run : runs)
			{
				if (run.eastWest != eastWest || run.stops.cores.empty())
					continue;
				ends.push_back(run.stops.cores.front());
				ends.push_back(run.stops.cores.back());
			}
			return ends.empty()
					   ? ends
					   : std::vector<double>{std::ranges::min(ends), std::ranges::max(ends)};
		}

		// 基礎伏図 1 枚ぶんの寸法の列（parse/Dimension.h 冒頭「基礎伏図は立上りに沿って
		// 押さえる」）。segments は立上り、points はアンカーボルト。
		std::vector<DimensionChainCommand>
		lineDimensionChains(const std::vector<LineSegment>& segments,
							const std::vector<core::Vec2>& points,
							const std::vector<core::GridCommand>& grids, const core::Vec2& min,
							const core::Vec2& max)
		{
			const core::Vec2 center{(min.x + max.x) / 2.0, (min.y + max.y) / 2.0};
			const RunLayout layout = placeRuns(segments, points, grids, center);
			const std::vector<PlacedRun>& runs = layout.placed;

			// 外周の 2 段目より外: 四辺とも外側に面する立上りの「芯の列」（取り合う立上りの芯と
			// 端。1 段目と同じなら重ねない）、上と右はその外に全長。
			std::vector<DimensionChainCommand> out;
			const auto sideStops = [&runs](bool eastWest, int side, bool cores)
			{
				std::vector<double> values;
				for (const PlacedRun& run : runs)
				{
					if (run.eastWest != eastWest || run.exteriorSide != side)
						continue;
					const std::vector<double>& from = cores ? run.stops.cores : run.stops.all;
					values.insert(values.end(), from.begin(), from.end());
				}
				return mergeStops(std::move(values));
			};
			const auto addSide =
				[&](DimensionAxis axis, bool eastWest, int side, double base, bool withOverall)
			{
				const std::vector<double> all = sideStops(eastWest, side, false);
				const std::vector<double> cores = sideStops(eastWest, side, true);
				int tier = 1;
				const std::vector<double>* inner = &all;
				if (cores.size() >= 2 && !sameStops(cores, all))
				{
					out.push_back(makeChain(axis, cores, base, side, tier++));
					inner = &cores;
				}
				const std::vector<double> overall = overallOf(runs, eastWest);
				if (withOverall && overall.size() == 2 && !sameStops(overall, *inner))
					out.push_back(makeChain(axis, overall, base, side, tier));
			};
			addSide(DimensionAxis::Horizontal, true, 1, max.y, true);
			addSide(DimensionAxis::Vertical, false, -1, min.x, false);
			addSide(DimensionAxis::Horizontal, true, -1, min.y, false);
			addSide(DimensionAxis::Vertical, false, 1, max.x, true);

			// 1 段目: 立上りに沿う列。外側に面するものは図の外形を根元にして外周に並べる。
			//
			// 内部の立上りの列には、**既に外側の列にある立上りの芯・端どうしの寸法（同じ 2 点の
			// 間）は書かない**（ご要望: 連続した立上りに同じ寸法を重ねない。外側を優先）。
			// アンカーボルトの絡む寸法は比べない（WrittenSegments）。外側から順に見て、書いた
			// 芯どうしの寸法を覚えていく。
			WrittenSegments written;
			// 外周の 2 段目より外は芯と端だけでできている。
			for (const DimensionChainCommand& chain : out)
				written.remember(chain.axis, chain.stops, chain.stops);

			for (const PlacedRun& run : runs)
			{
				if (run.stops.all.size() < 2 || run.exteriorSide == 0)
					continue;
				const DimensionAxis axis =
					run.eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical;
				const core::Vec2& edge = run.exteriorSide > 0 ? max : min;
				out.push_back(makeChain(axis, run.stops.all, run.eastWest ? edge.y : edge.x,
										run.exteriorSide, 0));
				written.remember(axis, run.stops.all, run.stops.cores);
			}

			// 内部の立上りの列は、その芯から図の外側（中心から遠い側）へ出す。
			for (const PlacedRun* run : interiorRuns(runs, center))
			{
				const DimensionAxis axis =
					run->eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical;
				const double base = run->stops.line->coord;
				const double middle = run->eastWest ? center.y : center.x;
				const int side = base >= middle ? 1 : -1;
				const std::vector<double>& stops = run->stops.all;
				const std::vector<double>& cores = run->stops.cores;
				const auto emit = [&](const std::vector<double>& values, int tier)
				{ emitUnwritten(out, written, axis, values, cores, base, side, tier); };

				// **立上りの芯・端の列とアンカーボルトの列を分ける**（ご要望: 現場では立上りの
				// 位置が決まってからアンカーボルトを置くので、立上りの寸法だけを追えるように）。
				// アンカーボルトが乗る立上りは、芯・端だけの列を 1 つ外の段に出し、1 段目には
				// アンカーボルトの絡む寸法だけを残す（芯・端どうしは外の列にあるので抜ける）。
				// アンカーボルトが無ければ芯・端の列が 1 段目。
				const bool withBolts = !sameStops(stops, cores);
				const int coreTier = withBolts ? 1 : 0;
				emit(cores, coreTier);
				if (withBolts)
					emit(stops, 0);

				// 半島状・独立した立上り（自由端で終わる一続き）は長さも押さえる（ご要望:
				// y3 通りの x0〜x1 の端＝200＋510＋260＝970）。芯・端の列と同じなら出さない。
				if ((run->stops.freeFront || run->stops.freeBack) && cores.size() >= 3 &&
					!written.contains(axis, cores.front(), cores.back()))
				{
					out.push_back(
						makeChain(axis, {cores.front(), cores.back()}, base, side, coreTier + 1));
					written.remember(axis, {cores.front(), cores.back()}, cores);
				}
			}
			return out;
		}

		// a〜b が stops の隣り合う 2 点か（同じ寸法が stops の列に既にあるか）。
		bool isSegmentOf(const std::vector<double>& stops, double a, double b)
		{
			for (std::size_t i = 0; i + 1 < stops.size(); ++i)
			{
				if (std::abs(stops[i] - a) <= kDimensionMergeTol &&
					std::abs(stops[i + 1] - b) <= kDimensionMergeTol)
					return true;
			}
			return false;
		}

		// 通り 1 本の柱の列（parse/Dimension.h 冒頭「床伏図・小屋伏図も横架材に沿って
		// 押さえる」）。梁の取り合い・端から外れた柱があれば、柱の位置と通りの両端。
		// 無ければ（柱の無い通り・柱が全部取り合いの上にある通り）柱と梁を分ける意味が
		// 無いので、芯・端と柱すべての 1 列。
		std::vector<double> columnStops(const WallLineStops& stops)
		{
			const bool apart = std::ranges::any_of(stops.points, [&stops](double point)
												   { return !nearAny(stops.cores, point); });
			if (!apart || stops.cores.empty())
				return stops.all;
			std::vector<double> columns = stops.points;
			columns.push_back(stops.cores.front());
			columns.push_back(stops.cores.back());
			return mergeStops(std::move(columns));
		}

		// 柱とは別に押さえる横架材の列（取り合う梁の芯・端）。柱の列から外れた取り合いが
		// 1 つも無ければ作らない。柱の列に同じ寸法がある区間と、skip が真の区間は抜く。
		template <class Skip>
		std::vector<std::vector<double>> beamPieces(const std::vector<double>& cores,
													const std::vector<double>& columns, Skip skip)
		{
			if (std::ranges::all_of(cores,
									[&columns](double core) { return nearAny(columns, core); }))
				return {};
			return splitWhere(cores, [&](double a, double b)
							  { return isSegmentOf(columns, a, b) || skip(a, b); });
		}

		// 床伏図・小屋伏図 1 枚ぶんの寸法の列。segments は横架材、points は柱。
		std::vector<DimensionChainCommand>
		framingLineChains(const std::vector<LineSegment>& segments,
						  const std::vector<core::Vec2>& points,
						  const std::vector<core::GridCommand>& grids, const core::Vec2& min,
						  const core::Vec2& max)
		{
			const core::Vec2 center{(min.x + max.x) / 2.0, (min.y + max.y) / 2.0};
			const RunLayout layout = placeRuns(segments, points, grids, center);
			const std::vector<PlacedRun>& runs = layout.placed;
			std::vector<DimensionChainCommand> out;
			WrittenSegments written;

			// 外周: 辺ごとに、外側に面する通りの柱の列を**1 本につなげて**図の外形から出す
			// （ご要望: 段違いの外周で列が途切れるのは不自然）。全長を持つ上と右は全長の端まで
			// 延ばす（外側に面する横架材の無い区間＝右の 5〜7' 間なども押さえる）。柱と梁の
			// 取り合いが一致しない辺は、横架材の列を 1 段内側に出し、柱の列を 1 段外へ出す。
			struct Side
			{
				bool eastWest;
				int side;
				double base;
				bool withOverall;
			};
			// 辺ごとの柱の列・横架材（取り合い）の測点・全長。
			struct SideRows
			{
				Side edge;
				DimensionAxis axis;
				std::vector<double> row;
				std::vector<double> cores;
				std::vector<double> overall;
			};
			std::vector<SideRows> sides;
			for (const Side& edge : {Side{true, 1, max.y, true}, Side{false, -1, min.x, false},
									 Side{true, -1, min.y, false}, Side{false, 1, max.x, true}})
			{
				SideRows rows{edge,
							  edge.eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical,
							  {},
							  {},
							  overallOf(runs, edge.eastWest)};
				for (const PlacedRun& run : runs)
				{
					if (run.eastWest != edge.eastWest || run.exteriorSide != edge.side ||
						run.stops.all.size() < 2)
						continue;
					const std::vector<double> columns = columnStops(run.stops);
					rows.row.insert(rows.row.end(), columns.begin(), columns.end());
					rows.cores.insert(rows.cores.end(), run.stops.cores.begin(),
									  run.stops.cores.end());
				}
				// 外側に面する通りが無い辺でも、全長を持つ上と右は全長を出す（基礎伏図と同じ。
				// 通りが全部下／左に面する U 字の架構など）。そのときの列は全長そのもの。
				if (edge.withOverall)
					rows.row.insert(rows.row.end(), rows.overall.begin(), rows.overall.end());
				rows.row = mergeStops(std::move(rows.row));
				if (rows.row.size() < 2)
					continue;
				rows.cores = mergeStops(std::move(rows.cores));
				// 柱の列は四辺とも先に覚える。横架材の列は、どの辺の柱の列にある寸法も重ねない
				// （ご要望: い通りの 5〜8 の 2685 が、右の柱の列を全長の端まで延ばした 5〜7' と
				// 重なっていた）。
				written.remember(rows.axis, rows.row, rows.row);
				sides.push_back(std::move(rows));
			}
			for (const SideRows& rows : sides)
			{
				const Side& edge = rows.edge;
				std::vector<std::vector<double>> beams =
					beamPieces(rows.cores, rows.row, [&written, &rows](double a, double b)
							   { return written.contains(rows.axis, a, b); });
				const int rowTier = beams.empty() ? 0 : 1;
				if (rows.row.size() >= 2)
					out.push_back(makeChain(rows.axis, rows.row, edge.base, edge.side, rowTier));
				for (std::vector<double>& chain : beams)
				{
					written.remember(rows.axis, chain, chain);
					out.push_back(makeChain(rows.axis, std::move(chain), edge.base, edge.side, 0));
				}
				if (edge.withOverall && rows.overall.size() == 2 &&
					!sameStops(rows.overall, rows.row))
					out.push_back(
						makeChain(rows.axis, rows.overall, edge.base, edge.side, rowTier + 1));
			}

			// 内部の通り: 柱の列と横架材の列。どちらも外側の列（先に書いた列）にある芯・端
			// どうしの寸法は重ねない（ご要望: ろ〜はの 910 が 1 通りと 5 通りに、1〜1' の 650 が
			// ろ通りとは通りに重なっていた）。
			for (const PlacedRun* run : interiorRuns(runs, center))
			{
				const DimensionAxis axis =
					run->eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical;
				const double base = run->stops.line->coord;
				const double middle = run->eastWest ? center.y : center.x;
				const int side = base >= middle ? 1 : -1;
				const std::vector<double>& cores = run->stops.cores;
				const std::vector<double> columns = columnStops(run->stops);
				// 横架材の列は柱の列を覚える前に決める（柱の列と同じ区間は isSegmentOf が抜く）。
				std::vector<std::vector<double>> beams =
					beamPieces(cores, columns, [&written, axis](double a, double b)
							   { return written.contains(axis, a, b); });
				const int columnTier = beams.empty() ? 0 : 1;
				emitUnwritten(out, written, axis, columns, cores, base, side, columnTier);
				for (std::vector<double>& chain : beams)
					out.push_back(makeChain(axis, std::move(chain), base, side, 0));
				written.remember(axis, cores, cores);
				// 半島状・独立した梁（自由端で終わる一続き）は長さも押さえる。
				if ((run->stops.freeFront || run->stops.freeBack) && columns.size() >= 3 &&
					!written.contains(axis, cores.front(), cores.back()))
				{
					out.push_back(
						makeChain(axis, {cores.front(), cores.back()}, base, side, columnTier + 1));
					written.remember(axis, {cores.front(), cores.back()}, cores);
				}
			}
			return out;
		}
	} // namespace

	std::vector<core::DimensionChainCommand>
	foundationDimensionChains(const std::vector<core::WallCommand>& walls,
							  const std::vector<core::SymbolCommand>& anchorBolts,
							  const std::vector<core::GridCommand>& grids, const core::Vec2& min,
							  const core::Vec2& max)
	{
		std::vector<LineSegment> segments;
		segments.reserve(walls.size());
		for (const core::WallCommand& wall : walls)
			segments.push_back(LineSegment{wall.start, wall.end, wall.thickness});
		std::vector<core::Vec2> points;
		points.reserve(anchorBolts.size());
		for (const core::SymbolCommand& bolt : anchorBolts)
			points.push_back(bolt.position);
		return lineDimensionChains(segments, points, grids, min, max);
	}

	std::vector<core::DimensionChainCommand>
	framingDimensionChains(const std::vector<core::MemberCommand>& members,
						   const std::vector<core::ColumnCommand>& columns,
						   const std::vector<core::GridCommand>& grids, const core::Vec2& min,
						   const core::Vec2& max)
	{
		// 横架材の端点は取り付く相手（横架材・柱）の芯の上にある（core::MemberCommand）
		// ので、取り合いは立上りと同じ判定で拾える。斜めの材は通りを作らない
		// （collectWallLines が拾わない）。
		std::vector<LineSegment> segments;
		segments.reserve(members.size());
		for (const core::MemberCommand& member : members)
			segments.push_back(LineSegment{member.start, member.end, member.width});
		std::vector<core::Vec2> points;
		points.reserve(columns.size());
		for (const core::ColumnCommand& column : columns)
			points.push_back(column.position);
		return framingLineChains(segments, points, grids, min, max);
	}

	std::vector<core::DimensionChainCommand>
	buildPlanDimensionCommands(const core::Document& document, const core::SheetCommand& sheet)
	{
		const std::vector<std::string>& layers = sheet.viewport.layers;
		// 図の外形は**通り芯を除いて**測る（通り芯は符号の円まで外へ延びるので、それで
		// 外形を取ると寸法が符号の外へ押し出される）。
		std::vector<std::string> contentLayers;
		std::ranges::copy_if(layers, std::back_inserter(contentLayers),
							 [](const std::string& layer) { return layer != core::kGridLayer; });
		core::Vec2 min;
		core::Vec2 max;
		if (contentLayers.empty() || !core::planContentBounds(document, contentLayers, min, max))
			return {};

		const std::map<std::string, LayerLevel> levels = layerLevels(document.stories);
		std::vector<double> elementX;
		std::vector<double> elementY;
		const auto takeMember = [&](const core::Vec2& start, const core::Vec2& end)
		{
			if (runsNorthSouth(start, end))
				elementX.push_back((start.x + end.x) / 2.0);
			else if (runsEastWest(start, end))
				elementY.push_back((start.y + end.y) / 2.0);
		};

		switch (sheet.kind)
		{
		case core::PlanKind::Foundation:
		{
			// 立上りの通りに沿う列（アンカーボルト・切れ目・取り合う立上りの芯）と、外周の
			// 芯の列・全長。外周の列には外周の立上りに取り合うものしか載せない（内部の
			// 立上りの位置はその立上りに沿う列が押さえる）ので、外周の部材の位置の列
			// （perimeterDimensionChains）は使わない。床伏図・小屋伏図も同じ。
			std::vector<core::WallCommand> walls;
			std::ranges::copy_if(document.walls, std::back_inserter(walls),
								 [&layers](const core::WallCommand& wall)
								 { return onLayers(layers, wall.layer); });
			std::vector<core::SymbolCommand> bolts;
			std::ranges::copy_if(document.anchorBolts, std::back_inserter(bolts),
								 [&layers](const core::SymbolCommand& bolt)
								 { return onLayers(layers, bolt.layer); });
			return foundationDimensionChains(walls, bolts, document.grids, min, max);
		}
		case core::PlanKind::Framing:
		{
			// 柱（小屋束を含む）と梁を、基礎伏図と同じく横架材の通りに沿って押さえる。外周には
			// 外周の横架材に乗る柱と、外周の横架材に取り合う直交する梁だけを出し、それ以外は
			// 内部の横架材に沿う列が押さえる。
			std::vector<core::MemberCommand> members;
			std::ranges::copy_if(document.members, std::back_inserter(members),
								 [&layers](const core::MemberCommand& member)
								 { return onLayers(layers, member.layer); });
			std::vector<core::ColumnCommand> columns;
			std::ranges::copy_if(document.columns, std::back_inserter(columns),
								 [&layers](const core::ColumnCommand& column)
								 { return onLayers(layers, column.layer); });
			return framingDimensionChains(members, columns, document.grids, min, max);
		}
		case core::PlanKind::Moya:
			// 母屋だけ（垂木・登り梁は押さえない）。母屋かどうかは配置先レイヤのレベル種別で
			// 決める——parse/Member が母屋を置いたレイヤそのもので、名前の綴りを見ない。
			for (const core::MemberCommand& member : document.members)
			{
				if (!onLayers(layers, member.layer))
					continue;
				const auto level = levels.find(member.layer);
				if (level != levels.end() && level->second.type == core::kLevelMoya)
					takeMember(member.start, member.end);
			}
			break;
		}

		std::vector<DimensionChainCommand> out = perimeterDimensionChains(
			elementX, elementY, gridStops(document.grids, DimensionAxis::Horizontal),
			gridStops(document.grids, DimensionAxis::Vertical), min, max, 0);
		return out;
	}

	std::vector<core::DimensionChainCommand>
	buildSectionDimensionCommands(const core::Document& document,
								  const core::SectionCommand& section)
	{
		// low / high は柱・沿う材の範囲（柱の位置の列が拾う通り芯を絞る）、left は切り口も
		// 含めた左端（高さの列の根元。レベル記号の x と揃える）。
		double low = 0.0;
		double high = 0.0;
		if (!sectionAlongRange(document, section, low, high, false))
			return {};
		double left = low;
		double right = high;
		sectionAlongRange(document, section, left, right, true);
		double bottom = 0.0;
		double top = 0.0;
		if (!core::sectionHeightRange(document, bottom, top))
			return {};
		// 高さ範囲は上下に余白を足してあるので、建物の下端・上端へ戻す。
		bottom += core::kSectionHeightMargin;
		top -= core::kSectionHeightMargin;

		const double origin = core::sectionAlongOrigin(section);
		const auto along = [&](const core::Vec2& plan)
		{ return core::sectionAnnotationPoint(plan, 0.0, section.direction, origin).x; };

		std::vector<DimensionChainCommand> out;

		// 横: 柱・束の位置。**1 階の柱は図の下**、**1 階と合わない上階の柱と小屋束は図の上**に
		// 出す（利用者の指定）。
		//   * 柱は立つ階ごとに分ける（span レイヤ "{from}to{to}-柱" の from〜to がまたぐ階。
		//     通し柱はまたぐ階のすべてに数える）。span レイヤでない柱は 1 階とみなす。
		//   * **紙面と平行な横架材が直交する横架材にぶつかる位置**も、その横架材の階の列の
		//     測点にする（は通りの 5通り。利用者の指定）。沿う材の端（端点は接合点。M20）に、
		//     同じ階の切り口があるところ。
		//   * 上階の柱の列は、その階に**直下の階の柱と合わない柱が 1 本でもあるとき**だけ、その
		//     階の柱すべて（＋上記のぶつかる位置）で作る（下の列と重なる寸法が出ても構わない。
		//     利用者の指定）。合うかどうかは柱だけで見る。
		//   * 小屋束の列は上階の柱の列の 1 つ外の段（どの階の小屋束もまとめて 1 列。屋根の階に
		//     立つ柱も含める）。**建物の外周芯**（屋根の階より下の柱の両端）も測点に入れる
		//     （利用者の指定）。
		//   * **部材の無い通り芯は測点にしない**（通り芯しか無い位置を測っても意味が無い）。
		//     通り芯は測点と重なるときに値を貸すだけ（寸法の数字が通り芯の間隔ちょうどになる）。
		std::vector<double> gridAll;
		const DimensionAxis crossing = section.direction == core::SectionDirection::X
										   ? DimensionAxis::Vertical
										   : DimensionAxis::Horizontal;
		for (const double value : gridStops(document.grids, crossing))
			gridAll.push_back(value - origin);
		const auto snapWithin = [&gridAll](double value, double tol)
		{
			for (const double g : gridAll)
			{
				if (std::abs(g - value) <= tol)
					return g;
			}
			return value;
		};
		const auto lendGrid = [&snapWithin](std::vector<double> values)
		{
			for (double& value : values)
				value = snapWithin(value, kDimensionMergeTol);
			return mergeStops(std::move(values));
		};

		// 屋根の階（軒高ストーリ。FL の階の数 + 1）。ここに立つ柱（軒高の梁の上の束。
		// ホームズ君は柱として出すことがある）は小屋束と同じ列で押さえる（1通りの実機）。
		const int roofFloor =
			static_cast<int>(std::ranges::count_if(document.stories,
												   [](const core::StoryCommand& story) {
													   return !hasLevel(story, core::kLevelGL) &&
															  !hasLevel(story, core::kLevelEaves);
												   })) +
			1;
		std::map<int, std::vector<double>> floorColumns;
		std::vector<double> koyazuka;
		for (const core::ColumnCommand& column : document.columns)
		{
			if (!columnOnCutPlane(column, section))
				continue;
			const double a = along(column.position);
			double from = 0.0;
			double to = 0.0;
			const bool span = parseSpanLayer(column.layer, from, to) && to > from;
			if (column.structuralUse == core::kStructuralUseKoyazuka ||
				(span && std::lround(from) >= roofFloor))
			{
				koyazuka.push_back(a);
				continue;
			}
			if (!span)
			{
				floorColumns[1].push_back(a);
				continue;
			}
			// from は整数（柱が立つ床）。半整数の to（屋根面で止まる）はその階までに数える。
			for (int floor = static_cast<int>(std::lround(from)); floor < to; ++floor)
				floorColumns[floor].push_back(a);
		}
		for (auto& entry : floorColumns)
			entry.second = lendGrid(std::move(entry.second));
		const int lowestFloor = floorColumns.empty() ? 1 : std::min(1, floorColumns.begin()->first);

		// 紙面と平行な横架材が直交する横架材にぶつかる位置（階ごと）。測点は沿う材の端
		// そのもので、通り芯はちょうど重なるときだけ値を貸す——外壁芯が通り芯より少し内側に
		// ある建物で、端を近くの通り芯へ寄せると外壁芯と通り芯の間に 45 のような寸法が出た
		// （い通りの 8通り。実機）。
		const std::map<std::string, int> floorsOfLayer = layerFloors(document.stories);
		const auto floorOf = [&floorsOfLayer](const std::string& layer)
		{
			const auto found = floorsOfLayer.find(layer);
			return found == floorsOfLayer.end() ? -1 : found->second;
		};
		std::vector<std::pair<double, int>> cuts;
		for (const core::MemberCommand& member : document.members)
		{
			core::Vec2 point;
			if (memberCrossesCutPlane(member, section, point))
				cuts.emplace_back(along(point), floorOf(member.layer));
		}
		std::map<int, std::vector<double>> floorJunctions;
		for (const core::MemberCommand& member : document.members)
		{
			if (!memberOnCutPlane(member, section))
				continue;
			const int floor = floorOf(member.layer);
			if (floor < 0)
				continue;
			for (const core::Vec2& end : {member.start, member.end})
			{
				const double a = along(end);
				const bool meets = std::ranges::any_of(
					cuts, [a, floor](const std::pair<double, int>& cut)
					{ return cut.second == floor && std::abs(cut.first - a) <= kClusterTol; });
				if (meets)
					floorJunctions[floor].push_back(snapWithin(a, kDimensionMergeTol));
			}
		}
		// 階 floor の列の測点（柱＋ぶつかる位置）。
		const auto floorStops = [&](int floor)
		{
			std::vector<double> stops;
			if (const auto found = floorColumns.find(floor); found != floorColumns.end())
				stops = found->second;
			if (const auto found = floorJunctions.find(floor); found != floorJunctions.end())
				stops.insert(stops.end(), found->second.begin(), found->second.end());
			return mergeStops(std::move(stops));
		};

		std::vector<double> columns = floorStops(lowestFloor);

		// その面の**最外周**も押さえる（利用者の指定）。柱・沿う材より**kClusterTol を超えて**
		// 外に横架材の切り口があれば、左右それぞれ最も外の切り口の芯を下の列の測点に足す
		// （又は通りなら 1通り〜5通り）。間に並ぶ切り口は足さない（押さえるのは最外周だけ）。
		// 外壁芯のすぐ外（通り芯の上など）の切り口は足さない——押さえるのは建物の外周
		// （外壁芯）まで（い通りの 8通り。実機）。切り口の近く（kClusterTol 以内）に通り芯が
		// あれば、寸法の数字が通り芯の間隔になるよう通り芯の値を採る。
		if (!cuts.empty())
		{
			const auto [lowest, highest] =
				std::ranges::minmax_element(cuts, {}, &std::pair<double, int>::first);
			if (lowest->first < low - kClusterTol)
				columns.push_back(snapWithin(lowest->first, kClusterTol));
			if (highest->first > high + kClusterTol)
				columns.push_back(snapWithin(highest->first, kClusterTol));
		}
		const std::vector<double> detail = mergeStops(std::move(columns));
		if (detail.size() >= 2)
			out.push_back(makeChain(DimensionAxis::Horizontal, detail, bottom, -1, 0));

		// 図の上: 根元は建物の上端（全軸組図で共通）。
		int topTier = 0;
		for (const auto& [floor, stops] : floorColumns)
		{
			if (floor <= lowestFloor)
				continue;
			const auto lower = floorColumns.find(floor - 1);
			const bool mismatched = lower == floorColumns.end() ||
									std::ranges::any_of(stops, [&lower](double v)
														{ return !nearAny(lower->second, v); });
			const std::vector<double> upper = floorStops(floor);
			if (mismatched && upper.size() >= 2)
				out.push_back(makeChain(DimensionAxis::Horizontal, upper, top, 1, topTier++));
		}
		if (!koyazuka.empty())
		{
			// 建物の外周芯（屋根の階より下の柱の両端。柱が無ければ下の列の両端）から押さえる
			// （利用者の指定。小屋束が立つ階の外壁ではなく、あくまで建物の外周）。
			std::vector<double> walls;
			for (const auto& entry : floorColumns)
				walls.insert(walls.end(), entry.second.begin(), entry.second.end());
			walls = mergeStops(std::move(walls));
			if (walls.empty())
				walls = detail;
			std::vector<double> posts = koyazuka;
			if (!walls.empty())
			{
				posts.push_back(walls.front());
				posts.push_back(walls.back());
			}
			posts = lendGrid(std::move(posts));
			if (posts.size() >= 2)
				out.push_back(makeChain(DimensionAxis::Horizontal, posts, top, 1, topTier++));
		}

		// 縦: GL・FL・軒高と、そこからの標準の横架材天端 → GL・FL・軒高の間隔。図の左に出す。
		for (DimensionChainCommand& chain :
			 sectionHeightChains(sectionHeights(document.stories), left))
			out.push_back(std::move(chain));

		// 標準の横架材天端と違う高さの横架材: 標準の天端からその材の天端までを、材の中央で
		// 押さえる。対象は横架材レベル（横架材天端・軒高）に置かれた水平な材だけ——母屋・
		// 登り梁は高さがもともと材ごとに違う（標準の天端という考えが無い）。
		// 同じ高さの材が並ぶと材の数だけ同じ寸法が並ぶので、測る区間（標準の天端〜材の天端）
		// が同じ材は 1 本にまとめ、そのうち最も長い材の中央に置く（まとめた材どうしが離れて
		// いても、置いた位置の下には必ずその高さの材がある）。
		struct OffStandard
		{
			double low = 0.0;
			double high = 0.0;
			double length = 0.0;
			double middle = 0.0;
		};
		std::vector<OffStandard> offStandards;
		const std::map<std::string, LayerLevel> levels = layerLevels(document.stories);
		for (const core::MemberCommand& member : document.members)
		{
			if (!memberOnCutPlane(member, section))
				continue;
			// 伏図レベルのレイヤ（"2-横架材天端(FL-872)"）の材も、その階の標準の天端
			// からの差を押さえる（印を外した元のレイヤのレベル。parse/PlanLevel）。
			const auto level = levels.find(core::stripPlanLevelTag(member.layer));
			if (level == levels.end() || (level->second.type != core::kLevelBeamTop &&
										  level->second.type != core::kLevelEaves))
				continue;
			if (std::abs(member.elevation - member.endElevation) > kDimensionMergeTol)
				continue;
			const double memberTop = core::memberTopZ(member);
			const double standard = level->second.z;
			if (std::abs(memberTop - standard) <= kDimensionMergeTol)
				continue;
			const double a = along(member.start);
			const double b = along(member.end);
			offStandards.push_back(OffStandard{std::min(memberTop, standard),
											   std::max(memberTop, standard), std::abs(b - a),
											   (a + b) / 2.0});
		}
		// 区間 → 長い順 → 位置の順に並べ、区間ごとの先頭（最も長い材）だけを残す
		// （入力の並びに依らない）。
		std::ranges::sort(offStandards,
						  [](const OffStandard& p, const OffStandard& q)
						  {
							  if (p.low != q.low)
								  return p.low < q.low;
							  if (p.high != q.high)
								  return p.high < q.high;
							  if (p.length != q.length)
								  return p.length > q.length;
							  return p.middle < q.middle;
						  });
		std::vector<OffStandard> kept;
		for (const OffStandard& candidate : offStandards)
		{
			const bool duplicate = std::ranges::any_of(
				kept,
				[&candidate](const OffStandard& k)
				{
					return std::abs(k.low - candidate.low) <= kDimensionMergeTol &&
						   std::abs(k.high - candidate.high) <= kDimensionMergeTol;
				});
			if (duplicate)
				continue;
			kept.push_back(candidate);
			out.push_back(makeChain(DimensionAxis::Vertical, {candidate.low, candidate.high},
									candidate.middle, 1, 0));
		}
		return out;
	}

	std::vector<core::LevelMarkCommand> buildSectionLevelMarks(const core::Document& document,
															   const core::SectionCommand& section)
	{
		// 切り口も含めた範囲（高さの列の根元と同じ。buildSectionDimensionCommands）。
		double low = 0.0;
		double high = 0.0;
		if (!sectionAlongRange(document, section, low, high, true))
			return {};
		const SectionHeights heights = sectionHeights(document.stories);
		// 左の高さの列の最も外の段（列が無ければ -1）。記号の名前をこれより外へ出す。
		const int outerTier = outermostTier(sectionHeightChains(heights, low));
		std::vector<core::LevelMarkCommand> marks = heights.marks;
		for (core::LevelMarkCommand& mark : marks)
		{
			mark.x = low;
			mark.right = high;
			mark.dimensionTier = outerTier;
		}
		return marks;
	}

	void attachDimensionCommands(core::Document& document)
	{
		for (core::SheetCommand& sheet : document.sheets)
			sheet.viewport.dimensions = buildPlanDimensionCommands(document, sheet);
		for (core::SectionCommand& section : document.sections)
		{
			section.viewport.dimensions = buildSectionDimensionCommands(document, section);
			section.levels = buildSectionLevelMarks(document, section);
		}
	}
} // namespace HomeskzIfcImport::parse
