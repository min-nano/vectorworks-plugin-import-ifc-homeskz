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
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 軸方向の XY 成分がこれ以下だと向きを決められないため既定（上）を使う
		// （Python 版 _DIR_TOL）。
		constexpr double kDirTol = 1e-9;

		// 「上または左」の比較を Python 版（小数 9 桁へ丸めたタプル比較）と同じ粒度で
		// 行うための丸め。厳密な等値比較を避けつつ、`py` がほぼ 0（材が南北向き）のときに
		// 「左」の判定へ落ちるようにする。
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

		// 横架材がその切断面に乗るか（＝断面に立面として写るか。parse/Tag.h「軸組図のタグ」）。
		// 通りに沿って走り、かつ芯が切断位置にある材だけを対象にする。判定の許容は
		// **切断位置を作ったときと同じ** kClusterTol を使う（同じ通りに乗る材の散らばりを
		// 吸収する値なので、別の定数を増やさない）。
		bool MemberOnCutPlane(const core::MemberCommand& member,
							  const core::SectionCommand& section)
		{
			const double dx = member.end.x - member.start.x;
			const double dy = member.end.y - member.start.y;
			const bool alongCut = section.direction == core::SectionDirection::X
									  ? std::abs(dx) < std::abs(dy)
									  : std::abs(dy) < std::abs(dx);
			if (!alongCut)
				return false;

			const double cut = CutCoord(section.lineStart, section.direction);
			const double centre = (CutCoord(member.start, section.direction) +
								   CutCoord(member.end, section.direction)) /
								  2.0;
			return std::abs(centre - cut) <= kClusterTol;
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

		// 軸直交（±90 度回転）の 2 候補。py が大きい（上）方を選び、同等なら px が小さい
		// （左）方を選ぶ（Python 版 _offset_side のタプル比較と同じ規則）。
		const double px = -dy / length;
		const double py = dx / length;
		const double up = RoundToCompare(py);
		const double down = RoundToCompare(-py);
		if (down > up || (!(up > down) && px > 0.0))
			return core::Vec2{-px, -py};
		return core::Vec2{px, py};
	}

	double sectionAlongOrigin(const core::SectionCommand& section)
	{
		// 断面線の**終点**（画面右の端）の、切断線に沿った座標。ここが注釈空間の横方向の
		// 原点（parse/Tag.h「断面の注釈空間」）。
		return section.direction == core::SectionDirection::X ? section.lineEnd.y
															  : section.lineEnd.x;
	}

	core::Vec2 sectionAnnotationPoint(const core::Vec2& plan, double elevation,
									  core::SectionDirection direction, double alongOrigin)
	{
		// 画面右方向は視線の向きが決める（parse/Tag.h「断面の注釈空間」）。X通りは −X 方向を
		// 見るので右が +Y、Y通りは +Y 方向を見るので右が +X。**横は断面線の終点からの距離**、
		// 高さはそのまま Z。
		const double right = direction == core::SectionDirection::X ? plan.y : plan.x;
		return core::Vec2{right - alongOrigin, elevation};
	}

	std::vector<core::TagCommand>
	buildPlanTagCommands(const std::vector<core::MemberCommand>& members,
						 const core::ViewportCommand& viewport)
	{
		std::vector<core::TagCommand> commands;
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const core::MemberCommand& member = members[i];
			// その伏図が映すレイヤに乗る横架材だけにタグを置く（Python 版 execute_sheets の
			// 振り分けと同じ条件を、こちらは解析側で済ませる）。
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
			commands.push_back(std::move(tag));
		}
		return commands;
	}

	std::vector<core::TagCommand>
	buildSectionTagCommands(const std::vector<core::MemberCommand>& members,
							const core::SectionCommand& section)
	{
		std::vector<core::TagCommand> commands;
		const double alongOrigin = sectionAlongOrigin(section);
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const core::MemberCommand& member = members[i];
			if (!MemberOnCutPlane(member, section))
				continue;

			// 断面に写る天端線（命令の start/end を注釈空間へ投影したもの）。その中点に
			// タグの下端中央が来る＝部材の上辺に接する（伏図で辺の中央へ寄せるのと同じ意図）。
			const core::Vec2 start = sectionAnnotationPoint(member.start, member.elevation,
															section.direction, alongOrigin);
			const core::Vec2 end = sectionAnnotationPoint(member.end, member.endElevation,
														  section.direction, alongOrigin);

			core::TagCommand tag;
			tag.memberIndex = i;
			tag.position = core::Vec2{(start.x + end.x) / 2.0, (start.y + end.y) / 2.0};
			// 断面では天端線がそのまま部材の上辺なので、逃がす向きは**その線の法線のうち
			// 上を向く側**（水平材なら真上）。伏図で「上または左」へ寄せるのと同じ意図。
			tag.offset = upwardNormal(end.x - start.x, end.y - start.y);
			// 傾斜材（登り梁・隅木）は立面でも傾くので、文字も天端線に沿わせる。
			tag.angle = tagAngle(end.x - start.x, end.y - start.y);
			commands.push_back(std::move(tag));
		}
		return commands;
	}

	void attachTagCommands(core::Document& document)
	{
		for (core::SheetCommand& sheet : document.sheets)
			sheet.viewport.tags = buildPlanTagCommands(document.members, sheet.viewport);
		for (core::SectionCommand& section : document.sections)
			section.viewport.tags = buildSectionTagCommands(document.members, section);
	}
} // namespace HomeskzIfcImport::parse
