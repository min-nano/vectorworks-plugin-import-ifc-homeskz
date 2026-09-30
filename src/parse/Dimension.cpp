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

		// 通り芯の間の位置を、その値を挟む通り芯へ広げる。立上りに沿う列の両端を
		// 通り芯で閉じる（端の測点が「どこから測ったか」を読めるようにする）。
		std::vector<double> gridsAround(const std::vector<double>& grid, double low, double high)
		{
			std::vector<double> out;
			for (const double value : grid)
			{
				if (value >= low - kDimensionMergeTol && value <= high + kDimensionMergeTol)
					out.push_back(value);
			}
			// 区間の外側で最も近い通り芯（あれば）も足す。
			const auto below = std::ranges::find_if(grid.rbegin(), grid.rend(), [low](double v)
													{ return v < low - kDimensionMergeTol; });
			if (below != grid.rend())
				out.push_back(*below);
			const auto above = std::ranges::find_if(grid, [high](double v)
													{ return v > high + kDimensionMergeTol; });
			if (above != grid.end())
				out.push_back(*above);
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
	foundationWallDimensionChains(const std::vector<core::WallCommand>& walls,
								  const std::vector<core::SymbolCommand>& anchorBolts,
								  const std::vector<core::GridCommand>& grids,
								  const core::Vec2& center)
	{
		std::vector<DimensionChainCommand> out;
		for (const bool eastWest : {true, false})
		{
			// 東西の通りは X を測り、南北に走る通り芯と交わる。
			const DimensionAxis axis =
				eastWest ? DimensionAxis::Horizontal : DimensionAxis::Vertical;
			const std::vector<double> grid = gridStops(grids, axis);
			for (const WallLine& line : collectWallLines(walls, eastWest))
			{
				std::vector<double> points;
				for (const auto& [a, b] : line.spans)
				{
					points.push_back(a);
					points.push_back(b);
				}
				// その通りの立上りの上に乗るアンカーボルト（芯からのずれが立上りの厚みの
				// 半分以内で、どれかの区間の中にあるもの）。
				for (const core::SymbolCommand& bolt : anchorBolts)
				{
					const double across = eastWest ? bolt.position.y : bolt.position.x;
					const double along = eastWest ? bolt.position.x : bolt.position.y;
					if (std::abs(across - line.coord) > line.halfThickness + kDimensionMergeTol)
						continue;
					const bool inside =
						std::ranges::any_of(line.spans,
											[along](const std::pair<double, double>& span) {
												return along >= span.first - kDimensionMergeTol &&
													   along <= span.second + kDimensionMergeTol;
											});
					if (inside)
						points.push_back(along);
				}

				// 通り芯と重ならない測点が無ければ作らない（通り芯の間隔を繰り返すだけ）。
				const bool anyOffGrid = std::ranges::any_of(points, [&grid](double value)
															{ return !nearAny(grid, value); });
				if (!anyOffGrid)
					continue;

				const double low = line.spans.front().first;
				const double high =
					std::ranges::max(line.spans, {}, &std::pair<double, double>::second).second;
				std::vector<double> stops = unionStops(gridsAround(grid, low, high), points);
				if (stops.size() < 2)
					continue;
				// 図の中心から遠い側（＝外側）へ出す。
				const double middle = eastWest ? center.y : center.x;
				const int side = line.coord >= middle ? 1 : -1;
				out.push_back(makeChain(axis, std::move(stops), line.coord, side, 0));
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

		int firstTier = 0;
		std::vector<DimensionChainCommand> local;
		switch (sheet.kind)
		{
		case core::PlanKind::Foundation:
		{
			// 立上りの通り（外周の列）と、立上りに沿う列（アンカーボルト・切れ目）。
			std::vector<core::WallCommand> walls;
			std::ranges::copy_if(document.walls, std::back_inserter(walls),
								 [&layers](const core::WallCommand& wall)
								 { return onLayers(layers, wall.layer); });
			std::vector<core::SymbolCommand> bolts;
			std::ranges::copy_if(document.anchorBolts, std::back_inserter(bolts),
								 [&layers](const core::SymbolCommand& bolt)
								 { return onLayers(layers, bolt.layer); });
			for (const core::WallCommand& wall : walls)
				takeMember(wall.start, wall.end);
			const core::Vec2 center{(min.x + max.x) / 2.0, (min.y + max.y) / 2.0};
			local = foundationWallDimensionChains(walls, bolts, document.grids, center);
			// 外周の立上りに沿う列が 1 段目を占めるので、外周の列は 2 段目から。
			firstTier = 1;
			break;
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
			gridStops(document.grids, DimensionAxis::Vertical), min, max, firstTier);
		out.insert(out.end(), std::make_move_iterator(local.begin()),
				   std::make_move_iterator(local.end()));
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
