//
//	parse/Tag.cpp
//
//	断面寸法データタグ命令の組み立ての実装。意図・規約は parse/Tag.h を参照。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Tag.h"
#include "parse/Section.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 軸方向の XY 成分がこれ以下だと向きを決められないため既定（上）を使う。
		constexpr double kDirTol = 1e-9;

		// 「上または左」の比較を小数 9 桁の粒度で行うための丸め。厳密な等値比較を避けつつ、
		// `py` がほぼ 0（材が南北向き）のときに「左」の判定へ落ちるようにする。
		double RoundToCompare(double value)
		{
			constexpr double kScale = 1e9;
			return std::round(value * kScale) / kScale;
		}

		// 平面座標のうち、その断面が「切る」軸の値を返す（X通り＝X、Y通り＝Y）。
		double CutCoord(const core::Vec2& point, core::SectionDirection direction)
		{
			return direction == core::SectionDirection::X ? point.x : point.y;
		}
	} // namespace

	double tagAngle(double dx, double dy)
	{
		double angle = std::atan2(dy, dx) * 180.0 / std::numbers::pi;
		while (angle > 90.0)
			angle -= 180.0;
		while (angle <= -90.0)
			angle += 180.0;
		return angle;
	}

	core::Vec2 upwardNormal(double du, double dv)
	{
		const double length = std::hypot(du, dv);
		if (length <= kDirTol)
			return core::Vec2{0.0, 1.0};
		// 線の法線 2 候補のうち上を向く側。真横（法線が水平）になることは天端線では
		// 起きない（鉛直な天端線＝長さ 0 の投影）ので、上下の判定だけで足りる。
		const double nx = -dv / length;
		const double ny = du / length;
		return ny < 0.0 ? core::Vec2{-nx, -ny} : core::Vec2{nx, ny};
	}

	core::Vec2 tagOffsetSide(double dx, double dy)
	{
		const double length = std::hypot(dx, dy);
		if (length <= kDirTol)
			return core::Vec2{0.0, 1.0};

		// 軸直交（±90 度回転）の 2 候補。py が大きい（上）方を選び、同等なら px が小さい（左）
		// 方を選ぶ。
		const double px = -dy / length;
		const double py = dx / length;
		const double up = RoundToCompare(py);
		const double down = RoundToCompare(-py);
		if (down > up || (!(up > down) && px > 0.0))
			return core::Vec2{-px, -py};
		return core::Vec2{px, py};
	}

	// 通りに沿って走り、かつ芯が切断位置にある材だけを対象にする。判定の許容は
	// **切断位置を作ったときと同じ** kClusterTol を使う（同じ通りに乗る材の散らばりを
	// 吸収する値なので、別の定数を増やさない）。
	bool memberOnCutPlane(const core::MemberCommand& member, const core::SectionCommand& section)
	{
		const double dx = member.end.x - member.start.x;
		const double dy = member.end.y - member.start.y;
		const bool alongCut = section.direction == core::SectionDirection::X
								  ? std::abs(dx) < std::abs(dy)
								  : std::abs(dy) < std::abs(dx);
		if (!alongCut)
			return false;

		const double cut = CutCoord(section.lineStart, section.direction);
		const double centre =
			(CutCoord(member.start, section.direction) + CutCoord(member.end, section.direction)) /
			2.0;
		return std::abs(centre - cut) <= kClusterTol;
	}

	bool columnOnCutPlane(const core::ColumnCommand& column, const core::SectionCommand& section)
	{
		// 横架材と同じ許容（kClusterTol）。切断位置は柱の座標を束ねて作ったものなので、
		// その通りに乗る柱は必ずこの幅に入る。
		const double cut = CutCoord(section.lineStart, section.direction);
		return std::abs(CutCoord(column.position, section.direction) - cut) <= kClusterTol;
	}

	bool memberCrossesCutPlane(const core::MemberCommand& member,
							   const core::SectionCommand& section, core::Vec2& crossing)
	{
		if (memberOnCutPlane(member, section))
			return false;
		const double cut = CutCoord(section.lineStart, section.direction);
		const double a = CutCoord(member.start, section.direction);
		const double b = CutCoord(member.end, section.direction);
		if (std::min(a, b) > cut + kClusterTol || std::max(a, b) < cut - kClusterTol)
			return false;
		// 芯と切断面の交点。切断面で止まる材（両端とも片側）は近い端へ寄せる。
		const double t =
			std::abs(b - a) > kDirTol ? std::clamp((cut - a) / (b - a), 0.0, 1.0) : 0.0;
		crossing = core::Vec2{member.start.x + ((member.end.x - member.start.x) * t),
							  member.start.y + ((member.end.y - member.start.y) * t)};
		return true;
	}

	std::vector<core::TagCommand>
	buildPlanTagCommands(const std::vector<core::MemberCommand>& members,
						 const core::ViewportCommand& viewport)
	{
		std::vector<core::TagCommand> commands;
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const core::MemberCommand& member = members[i];
			// その伏図が映すレイヤに乗る横架材だけにタグを置く。
			if (std::ranges::find(viewport.layers, member.layer) == viewport.layers.end())
				continue;

			const double dx = member.end.x - member.start.x;
			const double dy = member.end.y - member.start.y;
			const core::Vec2 side = tagOffsetSide(dx, dy);
			// 軸中央から部材の面（断面幅/2）まで寄せた点＝**部材の辺の中央**。ここにタグの
			// 下端中央が接する（余白を足さず面ちょうどに置くことで引出線が出ない。parse/Tag.h）。
			// タグ自身の大きさぶんの逃がしは描画側が実寸を測って足す（core/Document.h の
			// TagCommand）。
			const double half = member.width / 2.0;

			core::TagCommand tag;
			tag.memberIndex = i;
			tag.position = core::Vec2{(member.start.x + member.end.x) / 2.0 + side.x * half,
									  (member.start.y + member.end.y) / 2.0 + side.y * half};
			tag.offset = side;
			tag.angle = tagAngle(dx, dy);
			commands.push_back(tag);
		}
		return commands;
	}

	std::vector<core::TagCommand>
	buildSectionTagCommands(const std::vector<core::MemberCommand>& members,
							const core::SectionCommand& section)
	{
		std::vector<core::TagCommand> commands;
		const double alongOrigin = core::sectionAlongOrigin(section);
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const core::MemberCommand& member = members[i];
			if (!memberOnCutPlane(member, section))
				continue;

			// 断面に写る天端線（命令の start/end を注釈空間へ投影したもの）。その中点に
			// タグの下端中央が来る＝部材の上辺に接する（伏図で辺の中央へ寄せるのと同じ意図）。
			const core::Vec2 start = core::sectionAnnotationPoint(member.start, member.elevation,
																  section.direction, alongOrigin);
			const core::Vec2 end = core::sectionAnnotationPoint(member.end, member.endElevation,
																section.direction, alongOrigin);

			core::TagCommand tag;
			tag.memberIndex = i;
			tag.position = core::Vec2{(start.x + end.x) / 2.0, (start.y + end.y) / 2.0};
			// 断面では天端線がそのまま部材の上辺なので、逃がす向きは**その線の法線のうち
			// 上を向く側**（水平材なら真上）。伏図で「上または左」へ寄せるのと同じ意図。
			tag.offset = upwardNormal(end.x - start.x, end.y - start.y);
			// 傾斜材（登り梁・隅木）は立面でも傾くので、文字も天端線に沿わせる。
			tag.angle = tagAngle(end.x - start.x, end.y - start.y);
			commands.push_back(tag);
		}
		return commands;
	}

	std::string memberLevelNote(const core::MemberCommand& member,
								const std::vector<StoryInfo>& stories,
								const std::vector<long long>& standardHeights)
	{
		if (member.hipOrValley)
			return {};
		// 階はレイヤ名の接頭辞（"2-横架材天端(FL-872)" → "2"）から引く。伏図レベルの印は
		// 接頭辞の後ろなので、外さなくても接頭辞は変わらない。
		const std::size_t dash = member.layer.find('-');
		if (dash == std::string::npos)
			return {};
		const std::string prefix = member.layer.substr(0, dash);
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			if (storyLayerPrefix(i, story.isTop) != prefix)
				continue;
			const long long fl = std::llround(story.elevation);
			const long long low = std::llround(std::min(member.elevation, member.endElevation));
			const long long high = std::llround(std::max(member.elevation, member.endElevation));
			// 名前が取れない階は番号で呼ぶ（IFC の階名は "…FL" で終わるものしか採らない。
			// parse/Story の collectStories）。
			const std::string name =
				story.name.empty() ? storyLayerPrefix(i, story.isTop) + "FL" : story.name;
			if (low == high)
			{
				// 水平な材は標準の横架材の高さと違うときだけ（高さが分からない階は推した値）。
				const long long standard = i < standardHeights.size()
											   ? standardHeights[i]
											   : std::llround(beamTopElevation(story));
				if (low == standard)
					return {};
				return "(" + name + " " + core::signedMillimetreText(low - fl) + ")";
			}
			return "(" + name + " " + core::signedMillimetreText(low - fl) + "~" +
				   core::signedMillimetreText(high - fl) + ")";
		}
		return {};
	}

	void attachTagCommands(core::Document& document)
	{
		attachTagCommands(document, {}, {});
	}

	void attachTagCommands(core::Document& document, const std::vector<StoryInfo>& stories,
						   const std::vector<long long>& standardHeights)
	{
		// 注記は材ごとに 1 度だけ求め、その材のタグ（伏図・軸組図）すべてへ配る。
		std::vector<std::string> notes(document.members.size());
		for (std::size_t i = 0; i < document.members.size(); ++i)
			notes[i] = memberLevelNote(document.members[i], stories, standardHeights);
		const auto withNotes = [&notes](std::vector<core::TagCommand> tags)
		{
			for (core::TagCommand& tag : tags)
			{
				if (tag.memberIndex < notes.size())
					tag.note = notes[tag.memberIndex];
			}
			return tags;
		};
		for (core::SheetCommand& sheet : document.sheets)
			sheet.viewport.tags = withNotes(buildPlanTagCommands(document.members, sheet.viewport));
		for (core::SectionCommand& section : document.sections)
			section.viewport.tags = withNotes(buildSectionTagCommands(document.members, section));
	}
} // namespace HomeskzIfcImport::parse
