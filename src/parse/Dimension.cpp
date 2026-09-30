//
//	parse/Dimension.cpp
//
//	寸法の解析（docs/DEV-NOTES.md M31）。【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	寸法は命令セットだけから導出する（parse/Dimension.h 冒頭）。並びは入力の命令の並びに依らず、
//	測点を昇順にまとめ、列を「外周（X を測る列→Y を測る列）→ 立上りに沿う列（東西の通り→
//	南北の通り、それぞれ座標の昇順）」の順に出すので決定的になる。
//

#include "parse/Dimension.h"
#include "core/Document.h"
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

		// 直線に乗る立上りの群（基礎伏図の「通り」1 本）。
		struct WallLine
		{
			double coord = 0.0; // 直交座標（東西の通りなら Y）
			std::vector<std::pair<double, double>> spans; // 通りに沿った区間（昇順）
			double halfThickness = 0.0; // 群の中で最も厚い立上りの半分
		};

		// 立上りを東西の通り（eastWest=true）または南北の通りへまとめる。座標の昇順。
		std::vector<WallLine> collectWallLines(const std::vector<core::WallCommand>& walls,
											   bool eastWest)
		{
			std::vector<WallLine> lines;
			std::vector<const core::WallCommand*> picked;
			for (const core::WallCommand& wall : walls)
			{
				if (eastWest ? runsEastWest(wall.start, wall.end)
							 : runsNorthSouth(wall.start, wall.end))
					picked.push_back(&wall);
			}
			const auto coordOf = [eastWest](const core::WallCommand& wall) {
				return eastWest ? (wall.start.y + wall.end.y) / 2.0
								: (wall.start.x + wall.end.x) / 2.0;
			};
			const auto spanOf = [eastWest](const core::WallCommand& wall)
			{
				const double a = eastWest ? wall.start.x : wall.start.y;
				const double b = eastWest ? wall.end.x : wall.end.y;
				return std::pair<double, double>{std::min(a, b), std::max(a, b)};
			};
			// 座標 → 区間の順に並べてから群にする（入力の並びに依らない）。
			std::ranges::sort(picked,
							  [&](const core::WallCommand* a, const core::WallCommand* b)
							  {
								  const double ca = coordOf(*a);
								  const double cb = coordOf(*b);
								  if (ca != cb)
									  return ca < cb;
								  return spanOf(*a) < spanOf(*b);
							  });
			for (const core::WallCommand* wall : picked)
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
			std::vector<double> all; // cores ＋ その通りに乗るアンカーボルト
			// 両端（cores の最初と最後）が何とも取り合わない自由端か。自由端を持つ一続き
			// （半島状・独立した立上り）は長さも押さえる。
			bool freeFront = false;
			bool freeBack = false;
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
								const std::vector<core::SymbolCommand>& anchorBolts,
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

			// その通りの立上りの上に乗るアンカーボルト（芯からのずれが立上りの厚みの半分以内で、
			// どれかの区間の中にあるもの）。
			std::vector<double> points = cores;
			for (const core::SymbolCommand& bolt : anchorBolts)
			{
				const double across = eastWest ? bolt.position.y : bolt.position.x;
				const double along = eastWest ? bolt.position.x : bolt.position.y;
				if (std::abs(across - line.coord) > line.halfThickness + kDimensionMergeTol)
					continue;
				if (inSpans(line.spans, along, kDimensionMergeTol))
					points.push_back(along);
			}

			const auto snapToGrid = [&grid](const std::vector<double>& values)
			{
				std::vector<double> onGrid;
				std::ranges::copy_if(grid, std::back_inserter(onGrid),
									 [&values](double g) { return nearAny(values, g); });
				return unionStops(onGrid, values);
			};
			WallLineStops result{&line, snapToGrid(cores), snapToGrid(points)};
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
			return result;
		}

		// 東西の通り（eastWest=true）または南北の通りの測点。座標の昇順。
		std::vector<WallLineStops> wallLineStops(const std::vector<WallLine>& lines,
												 const std::vector<WallLine>& others,
												 const std::vector<core::SymbolCommand>& bolts,
												 const std::vector<core::GridCommand>& grids,
												 bool eastWest)
		{
			const std::vector<double> grid =
				gridStops(grids, eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical);
			std::vector<WallLineStops> out;
			out.reserve(lines.size());
			for (const WallLine& line : lines)
				out.push_back(lineStops(line, others, bolts, grid, eastWest));
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
												   const std::vector<core::SymbolCommand>& bolts,
												   const std::vector<core::GridCommand>& grids,
												   const core::Vec2& center)
		{
			std::vector<PlacedRun> out;
			for (const bool eastWest : {true, false})
			{
				const std::vector<WallLine>& runs = eastWest ? eastWestRuns : northSouthRuns;
				const std::vector<WallLineStops> stops = wallLineStops(
					runs, eastWest ? northSouthRuns : eastWestRuns, bolts, grids, eastWest);
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

		// 軸組図の高さの基準（GL・各階の FL・軒高）と、各階の標準の横架材天端。
		struct SectionHeights
		{
			std::vector<core::LevelMarkCommand> marks; // x は未設定（呼び出し側が入れる）
			std::vector<double> beamTops;
		};

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
					heights.beamTops.push_back(story.elevation);
					continue;
				}
				++floor;
				heights.marks.push_back(
					core::LevelMarkCommand{std::to_string(floor) + kLevelMarkFLSuffix,
										   story.elevation, 0.0, story.name, core::kLevelFL});
				for (const core::LevelCommand& level : story.levels)
				{
					if (level.type == core::kLevelBeamTop)
						heights.beamTops.push_back(story.elevation + level.offset);
				}
			}
			std::ranges::stable_sort(heights.marks, {}, &core::LevelMarkCommand::elevation);
			return heights;
		}

		// 軸組図の左に出す高さの列（GL・FL・軒高と、そこからの標準の横架材天端 → GL・FL・
		// 軒高の間隔）。left は図の左端（補助線の根元）。内側の段から順に並ぶ。レベル記号の
		// 起点をこの列より外へ出すので、寸法の命令とレベル記号の両方がここから段を知る。
		std::vector<DimensionChainCommand> sectionHeightChains(const SectionHeights& heights,
															   double left)
		{
			std::vector<double> levelZ;
			levelZ.reserve(heights.marks.size());
			for (const core::LevelMarkCommand& mark : heights.marks)
				levelZ.push_back(mark.elevation);
			levelZ = mergeStops(std::move(levelZ));
			const std::vector<double> withBeams = unionStops(levelZ, heights.beamTops);
			std::vector<DimensionChainCommand> out;
			int tier = 0;
			if (withBeams.size() >= 2 && !sameStops(withBeams, levelZ))
				out.push_back(makeChain(DimensionAxis::Vertical, withBeams, left, -1, tier++));
			if (levelZ.size() >= 2)
				out.push_back(makeChain(DimensionAxis::Vertical, levelZ, left, -1, tier++));
			return out;
		}

		// 切断面に乗る柱・横架材の、注釈空間の横の範囲。どちらも無ければ false。
		bool sectionAlongRange(const core::Document& document, const core::SectionCommand& section,
							   double& low, double& high)
		{
			const double origin = core::sectionAlongOrigin(section);
			bool any = false;
			const auto take = [&](const core::Vec2& plan)
			{
				const double along =
					core::sectionAnnotationPoint(plan, 0.0, section.direction, origin).x;
				low = any ? std::min(low, along) : along;
				high = any ? std::max(high, along) : along;
				any = true;
			};
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
			return any;
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

	std::vector<core::DimensionChainCommand>
	foundationDimensionChains(const std::vector<core::WallCommand>& walls,
							  const std::vector<core::SymbolCommand>& anchorBolts,
							  const std::vector<core::GridCommand>& grids, const core::Vec2& min,
							  const core::Vec2& max)
	{
		const std::vector<WallLine> eastWestLines = collectWallLines(walls, true);
		const std::vector<WallLine> northSouthLines = collectWallLines(walls, false);
		const std::vector<WallLine> eastWestRuns = splitIntoRuns(eastWestLines, northSouthLines);
		const std::vector<WallLine> northSouthRuns = splitIntoRuns(northSouthLines, eastWestLines);
		const core::Vec2 center{(min.x + max.x) / 2.0, (min.y + max.y) / 2.0};
		const std::vector<PlacedRun> runs =
			placeFoundationRuns(eastWestRuns, northSouthRuns, anchorBolts, grids, center);

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
		// 全長は立上りの芯の端から端（L 字などで最も外の通りが建物の全体に渡らなくても
		// 全体を測る）。
		const auto overallOf = [&runs](bool eastWest)
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
			const std::vector<double> overall = overallOf(eastWest);
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
		// アンカーボルトの絡む寸法は比べない（別の立上りのボルトがたまたま同じ位置にある
		// だけで、重なりではない）。外側から順に見て、書いた芯どうしの寸法を覚えていく。
		std::array<std::vector<std::pair<double, double>>, 2> written;
		const auto remember = [&written](DimensionAxis axis, const std::vector<double>& stops,
										 const std::vector<double>& cores)
		{
			for (std::size_t i = 0; i + 1 < stops.size(); ++i)
			{
				if (nearAny(cores, stops[i]) && nearAny(cores, stops[i + 1]))
					written.at(static_cast<std::size_t>(axis)).emplace_back(stops[i], stops[i + 1]);
			}
		};
		// 外周の 2 段目より外は芯と端だけでできている。
		for (const DimensionChainCommand& chain : out)
			remember(chain.axis, chain.stops, chain.stops);

		std::vector<const PlacedRun*> interior;
		for (const PlacedRun& run : runs)
		{
			if (run.stops.all.size() < 2)
				continue;
			if (run.exteriorSide == 0)
			{
				interior.push_back(&run);
				continue;
			}
			const DimensionAxis axis =
				run.eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical;
			const core::Vec2& edge = run.exteriorSide > 0 ? max : min;
			out.push_back(makeChain(axis, run.stops.all, run.eastWest ? edge.y : edge.x,
									run.exteriorSide, 0));
			remember(axis, run.stops.all, run.stops.cores);
		}

		// 内部の立上りの列は、その芯から図の外側（中心から遠い側）へ出す。
		const auto distance = [&center](const PlacedRun* run)
		{
			const double middle = run->eastWest ? center.y : center.x;
			return std::abs(run->stops.line->coord - middle);
		};
		std::ranges::stable_sort(interior, std::ranges::greater{}, distance);
		const auto isWritten = [&written](DimensionAxis axis, double a, double b)
		{
			return std::ranges::any_of(
				written.at(static_cast<std::size_t>(axis)),
				[a, b](const std::pair<double, double>& segment)
				{
					return std::abs(segment.first - a) <= kDimensionMergeTol &&
						   std::abs(segment.second - b) <= kDimensionMergeTol;
				});
		};
		for (const PlacedRun* run : interior)
		{
			const DimensionAxis axis =
				run->eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical;
			const double base = run->stops.line->coord;
			const double middle = run->eastWest ? center.y : center.x;
			const int side = base >= middle ? 1 : -1;
			const std::vector<double>& stops = run->stops.all;
			const std::vector<double>& cores = run->stops.cores;
			// 重なる寸法（芯・端どうしで、既に書いたもの）を抜いて、残りを続いている区間
			// ごとの列にし、書いた寸法を覚える。
			const auto emit = [&](const std::vector<double>& values, int tier)
			{
				std::vector<double> piece;
				const auto flush = [&]()
				{
					if (piece.size() >= 2)
						out.push_back(makeChain(axis, piece, base, side, tier));
					piece.clear();
				};
				for (std::size_t i = 0; i + 1 < values.size(); ++i)
				{
					if (nearAny(cores, values[i]) && nearAny(cores, values[i + 1]) &&
						isWritten(axis, values[i], values[i + 1]))
					{
						flush();
						continue;
					}
					if (piece.empty())
						piece.push_back(values[i]);
					piece.push_back(values[i + 1]);
				}
				flush();
				remember(axis, values, cores);
			};

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
				!isWritten(axis, cores.front(), cores.back()))
			{
				out.push_back(
					makeChain(axis, {cores.front(), cores.back()}, base, side, coreTier + 1));
				remember(axis, {cores.front(), cores.back()}, cores);
			}
		}
		return out;
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
			// （perimeterDimensionChains）は使わない。
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
			// 柱（小屋束を含む）と梁。
			for (const core::ColumnCommand& column : document.columns)
			{
				if (!onLayers(layers, column.layer))
					continue;
				elementX.push_back(column.position.x);
				elementY.push_back(column.position.y);
			}
			for (const core::MemberCommand& member : document.members)
			{
				if (onLayers(layers, member.layer))
					takeMember(member.start, member.end);
			}
			break;
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
		double low = 0.0;
		double high = 0.0;
		if (!sectionAlongRange(document, section, low, high))
			return {};
		double bottom = 0.0;
		double top = 0.0;
		if (!core::sectionHeightRange(document, bottom, top))
			return {};
		// 高さ範囲は上下に余白を足してあるので、建物の下端へ戻す。
		bottom += core::kSectionHeightMargin;

		const double origin = core::sectionAlongOrigin(section);
		const auto along = [&](const core::Vec2& plan)
		{ return core::sectionAnnotationPoint(plan, 0.0, section.direction, origin).x; };

		std::vector<DimensionChainCommand> out;

		// 横: 柱・束の位置（＋通り芯）。図の下に出す。通り芯の間隔だけの列は置かない
		// （部材の位置が分かれば足りる。伏図と同じ）。
		std::vector<double> columns;
		for (const core::ColumnCommand& column : document.columns)
		{
			if (columnOnCutPlane(column, section))
				columns.push_back(along(column.position));
		}
		// 切断面を横切る通り芯（X通りの断面なら東西に走る通り芯）のうち、図の範囲にあるもの。
		const DimensionAxis crossing = section.direction == core::SectionDirection::X
										   ? DimensionAxis::Vertical
										   : DimensionAxis::Horizontal;
		std::vector<double> grid;
		for (const double value : gridStops(document.grids, crossing))
		{
			const double a = value - origin;
			if (a >= low - kDimensionMergeTol && a <= high + kDimensionMergeTol)
				grid.push_back(a);
		}
		grid = mergeStops(std::move(grid));
		const std::vector<double> detail = unionStops(grid, columns);
		if (detail.size() >= 2)
			out.push_back(makeChain(DimensionAxis::Horizontal, detail, bottom, -1, 0));

		// 縦: GL・FL・軒高と、そこからの標準の横架材天端 → GL・FL・軒高の間隔。図の左に出す。
		for (DimensionChainCommand& chain :
			 sectionHeightChains(sectionHeights(document.stories), low))
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
			const auto level = levels.find(member.layer);
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
		double low = 0.0;
		double high = 0.0;
		if (!sectionAlongRange(document, section, low, high))
			return {};
		const SectionHeights heights = sectionHeights(document.stories);
		// 左の高さの列の最も外の段（列が無ければ -1）。記号の名前をこれより外へ出す。
		const int outerTier = static_cast<int>(sectionHeightChains(heights, low).size()) - 1;
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
