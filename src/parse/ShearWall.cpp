//
//	parse/ShearWall.cpp
//
//	耐力壁解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。意図と規約は parse/ShearWall.h を参照。
//

#include "parse/ShearWall.h"
#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
#include "parse/IfcGeometry.h"
#include "parse/StructuralClass.h"
#include "parse/Story.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::ShearWallCommand;
	using core::Vec2;
	using core::Vec3;

	namespace
	{
		// 文字列が接頭辞で始まるか。
		bool startsWith(const std::string& text, const char* prefix)
		{
			const std::string head(prefix);
			return text.size() >= head.size() && text.compare(0, head.size(), head) == 0;
		}

		// 平面ベクトルの内積。
		double dot2(const Vec2& a, const Vec2& b)
		{
			return (a.x * b.x) + (a.y * b.y);
		}

		// 平面の点 (s, n) を軸・法線から復元する（axis ⊥ normal・どちらも単位ベクトル）。
		Vec2 planPoint(const Vec2& axis, const Vec2& normal, double s, double n)
		{
			return (axis * s) + (normal * n);
		}

		// 壁面座標（s＝軸方向, z＝高さ）の点。
		struct FacePoint
		{
			double s = 0.0;
			double z = 0.0;
		};

		// 壁面内の見付け幅（筋かいの軸に直交する広がり）を返す。外形が面にならない・
		// 縮退しているときは 0。
		//
		// 押し出しが壁面の法線方向なので、(s, z) へ落とした外形は IFC のプロファイルを
		// 回転しただけの形になる。筋かいは「長い帯」なので、**外形を回転キャリパで測った
		// 最小の幅**がそのまま見付け幅（45×90 なら 90）になる——凸多角形の最小幅は
		// どれかの辺に直交する向きで実現されるので、辺ごとに直交方向の広がりを測って
		// 最小を採ればよい。
		//
		// **「最も離れた 2 点」を軸と見なしてはいけない。** 実データの筋かいは端が
		// 尖った六角形なので偶然それでも合うが、単純な矩形断面では最長が対角線になり、
		// 幅が 2 倍近くに化ける（軸が対角線へ傾くため）。
		double faceWidth(const std::vector<FacePoint>& face)
		{
			if (face.size() < 3)
				return 0.0;

			double best = std::numeric_limits<double>::max();
			const std::size_t count = face.size();
			for (std::size_t i = 0; i < count; ++i)
			{
				const FacePoint& from = face[i];
				const FacePoint& to = face[(i + 1) % count];
				const Vec2 edge{to.s - from.s, to.z - from.z};
				const double len = std::hypot(edge.x, edge.y);
				if (len < core::kGeomEps)
					continue; // 重複頂点の辺は向きが定まらない

				const Vec2 dir = edge * (1.0 / len);
				double low = std::numeric_limits<double>::max();
				double high = std::numeric_limits<double>::lowest();
				for (const FacePoint& point : face)
				{
					// dir を +90 度回した向きへの射影。
					const double perp = (-point.s * dir.y) + (point.z * dir.x);
					low = std::min(low, perp);
					high = std::max(high, perp);
				}
				best = std::min(best, high - low);
			}
			return best == std::numeric_limits<double>::max() ? 0.0 : best;
		}

		// 解析中の耐力壁 1 枚（同じ耐力壁になる要素をまとめたもの）。
		struct Group
		{
			core::ShearWallKind kind = core::ShearWallKind::Brace;
			std::string name;		  // 筋かいのまとめ鍵（同名＝たすき掛け）
			bool doubleBrace = false; // Name が "筋かいダブル" 始まりだったか
			std::vector<ShearWallPiece> pieces;
		};

		// グループ全体の広がり（軸方向・高さ・材厚・見付け幅）。
		struct GroupExtent
		{
			double sMin = 0.0;
			double sMax = 0.0;
			double zBottom = 0.0;
			double zTop = 0.0;
			double thickness = 0.0;
			double width = 0.0;
		};

		GroupExtent groupExtent(const Group& group)
		{
			GroupExtent extent;
			extent.sMin = std::numeric_limits<double>::max();
			extent.sMax = std::numeric_limits<double>::lowest();
			extent.zBottom = std::numeric_limits<double>::max();
			extent.zTop = std::numeric_limits<double>::lowest();
			for (const ShearWallPiece& piece : group.pieces)
			{
				extent.sMin = std::min(extent.sMin, piece.sMin);
				extent.sMax = std::max(extent.sMax, piece.sMax);
				extent.zBottom = std::min(extent.zBottom, piece.zBottom);
				extent.zTop = std::max(extent.zTop, piece.zTop);
				extent.thickness = std::max(extent.thickness, piece.thickness);
				extent.width = std::max(extent.width, piece.width);
			}
			return extent;
		}

		// span 柱レイヤの base ストーリ（0 起点）が index の柱だけを集める。
		std::vector<const core::ColumnCommand*>
		columnsOfStory(const std::vector<core::ColumnCommand>& columns, std::size_t index)
		{
			std::vector<const core::ColumnCommand*> found;
			for (const core::ColumnCommand& column : columns)
			{
				double from = 0.0;
				double to = 0.0;
				if (!parseSpanLayer(column.layer, from, to))
					continue;
				if (std::llround(from) == static_cast<long long>(index) + 1)
					found.push_back(&column);
			}
			return found;
		}

		// 点に最も近い柱を返す（許容内に無ければ nullptr）。同距離なら**先に現れた柱**を
		// 採るので、列挙順に依存しない決定的な結果になる（columns の並びが決定的なため）。
		const core::ColumnCommand*
		nearestColumn(const std::vector<const core::ColumnCommand*>& columns, const Vec2& point)
		{
			const core::ColumnCommand* best = nullptr;
			double bestDistance = kShearWallColumnTol;
			for (const core::ColumnCommand* column : columns)
			{
				const double distance =
					std::hypot(column->position.x - point.x, column->position.y - point.y);
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = column;
				}
			}
			return best;
		}

		// 柱レイヤ名を ";" で連ねる（PIO の TargetLayers パラメータ）。
		std::string joinLayers(const std::vector<std::string>& layers)
		{
			std::string joined;
			for (const std::string& layer : layers)
			{
				if (!joined.empty())
					joined += ";";
				joined += layer;
			}
			return joined;
		}

		// 面材の面（表／裏／両面）と、軸からの距離を決める。line は軸の線が法線方向の
		// どこにあるか（＝柱芯を通る線の位置）。
		void resolvePanelSide(const Group& group, double line, ShearWallCommand& command)
		{
			bool front = false;
			bool back = false;
			double sum = 0.0;
			for (const ShearWallPiece& piece : group.pieces)
			{
				const double side = piece.offset - line;
				if (side > kShearWallMergeTol)
					front = true;
				else if (side < -kShearWallMergeTol)
					back = true;
				sum += std::abs(side);
			}
			command.panelOffset = sum / static_cast<double>(group.pieces.size());
			if (front && back)
				command.panelSide = core::ShearWallPanelSide::Both;
			else if (back)
				command.panelSide = core::ShearWallPanelSide::Back;
			else
				command.panelSide = core::ShearWallPanelSide::Front;
		}

		// 階 1 つぶんの耐力壁要素を、同じ耐力壁になるものごとにまとめて集める。
		// 並びは要素の出現順（＝storyElements の並び）で決定的。
		std::vector<Group> collectGroups(const Model& model, const std::vector<int>& elements)
		{
			std::vector<Group> groups;
			for (const int elementId : elements)
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr)
					continue;
				const bool brace = isShearBrace(*element);
				if (!brace && !isShearPanel(*element))
					continue;

				ShearWallPiece piece;
				if (!resolveShearWallPiece(model, *element, brace, piece))
					continue; // ソリッドを解決できない・水平押し出しでない要素はスキップ

				Group* found = nullptr;
				if (brace)
				{
					// たすき掛けは**同じ Name の 2 要素**として出るので、Name でまとめる。
					const std::string name = entityName(*element);
					for (Group& group : groups)
					{
						if (group.kind == core::ShearWallKind::Brace && group.name == name)
						{
							found = &group;
							break;
						}
					}
					if (found == nullptr)
					{
						Group group;
						group.kind = core::ShearWallKind::Brace;
						group.name = name;
						group.doubleBrace = isDoubleBrace(*element);
						groups.push_back(std::move(group));
						found = &groups.back();
					}
				}
				else
				{
					// 大壁の表裏は「同じ軸・同じ区間」の面材 2 枚として出るので、それを
					// まとめる（法線の正負で表裏に分かれる）。
					for (Group& group : groups)
					{
						if (group.kind != core::ShearWallKind::Panel || group.pieces.empty())
							continue;
						const ShearWallPiece& first = group.pieces.front();
						if (std::abs(dot2(first.axis, piece.axis) - 1.0) > core::kPointEps)
							continue;
						if (std::abs(first.sMin - piece.sMin) > kShearWallMergeTol ||
							std::abs(first.sMax - piece.sMax) > kShearWallMergeTol)
							continue;
						// **法線方向の近さも要る。** 同じ通りに並ぶ 2 枚の壁は、軸も
						// 軸方向の区間も一致しうる（間口の同じ部屋が並ぶだけで起こる）。
						// 表裏はせいぜい柱幅＋板厚しか離れないので、その範囲だけをまとめる。
						if (std::abs(first.offset - piece.offset) > kShearWallPairOffsetTol)
							continue;
						found = &group;
						break;
					}
					if (found == nullptr)
					{
						Group group;
						group.kind = core::ShearWallKind::Panel;
						groups.push_back(std::move(group));
						found = &groups.back();
					}
				}
				found->pieces.push_back(piece);
			}
			return groups;
		}
	} // namespace

	bool isShearBrace(const Entity& element)
	{
		return element.type == "IFCMEMBER" && startsWith(entityName(element), kBracePrefix);
	}

	bool isDoubleBrace(const Entity& element)
	{
		return element.type == "IFCMEMBER" && startsWith(entityName(element), kDoubleBracePrefix);
	}

	bool isShearPanel(const Entity& element)
	{
		return element.type == "IFCWALL" && startsWith(entityName(element), kPanelPrefix);
	}

	bool resolveShearWallPiece(const Model& model, const Entity& element, bool brace,
							   ShearWallPiece& out)
	{
		WorldSolid solid;
		if (!resolveElementWorldSolid(model, &element, solid))
			return false;

		// 押し出しは壁面に直交する＝水平でなければならない（鉛直押し出しの火打等は
		// 耐力壁として解釈できない）。
		const Vec3 extrude = solid.extrudeDir;
		const double planLength = std::hypot(extrude.x, extrude.y);
		if (std::abs(extrude.z) > kShearWallHorizontalTol || planLength < core::kGeomEps)
			return false;

		const std::vector<Vec3> base = solid.base();
		const std::vector<Vec3> top = solid.top();
		if (base.size() < 3 || top.size() != base.size())
			return false;

		ShearWallPiece piece;
		// 法線＝押し出し方向、軸＝それを −90 度回した向き。**normal は axis を +90 度
		// 回した向き**という関係を保つ（表／裏の左右がこの関係で決まる）。軸の向きは
		// (x, y) の辞書順で正へ揃え、反転したら法線も一緒に返す。
		piece.normal = Vec2{extrude.x / planLength, extrude.y / planLength};
		piece.axis = Vec2{piece.normal.y, -piece.normal.x};
		if (piece.axis.x < -core::kGeomEps ||
			(std::abs(piece.axis.x) <= core::kGeomEps && piece.axis.y < 0.0))
		{
			piece.axis = piece.axis * -1.0;
			piece.normal = piece.normal * -1.0;
		}

		double sMin = std::numeric_limits<double>::max();
		double sMax = std::numeric_limits<double>::lowest();
		double nMin = std::numeric_limits<double>::max();
		double nMax = std::numeric_limits<double>::lowest();
		double zMin = std::numeric_limits<double>::max();
		double zMax = std::numeric_limits<double>::lowest();
		std::vector<FacePoint> face;
		face.reserve(base.size());
		for (std::size_t loop = 0; loop < 2; ++loop)
		{
			const std::vector<Vec3>& points = (loop == 0) ? base : top;
			for (const Vec3& point : points)
			{
				const Vec2 plan{point.x, point.y};
				const double s = dot2(plan, piece.axis);
				const double n = dot2(plan, piece.normal);
				sMin = std::min(sMin, s);
				sMax = std::max(sMax, s);
				nMin = std::min(nMin, n);
				nMax = std::max(nMax, n);
				zMin = std::min(zMin, point.z);
				zMax = std::max(zMax, point.z);
				if (loop == 0)
					face.push_back(FacePoint{s, point.z});
			}
		}
		if (sMax - sMin < core::kPointEps || zMax - zMin < core::kPointEps ||
			nMax - nMin < core::kPointEps)
			return false;

		piece.sMin = sMin;
		piece.sMax = sMax;
		piece.offset = (nMin + nMax) / 2.0;
		piece.zBottom = zMin;
		piece.zTop = zMax;
		piece.thickness = nMax - nMin;

		if (brace)
		{
			piece.width = faceWidth(face);
			if (piece.width < core::kPointEps)
				return false;

			// 傾きの向き: 断面外形の**最も高い点**が軸方向のどちら寄りにあるか。
			double topS = face.front().s;
			double topZ = face.front().z;
			for (const FacePoint& point : face)
			{
				if (point.z > topZ)
				{
					topZ = point.z;
					topS = point.s;
				}
			}
			piece.risesToMax = (sMax - topS) < (topS - sMin);
		}

		out = piece;
		return true;
	}

	std::vector<ShearWallCommand>
	buildShearWallCommands(Context& context, const std::vector<core::ColumnCommand>& columns)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo> stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();
		const std::map<int, std::vector<std::string>> columnLayers =
			collectColumnLayersByStory(columns);

		std::vector<ShearWallCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerName(i, story.isTop, kLevelShearWall);
			// レイヤ平面（ストーリ相対）＝その階の横架材天端。最上階は軒高＝0。
			const double layerZ = story.isTop ? 0.0 : story.beamOffset;
			const auto layerList = columnLayers.find(static_cast<int>(i));
			const std::string targets =
				layerList == columnLayers.end() ? std::string() : joinLayers(layerList->second);
			const std::vector<const core::ColumnCommand*> storyColumns = columnsOfStory(columns, i);

			for (const Group& group : collectGroups(model, context.storyElements(story.id)))
			{
				const GroupExtent extent = groupExtent(group);
				const ShearWallPiece& first = group.pieces.front();

				// 要素自身の端（センタリング済み）で柱を探す。面材は壁芯から板厚ぶん
				// 外れているが、探す許容（kShearWallColumnTol）に対しては誤差の範囲。
				const core::ColumnCommand* startColumn = nearestColumn(
					storyColumns,
					planPoint(first.axis, first.normal, extent.sMin, first.offset) - center);
				const core::ColumnCommand* endColumn = nearestColumn(
					storyColumns,
					planPoint(first.axis, first.normal, extent.sMax, first.offset) - center);

				// 軸の線が法線方向のどこにあるか。柱が見つかればその柱芯を通り、
				// 見つからなければ要素自身（面材が複数枚あればその中点）を線とみなす。
				// **柱の無い端もこの線へ載せる**——面材自身の端をそのまま使うと、
				// 片端だけ板厚ぶん外れた斜めの軸になってしまう。
				double line = first.offset;
				if (startColumn != nullptr)
					line = dot2(startColumn->position + center, first.normal);
				else if (endColumn != nullptr)
					line = dot2(endColumn->position + center, first.normal);
				else if (group.pieces.size() > 1)
				{
					line = 0.0;
					for (const ShearWallPiece& piece : group.pieces)
						line += piece.offset;
					line /= static_cast<double>(group.pieces.size());
				}

				const Vec2 startPoint =
					startColumn != nullptr
						? startColumn->position
						: planPoint(first.axis, first.normal, extent.sMin, line) - center;
				const Vec2 endPoint =
					endColumn != nullptr
						? endColumn->position
						: planPoint(first.axis, first.normal, extent.sMax, line) - center;
				if (core::samePoint(startPoint, endPoint))
					continue; // 両端が同じ柱に寄った（＝軸が決まらない）

				// 内法は柱芯間から両側の半柱幅を引いたもの。柱が見つからなければ要素自身の
				// 広がりで代用する（PIO は図面の柱から引き直すので、これは控え）。
				double clear = std::hypot(endPoint.x - startPoint.x, endPoint.y - startPoint.y);
				if (startColumn != nullptr)
					clear -= startColumn->width / 2.0;
				if (endColumn != nullptr)
					clear -= endColumn->width / 2.0;
				if (clear <= 0.0)
					clear = extent.sMax - extent.sMin;
				if (clear <= 0.0)
					continue;

				ShearWallCommand command;
				command.layer = layer;
				command.targetLayers = targets;
				command.start = startPoint;
				command.end = endPoint;
				command.kind = group.kind;
				command.thickness = extent.thickness;
				command.clearSpan = clear;
				command.bottomHeight = extent.zBottom - layerZ;
				command.topHeight = extent.zTop - layerZ;

				if (group.kind == core::ShearWallKind::Brace)
				{
					command.drawClass = CLASS_BRACE;
					command.width = extent.width;
					command.braceStyle = (group.doubleBrace || group.pieces.size() >= 2)
											 ? core::ShearWallBraceStyle::Double
											 : core::ShearWallBraceStyle::Single;
					command.braceRisesToEnd = first.risesToMax;
				}
				else
				{
					command.drawClass = CLASS_SHEAR_PANEL;
					resolvePanelSide(group, line, command);
				}
				commands.push_back(std::move(command));
			}
		}
		return commands;
	}

	std::vector<ShearWallCommand> buildShearWallCommands(Context& context)
	{
		return buildShearWallCommands(context, context.columns());
	}

	std::vector<ShearWallCommand> buildShearWallCommands(const Model& model)
	{
		Context context(model);
		return buildShearWallCommands(context, context.columns());
	}

	bool anyShearWallOnLayer(const std::vector<ShearWallCommand>& walls, const std::string& layer)
	{
		return std::ranges::any_of(walls, [&layer](const ShearWallCommand& wall)
								   { return wall.layer == layer; });
	}
} // namespace HomeskzIfcImport::parse
