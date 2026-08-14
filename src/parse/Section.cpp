//
//	parse/Section.cpp
//
//	軸組図（断面ビューポート）の解析。Python 版 ifc/section.py に対応する（ROADMAP.md M14）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	このモジュールも parse/Sheet と同じく IFC の幾何をほとんど見ない——**他のモジュールが
//	既に出した答え**（柱・横架材の命令／通り芯の線分／ストーリの作るレイヤ）を組み合わせて
//	「どこを切り、どう名付け、何を映すか」を決める。IFC を直接見るのは通り芯の線分だけで、
//	それも parse/Grid（共有コンテキスト）から受け取る。
//

#include "parse/Section.h"
#include "core/Document.h"
#include "parse/Context.h"
#include "parse/Grid.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		using core::SectionDirection;

		// 切断位置の判定で「梁」とみなさない横架材のクラス（Python 版 _NON_BEAM_CLASSES）。
		// 大引（床組）・母屋（小屋組）は軸組の主要材ではないので、それらだけが柱と重なる
		// 通りは軸組図にしない。
		bool isNonBeamClass(const std::string& drawClass)
		{
			return drawClass == CLASS_OOBIKI || drawClass == CLASS_MOYA;
		}

		// いろは順の 48 文字（Python 版 _IROHA）。通り芯名がこれだけで構成されていれば
		// 「いろは書式」とみなし、中間の通りを `又` で連番する。
		constexpr const char* kIroha =
			"いろはにほへとちりぬるをわかよたれそつねならむうゐのおくやま"
			"けふこえてあさきゆめみしゑひもせす";

		// UTF-8 文字列を 1 文字（コードポイント）ずつに割る。壊れた並びに出会ったら
		// そこで打ち切る（名前の判定に使うだけなので、判定が false 側へ倒れれば十分）。
		std::vector<std::string> splitUtf8(const std::string& text)
		{
			std::vector<std::string> chars;
			for (std::size_t i = 0; i < text.size();)
			{
				const auto lead = static_cast<unsigned char>(text[i]);
				std::size_t length = 1;
				if ((lead & 0x80U) == 0U)
					length = 1;
				else if ((lead & 0xE0U) == 0xC0U)
					length = 2;
				else if ((lead & 0xF0U) == 0xE0U)
					length = 3;
				else if ((lead & 0xF8U) == 0xF0U)
					length = 4;
				else
					break; // 継続バイトが先頭に来た＝壊れている
				if (i + length > text.size())
					break;
				chars.push_back(text.substr(i, length));
				i += length;
			}
			return chars;
		}

		// 通り芯名がいろは文字だけで構成されるか（Python 版 _is_iroha_name）。
		bool isIrohaName(const std::string& name)
		{
			static const std::vector<std::string> iroha = splitUtf8(kIroha);
			const std::vector<std::string> chars = splitUtf8(name);
			if (chars.empty())
				return false;
			return std::ranges::all_of(chars, [](const std::string& c)
									   { return std::ranges::find(iroha, c) != iroha.end(); });
		}

		// 座標に付く種別（柱か梁か）。クラスタが両方を含むかだけを見る。
		struct Tagged
		{
			double coord = 0.0;
			bool column = false;
		};

		// 1 つのクラスタ（近い座標の集まり）。
		struct Cluster
		{
			double coord = 0.0;
			bool hasColumn = false;
			bool hasBeam = false;
		};

		// 座標を昇順に並べ、隣との差が tol を超えるところで切ってクラスタにする
		// （Python 版 _clusters）。クラスタの座標はその平均＝柱梁の芯。
		std::vector<Cluster> clusterCoords(std::vector<Tagged> tagged, double tol)
		{
			if (tagged.empty())
				return {};
			// 座標の昇順（同座標では柱を先に）。**入力の並び順に依存しない**結果にするため、
			// 比較は値だけで決める（CLAUDE.md「決定性を守る」）。
			std::ranges::sort(tagged,
							  [](const Tagged& a, const Tagged& b)
							  {
								  if (a.coord < b.coord)
									  return true;
								  if (b.coord < a.coord)
									  return false;
								  return a.column && !b.column;
							  });

			std::vector<Cluster> clusters;
			double sum = 0.0;
			std::size_t count = 0;
			double last = tagged.front().coord;
			Cluster current;
			// 溜まっている座標を 1 クラスタとして確定する。**呼ぶ時点で必ず 1 件以上ある**
			// （空の入力は上で弾き、ループ内では隙間を見つけたときだけ呼ぶ）ので、
			// 0 件の番人は置かない。
			const auto flush = [&]()
			{
				current.coord = sum / static_cast<double>(count);
				clusters.push_back(current);
				current = Cluster{};
				sum = 0.0;
				count = 0;
			};
			for (const Tagged& item : tagged)
			{
				if (count > 0 && item.coord - last > tol)
					flush();
				sum += item.coord;
				++count;
				last = item.coord;
				if (item.column)
					current.hasColumn = true;
				else
					current.hasBeam = true;
			}
			flush();
			return clusters;
		}

		// 通り芯（センタリング済み）の平面 bbox。指示線をどこまで延ばすかを決める。
		struct PlanBounds
		{
			double minX = 0.0;
			double maxX = 0.0;
			double minY = 0.0;
			double maxY = 0.0;
		};

		// 通り芯（センタリング済み）の平面 bbox。線分が 1 本も無ければ false。
		bool gridPlanBounds(const std::vector<GridLine>& lines, const core::Vec2& center,
							PlanBounds& bounds)
		{
			if (lines.empty())
				return false;
			bounds.minX = std::numeric_limits<double>::max();
			bounds.maxX = std::numeric_limits<double>::lowest();
			bounds.minY = bounds.minX;
			bounds.maxY = bounds.maxX;
			for (const GridLine& line : lines)
			{
				for (const core::Vec2& point : {line.start, line.end})
				{
					bounds.minX = std::min(bounds.minX, point.x - center.x);
					bounds.maxX = std::max(bounds.maxX, point.x - center.x);
					bounds.minY = std::min(bounds.minY, point.y - center.y);
					bounds.maxY = std::max(bounds.maxY, point.y - center.y);
				}
			}
			return true;
		}

		// 1 方向ぶんの section 命令を組み立てる。
		std::vector<core::SectionCommand>
		commandsForDirection(SectionDirection direction, const std::vector<double>& cuts,
							 const std::vector<NamedAxis>& axes, const PlanBounds& bounds,
							 const std::vector<std::string>& layers)
		{
			const std::vector<std::string> names = nameSectionCuts(cuts, axes);
			std::vector<core::SectionCommand> commands;
			commands.reserve(cuts.size());
			for (std::size_t i = 0; i < cuts.size(); ++i)
			{
				const double cut = cuts[i];
				core::SectionCommand command;
				command.number = kSectionSheetNumber;
				command.title = kSectionSheetTitle;
				command.direction = direction;
				if (direction == SectionDirection::X)
				{
					// X通り＝定 X の切断面。指示線は Y 方向へ（建物の外まで）延ばす。
					command.lineStart = core::Vec2{cut, bounds.minY - kSectionLineMargin};
					command.lineEnd = core::Vec2{cut, bounds.maxY + kSectionLineMargin};
					// 視線は −X 方向（＝建物の東側から西を見る）。こうすると図面の右へ
					// Y 座標が増える＝通り名（い・ろ・は…）が左から右へ並ぶ。
					command.viewPoint =
						core::Vec2{cut - kViewPointOffset, (bounds.minY + bounds.maxY) / 2.0};
				}
				else
				{
					// Y通り＝定 Y の切断面。指示線は X 方向へ延ばす。
					command.lineStart = core::Vec2{bounds.minX - kSectionLineMargin, cut};
					command.lineEnd = core::Vec2{bounds.maxX + kSectionLineMargin, cut};
					// 視線は +Y 方向（＝建物の南側から北を見る）。図面の右へ X 座標が
					// 増える＝通り名（X1・X2…）が左から右へ並ぶ。
					command.viewPoint =
						core::Vec2{(bounds.minX + bounds.maxX) / 2.0, cut + kViewPointOffset};
				}
				command.viewport.drawingNumber = names[i];
				command.viewport.drawingTitle = names[i] + kSectionTitleSuffix;
				command.viewport.layers = layers;
				commands.push_back(std::move(command));
			}
			return commands;
		}
	} // namespace

	std::vector<NamedAxis> namedAxes(const std::vector<GridLine>& lines, const core::Vec2& center,
									 SectionDirection direction)
	{
		// 同名の通り芯が複数区間に分かれていても 1 本にまとめる（最初に現れた区間の中点）。
		std::vector<NamedAxis> axes;
		std::set<std::string> seen;
		for (const GridLine& line : lines)
		{
			if (line.label.empty())
				continue;
			// X 通りの線は定 X（縦線）なので座標は X、Y 通りは座標が Y。方向の判定は
			// 通り芯のクラス分けと同じ述語（parse/Grid の isXAxis）を通す。
			const bool xAxis = isXAxis(line);
			if (xAxis != (direction == SectionDirection::X))
				continue;
			if (seen.contains(line.label))
				continue;
			const double coord = xAxis ? ((line.start.x + line.end.x) / 2.0) - center.x
									   : ((line.start.y + line.end.y) / 2.0) - center.y;
			seen.insert(line.label);
			axes.push_back(NamedAxis{line.label, coord});
		}
		// 座標の昇順（同座標なら名前順）。入力の並び順に依存しない決定的な並びにする。
		std::ranges::sort(axes,
						  [](const NamedAxis& a, const NamedAxis& b)
						  {
							  if (a.coord < b.coord)
								  return true;
							  if (b.coord < a.coord)
								  return false;
							  return a.name < b.name;
						  });
		return axes;
	}

	std::vector<double> sectionCutPositions(const std::vector<core::ColumnCommand>& columns,
											const std::vector<core::MemberCommand>& members,
											SectionDirection direction)
	{
		const bool xDirection = direction == SectionDirection::X;
		std::vector<Tagged> tagged;
		tagged.reserve(columns.size() + members.size());
		for (const core::ColumnCommand& column : columns)
			tagged.push_back(Tagged{xDirection ? column.position.x : column.position.y, true});
		for (const core::MemberCommand& member : members)
		{
			if (isNonBeamClass(member.drawClass))
				continue;
			const double dx = member.end.x - member.start.x;
			const double dy = member.end.y - member.start.y;
			// X通りは Y 方向に走る梁（|Δx| < |Δy|）、Y通りは X 方向に走る梁を見る
			// ——切断面に**平行**な梁だけがその通りの軸組を成す。
			const bool runsY = std::abs(dx) < std::abs(dy);
			if (xDirection != runsY)
				continue;
			const double coord = xDirection ? (member.start.x + member.end.x) / 2.0
											: (member.start.y + member.end.y) / 2.0;
			tagged.push_back(Tagged{coord, false});
		}

		std::vector<double> cuts;
		for (const Cluster& cluster : clusterCoords(std::move(tagged), kClusterTol))
		{
			if (cluster.hasColumn && cluster.hasBeam)
				cuts.push_back(cluster.coord);
		}
		return cuts;
	}

	std::vector<std::string> nameSectionCuts(const std::vector<double>& cuts,
											 const std::vector<NamedAxis>& axes)
	{
		std::vector<std::string> names;
		names.reserve(cuts.size());
		// 基準にしている通り芯（axes の添字）。座標そのものではなく添字で持つのは、
		// 「基準が別の通りへ移ったか」を実数の等値比較なしで判定するため。
		std::optional<std::size_t> base;
		int counter = 0;
		for (const double cut : cuts)
		{
			// 名前付き通り芯に一致すればその名前（以降の中間通りはこの通りを基準にする）。
			const auto match =
				std::ranges::find_if(axes, [cut](const NamedAxis& axis)
									 { return std::abs(axis.coord - cut) <= kAxisMatchTol; });
			if (match != axes.end())
			{
				base = static_cast<std::size_t>(std::distance(axes.begin(), match));
				counter = 0;
				names.push_back(match->name);
				continue;
			}
			// 中間の通り: 直前（座標の小さい側）の通り芯を基準に連番する。axes は昇順なので
			// 条件を満たす最後のものが「直前」。
			std::optional<std::size_t> preceding;
			for (std::size_t i = 0; i < axes.size(); ++i)
			{
				if (axes[i].coord < cut - kAxisMatchTol)
					preceding = i;
			}
			if (preceding.has_value())
			{
				if (base != preceding)
				{
					base = preceding;
					counter = 0;
				}
			}
			else if (!base.has_value() && !axes.empty())
			{
				// 最初の通り芯より手前の中間通り（まれ）。先頭の通り芯を基準にする。
				base = 0U;
			}
			++counter;
			if (!base.has_value())
			{
				// 名前付き通り芯が 1 本も無い図面。連番だけを名前にする。
				names.push_back(std::to_string(counter));
				continue;
			}
			const std::string& baseName = axes[*base].name;
			if (isIrohaName(baseName))
			{
				// いろは書式は「又」を前置して増やす（又い → 又又い）。
				std::string name;
				for (int i = 0; i < counter; ++i)
					name += "又";
				names.push_back(name + baseName);
			}
			else
			{
				// 数字書式はダッシュを後置して増やす（X1' → X1''）。
				std::string name = baseName;
				for (int i = 0; i < counter; ++i)
					name += "'";
				names.push_back(std::move(name));
			}
		}
		return names;
	}

	std::vector<std::string> sectionLayers(const std::vector<core::StoryCommand>& stories)
	{
		std::vector<std::string> layers;
		for (const core::StoryCommand& story : stories)
		{
			for (const core::LevelCommand& level : story.levels)
			{
				if (!level.layer.empty())
					layers.push_back(level.layer);
			}
		}
		if (!layers.empty())
			layers.emplace_back(core::kGridLayer);
		return layers;
	}

	std::vector<core::SectionCommand> buildSectionCommands(Context& context,
														   const core::Document& document)
	{
		// 通り芯が無いと平面の広がり（指示線の長さ）も通り名も決められないので作らない
		// （Python 版 build_section_commands と同じ）。
		PlanBounds bounds;
		if (!gridPlanBounds(context.gridLines(), context.gridCenter(), bounds))
			return {};

		// 映すレイヤが無い（ストーリを作れていない）なら、断面には何も出ないので作らない。
		const std::vector<std::string> layers = sectionLayers(document.stories);
		if (layers.empty())
			return {};

		const std::vector<double> xCuts =
			sectionCutPositions(document.columns, document.members, SectionDirection::X);
		const std::vector<double> yCuts =
			sectionCutPositions(document.columns, document.members, SectionDirection::Y);
		const std::vector<NamedAxis> xAxes =
			namedAxes(context.gridLines(), context.gridCenter(), SectionDirection::X);
		const std::vector<NamedAxis> yAxes =
			namedAxes(context.gridLines(), context.gridCenter(), SectionDirection::Y);

		std::vector<core::SectionCommand> commands =
			commandsForDirection(SectionDirection::X, xCuts, xAxes, bounds, layers);
		for (core::SectionCommand& command :
			 commandsForDirection(SectionDirection::Y, yCuts, yAxes, bounds, layers))
			commands.push_back(std::move(command));
		return commands;
	}

	// --- const Model& を直接取るオーバーロード（単体テスト用。内部でコンテキストを作って
	// 捨てる＝従来どおりの挙動。CLAUDE.md「共有コンテキスト」）-----------------------
	std::vector<core::SectionCommand> buildSectionCommands(const Model& model,
														   const core::Document& document)
	{
		Context context(model);
		return buildSectionCommands(context, document);
	}
} // namespace HomeskzIfcImport::parse
