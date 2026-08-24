//
//	core/Document.cpp
//
//	validateDocument の実装。Python 版 document.py の validateDocument に対応する。
//	SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//
//	現状はバージョンの妥当性と、stories（M3）・floors（M5）・members（M7）・columns（M8）・
//	walls / slabs（M9）・wallJoins / 底盤の modifiers＝地中梁（M10）・rafters / roofs（M6）・
//	grids（M1）・シンボル置換系（M11: anchorBolts / floorPosts / fireBraces / joints）・
//	sheets（M13。シートレイヤ上のグラフィック凡例を含む）・sections（M14）・
//	ビューポート注釈の断面寸法データタグ（M13）の
//	各命令の必須フィールド・値域を見る。命令リストが追加されるたびに、対応する検証規則
//	（必須フィールドの有無・参照整合性・値域）をここへ足していく。
//
//	加えて、描画側から切り離せる純計算をここに置く（desiredStoryLayerOrder＝レイヤの希望
//	スタック順、raiseModifierTop＝地中梁の可視ソリッドの呑み込み）。SDK を触らないので無 SDK テストで検証できる
//	（CLAUDE.md「テスト方針」）。
//

#include "core/Document.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ranges>
#include <string>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// ストーリレベル 1 つが妥当か（Python 版 _validate_level 相当）。種別・レイヤ名が
		// 非空であること。offset は数値（C++ では double なので常に成立）。
		bool isValidLevel(const LevelCommand& level)
		{
			return !level.type.empty() && !level.layer.empty();
		}

		// ストーリ 1 つが妥当か（Python 版 _validate_story 相当）。名前・接尾辞が非空で
		// （空 suffix は VW 2026 で 2 回目以降の CreateStory が失敗するため不可）、各レベルが
		// 妥当であること。elevation は数値（double なので常に成立）。
		bool isValidStory(const StoryCommand& story)
		{
			return !story.name.empty() && !story.suffix.empty() &&
				   std::ranges::all_of(story.levels, isValidLevel);
		}

		// 構成層（スラブ・壁）1 枚が妥当か。名前とクラス名が非空で、層厚が 0 以上
		// （負の層は作れない）。クラス名は「層が何でできているか」を表し、層の描画属性を
		// そのクラス属性に従わせるための唯一の手掛かりなので、空を通さない
		// （core/Document.h「構成要素のクラス」）。
		bool isValidComponent(const ComponentCommand& component)
		{
			return !component.name.empty() && !component.drawClass.empty() &&
				   component.thickness >= 0.0;
		}

		// 床板 1 枚が妥当か（Python 版 _validate_floor 相当）。配置先レイヤ名・クラス名が
		// 非空で、平面外形が 3 点以上（面になる）で、高さ基準のレベル種別が非空で、構成層が
		// 1 枚以上あり総厚が正であること。elevation / bound.offset は数値（double なので
		// 常に成立）。
		bool isValidFloor(const FloorCommand& floor)
		{
			if (floor.layer.empty() || floor.drawClass.empty() || floor.boundary.size() < 3 ||
				floor.bound.level.empty() || floor.components.empty())
				return false;
			if (!std::ranges::all_of(floor.components, isValidComponent))
				return false;

			double total = 0.0;
			for (const ComponentCommand& component : floor.components)
				total += component.thickness;
			return total > 0.0;
		}

		// 垂木 1 本が妥当か（Python 版 _validate_rafter 相当）。配置先レイヤ名・クラス名が
		// 非空で、断面（幅・せい）が正で、平面の始点（軒側＝支持点）と終点（棟側）が縮退して
		// いないこと（縮退＝始点と終点が同じ点。判定は core/Geometry の samePoint）。
		// elevation / endElevation / overhang / embedment は数値（double なので常に成立）。
		// Python 版は型だけを見るが、C++ は型が静的なので「描けない値」を弾く幾何の関門に
		// 読み替える（床板と同じ方針）。
		bool isValidRafter(const RafterCommand& rafter)
		{
			return !rafter.layer.empty() && !rafter.drawClass.empty() && rafter.width > 0.0 &&
				   rafter.height > 0.0 && !samePoint(rafter.start, rafter.end);
		}

		// 横架材 1 本が妥当か（Python 版 _validate_member 相当）。配置先レイヤ名・クラス名・
		// 構造材 ID が非空で、断面（幅・せい）が正で、天端中央線の始端・終端が縮退していない
		// こと（判定は core/Geometry の samePoint）。始端・終端の高さ基準のレベル種別も非空
		// （空だと SetObjectStoryBound が解決できず、高さがレイヤ基準へリセットされる）。
		// elevation / endElevation は数値（double なので常に成立）。
		bool isValidMember(const MemberCommand& member)
		{
			return !member.layer.empty() && !member.drawClass.empty() && !member.memberId.empty() &&
				   member.width > 0.0 && member.height > 0.0 &&
				   !samePoint(member.start, member.end) && !member.startBound.level.empty() &&
				   !member.endBound.level.empty();
		}

		// 柱 1 本が妥当か（Python 版 _validate_column 相当）。配置先レイヤ名（span レイヤ）・
		// クラス名・構造材 ID・構造用途が非空で、断面（幅・せい）と柱高さが正で、上下端の
		// 高さ基準のレベル種別が非空であること（空だと SetObjectStoryBound が解決できず、
		// 高さがレイヤ基準へリセットされる）。elevation は数値（double なので常に成立）。
		bool isValidColumn(const ColumnCommand& column)
		{
			return !column.layer.empty() && !column.drawClass.empty() && !column.memberId.empty() &&
				   !column.structuralUse.empty() && column.width > 0.0 && column.depth > 0.0 &&
				   column.height > 0.0 && !column.bottomBound.level.empty() &&
				   !column.topBound.level.empty();
		}

		// 基礎の立上り 1 本が妥当か（Python 版 _validate_wall 相当）。配置先レイヤ名・
		// クラス名が非空で、壁厚が正で、壁芯の始点と終点が縮退していないこと
		// （判定は core/Geometry の samePoint）。上下端の高さ基準のレベル種別も非空（空だと
		// SetWallOverallHeights が解決できず、レイヤの「壁の高さ」設定に落ちる）。構成層は
		// 1 枚以上あり総厚が正であること（スラブと同じ関門。構成層の合計＝壁厚）。
		bool isValidWall(const WallCommand& wall)
		{
			if (wall.layer.empty() || wall.drawClass.empty() || wall.thickness <= 0.0 ||
				samePoint(wall.start, wall.end) || wall.bottomBound.level.empty() ||
				wall.topBound.level.empty() || wall.components.empty())
				return false;
			if (!std::ranges::all_of(wall.components, isValidComponent))
				return false;

			double total = 0.0;
			for (const ComponentCommand& component : wall.components)
				total += component.thickness;
			return total > 0.0;
		}

		// 地中梁（台形プリズム）1 本が妥当か（Python 版 _validate_modifier 相当）。断面が
		// 3 点以上（面になる）で、押し出し長が正であること（長さ 0 のプリズムは描けない）。
		// origin / azimuth は数値（double なので常に成立）。
		bool isValidModifier(const ModifierCommand& modifier)
		{
			return modifier.profile.size() >= 3 && modifier.depth > 0.0;
		}

		// 基礎の底盤 1 枚が妥当か（Python 版 _validate_slab 相当）。床板と同じ関門
		// （レイヤ名・クラス名が非空／外形 3 点以上／高さ基準のレベル種別が非空／構成層が
		// 1 枚以上あり総厚が正）に、コンクリート厚が正であることと、噛み合う地中梁がすべて
		// 妥当であることを足す（厚み 0 の構成層は VW が受け付けない）。
		bool isValidSlab(const SlabCommand& slab)
		{
			if (slab.layer.empty() || slab.drawClass.empty() || slab.boundary.size() < 3 ||
				slab.bound.level.empty() || slab.components.empty() || slab.thickness <= 0.0)
				return false;
			if (!std::ranges::all_of(slab.components, isValidComponent))
				return false;
			if (!std::ranges::all_of(slab.modifiers, isValidModifier))
				return false;

			double total = 0.0;
			for (const ComponentCommand& component : slab.components)
				total += component.thickness;
			return total > 0.0;
		}

		// 壁結合 1 件が妥当か（Python 版 _validate_wall_join 相当）。結合する 2 本が**異なる**
		// 立上りで、どちらも walls の範囲内を指すこと（範囲外の添字は描画側でハンドルを
		// 引けず、黙って結合されないだけになるので検証で弾く）。結合種別は enum なので
		// 値域は型が保証する。ピック点・交点は数値（double なので常に成立）。
		bool isValidWallJoin(const WallJoinCommand& join, std::size_t wallCount)
		{
			return join.a != join.b && join.a < wallCount && join.b < wallCount;
		}

		// 野地板 1 枚が妥当か（Python 版 _validate_roof 相当）。配置先レイヤ名・クラス名が
		// 非空で、平面外形が 3 点以上（面になる）で、厚みが正であること。勾配（rise/run）と
		// 高さは数値（double なので常に成立）で、退化した勾配は描画側がフォールバックで
		// 扱うためここでは弾かない（1 枚の異常で文書全体を描かないのは過剰）。
		bool isValidRoof(const RoofCommand& roof)
		{
			return !roof.layer.empty() && !roof.drawClass.empty() && roof.boundary.size() >= 3 &&
				   roof.thickness > 0.0;
		}

		// シート（伏図）1 枚が妥当か（Python 版 _validate_sheet / _validate_viewport 相当）。
		// ビューポート注釈の断面寸法データタグ 1 つが妥当か（Python 版 _validate_tag 相当）。
		// スタイル名が非空で、関連付け先の横架材が members の範囲内であること（範囲外の
		// 添字は「どの部材にも付かないタグ」＝図面に寸法の出ない空のタグが残る）。position /
		// angle は数値（double なので常に成立）で値域の制限は無い。
		bool isValidTag(const TagCommand& tag, std::size_t memberCount)
		{
			return !tag.style.empty() && tag.memberIndex < memberCount;
		}

		// ビューポート 1 枚のタグがすべて妥当か。伏図・軸組図が同じ規則で見る。
		bool areValidTags(const ViewportCommand& viewport, std::size_t memberCount)
		{
			return std::ranges::all_of(viewport.tags, [memberCount](const TagCommand& tag)
									   { return isValidTag(tag, memberCount); });
		}

		// シートレイヤ番号（＝レイヤ名）とタイトルが非空で、ビューポートが表示レイヤを
		// **1 つ以上**持ち、そのレイヤ名がどれも非空であること。図面タイトル・図番は空でも
		// 描ける（ラベルが空になるだけ）ので弾かない——Python 版が型だけを見るのと同じ扱い。
		// 表示レイヤが 0 枚の伏図は「何も映らないビューポート」なので作らせない。
		bool isValidSheet(const SheetCommand& sheet)
		{
			// グラフィック凡例（M13）を載せるなら、スタイル名が非空であること。凡例の中身は
			// スタイルが決めるので（core/Document.h の LegendCommand）、スタイル名が空の凡例は
			// 「何も並ばない空の箱」にしかならない。凡例を載せない伏図（＝空の optional）は妥当。
			if (sheet.legend.has_value() && sheet.legend->style.empty())
				return false;
			return !sheet.number.empty() && !sheet.title.empty() &&
				   !sheet.viewport.layers.empty() &&
				   std::ranges::none_of(sheet.viewport.layers,
										[](const std::string& layer) { return layer.empty(); });
		}

		// 断面ビューポート（軸組図）1 枚が妥当か（Python 版 _validate_section 相当）。
		// 配置先シートレイヤ番号（＝レイヤ名）とタイトルが非空で、表示レイヤを 1 つ以上持ち
		// （伏図と同じ理由＝何も映らないビューポートを作らせない）、**断面指示線が縮退して
		// いない**（始点≠終点。縮退した線からは切断面が決まらない）こと。断面の範囲は
		// 命令が持たない（core/Document.h の SectionCommand 参照）ので見ない。
		bool isValidSection(const SectionCommand& section)
		{
			return !section.number.empty() && !section.title.empty() &&
				   !section.viewport.layers.empty() &&
				   std::ranges::none_of(section.viewport.layers,
										[](const std::string& layer) { return layer.empty(); }) &&
				   !samePoint(section.lineStart, section.lineEnd);
		}

		// シンボル配置 1 件が妥当か（Python 版 _validate_anchor_bolt / _validate_floor_post /
		// _validate_fire_brace / _validate_joint と同じ関門を 1 つにまとめたもの）。配置先
		// レイヤ名とシンボル名が非空であること。position / angle は数値（double なので常に
		// 成立）で、値域の制限は無い（角度は 0〜360 に正規化しない。VW 側が受け取る）。
		bool isValidSymbol(const SymbolCommand& symbol)
		{
			return !symbol.layer.empty() && !symbol.symbol.empty();
		}

		// 記号（断面記号・伏図記号）1 つが妥当か。PIO を置くレイヤ名・作図クラス名・
		// **検索対象レイヤ名**が非空であること（対象レイヤが空だと PIO は何も見つけられず、
		// 記号 0 個の空オブジェクトが図面に残る）。伏図記号はシンボル名も非空であること
		// （シンボルが無ければ平面記号は描けない）。targetClass は**空が正常**＝全クラス。
		bool isValidColumnMark(const ColumnMarkCommand& mark)
		{
			return !mark.layer.empty() && !mark.drawClass.empty() && !mark.targetLayer.empty() &&
				   (mark.style != ColumnMarkStyle::Plan || !mark.symbol.empty());
		}
	} // namespace

	bool validateDocument(const Document& document)
	{
		if (document.version != kDocumentVersion)
			return false;

		// ストーリ: 名前・接尾辞が非空で、各ストーリレベルの種別・レイヤ名が非空であること
		// （Python 版 _validate_story / _validate_level と同じ関門。ROADMAP.md M3）。
		if (!std::ranges::all_of(document.stories, isValidStory))
			return false;

		// 床板: 配置先レイヤ名・クラス名・スタイル名が非空で、外形が 3 点以上、高さ基準の
		// レベル種別が非空、構成層が 1 枚以上あり総厚が正であること（isValidFloor 参照。
		// Python 版 _validate_floor と同じ関門。ROADMAP.md M5）。
		if (!std::ranges::all_of(document.floors, isValidFloor))
			return false;

		// 横架材: 配置先レイヤ名・クラス名・構造材 ID が非空で、断面が正・天端中央線が非縮退、
		// 両端の高さ基準のレベル種別が非空であること（isValidMember 参照。Python 版
		// _validate_member と同じ関門。ROADMAP.md M7）。
		if (!std::ranges::all_of(document.members, isValidMember))
			return false;

		// 柱: 配置先レイヤ名（span レイヤ）・クラス名・構造材 ID・構造用途が非空で、断面と
		// 柱高さが正、上下端の高さ基準のレベル種別が非空であること（isValidColumn 参照。
		// Python 版 _validate_column と同じ関門。ROADMAP.md M8）。
		if (!std::ranges::all_of(document.columns, isValidColumn))
			return false;

		// 基礎: 立上りは壁厚が正・壁芯が非縮退・上下端のレベル種別が非空、底盤は床板と同じ
		// 関門＋コンクリート厚が正であること（isValidWall / isValidSlab 参照。Python 版
		// _validate_wall / _validate_slab と同じ関門。ROADMAP.md M9）。
		if (!std::ranges::all_of(document.walls, isValidWall))
			return false;
		if (!std::ranges::all_of(document.slabs, isValidSlab))
			return false;

		// 壁結合（M10）: 結合する 2 本が異なり、どちらも walls の範囲内であること
		// （isValidWallJoin 参照。Python 版 _validate_wall_join と同じ関門。ROADMAP.md M10）。
		// 地中梁は底盤の modifiers として isValidSlab が併せて見る。
		if (!std::ranges::all_of(document.wallJoins, [&document](const WallJoinCommand& join)
								 { return isValidWallJoin(join, document.walls.size()); }))
			return false;

		// 垂木・野地板: 配置先レイヤ名・クラス名が非空で、垂木は断面が正・平面が非縮退、
		// 野地板は外形 3 点以上・厚みが正であること（Python 版 _validate_rafter /
		// _validate_roof と同じ関門。ROADMAP.md M6）。
		if (!std::ranges::all_of(document.rafters, isValidRafter))
			return false;
		if (!std::ranges::all_of(document.roofs, isValidRoof))
			return false;

		// シンボル置換系（アンカーボルト・床束・火打・仕口）: 配置先レイヤ名とシンボル名が
		// 非空であること（isValidSymbol 参照。Python 版 _validate_anchor_bolt ほかと同じ関門。
		// ROADMAP.md M11）。4 種は同じ命令型なので同じ規則で見る。
		if (!std::ranges::all_of(document.anchorBolts, isValidSymbol) ||
			!std::ranges::all_of(document.floorPosts, isValidSymbol) ||
			!std::ranges::all_of(document.fireBraces, isValidSymbol) ||
			!std::ranges::all_of(document.joints, isValidSymbol))
			return false;

		// 断面記号・伏図記号（M12）: PIO のレイヤ名・作図クラス名・検索対象レイヤ名が非空で、
		// 伏図記号はシンボル名も非空であること（isValidColumnMark 参照。ROADMAP.md M12）。
		if (!std::ranges::all_of(document.columnMarks, isValidColumnMark))
			return false;

		// シート（伏図）: シートレイヤ番号・タイトルが非空で、ビューポートが非空のレイヤ名を
		// 1 つ以上持ち、グラフィック凡例を載せるならそのスタイル名も非空であること
		// （isValidSheet 参照。Python 版 _validate_sheet / _validate_viewport / _validate_legend と
		// 同じ関門。ROADMAP.md M13）。
		if (!std::ranges::all_of(document.sheets, isValidSheet))
			return false;

		// 断面ビューポート（軸組図）: シートレイヤ番号・タイトル・表示レイヤに加え、指示線が
		// 縮退していないこと（isValidSection 参照。ROADMAP.md M14）。
		if (!std::ranges::all_of(document.sections, isValidSection))
			return false;

		// 断面寸法データタグ（M13）: 伏図・軸組図どちらのビューポート注釈も、スタイル名が
		// 非空で関連付け先の横架材が members の範囲内であること（areValidTags 参照）。
		// タグはビューポート命令の中に住むので、シート・軸組図の関門を通った後に見る。
		const std::size_t memberCount = document.members.size();
		if (!std::ranges::all_of(document.sheets, [memberCount](const SheetCommand& sheet)
								 { return areValidTags(sheet.viewport, memberCount); }))
			return false;
		if (!std::ranges::all_of(document.sections, [memberCount](const SectionCommand& section)
								 { return areValidTags(section.viewport, memberCount); }))
			return false;

		// 通り芯: 配置先レイヤ名が空でなく、始点と終点が異なる（縮退していない）こと。
		// 同一判定は parse/Grid の重複線除去と同じ core/Geometry の samePoint を通す
		// （閾値がズレると「畳まれた線が検証では非縮退」のような食い違いが起こる）。
		// クラス名は空でもよい（無クラス＝既定クラスへ）。1 本でも不正なら描画しない
		// （Python 版 validateDocument と同じ関門。ROADMAP.md M1）。
		//
		// TODO: 命令リストが増えたら、要素ごとの all_of を && で連ねてここに積む
		// （anchorBolt … の検証。ROADMAP.md）。
		return std::ranges::all_of(
			document.grids, [](const GridCommand& grid)
			{ return !grid.layer.empty() && !samePoint(grid.start, grid.end); });
	}

	bool sectionHeightRange(const Document& document, double& start, double& end)
	{
		double low = std::numeric_limits<double>::max();
		double high = std::numeric_limits<double>::lowest();
		bool any = false;
		const auto take = [&](double z)
		{
			low = std::min(low, z);
			high = std::max(high, z);
			any = true;
		};

		// 床（基準面と、構成層の合計だけ下がった下端）。
		for (const FloorCommand& floor : document.floors)
		{
			double thickness = 0.0;
			for (const ComponentCommand& component : floor.components)
				thickness += component.thickness;
			take(floor.elevation);
			take(floor.elevation - thickness);
		}
		// 横架材（天端と、せいのぶん下がった下端。傾斜梁は両端とも見る）。
		for (const MemberCommand& member : document.members)
		{
			take(member.elevation);
			take(member.endElevation);
			take(std::min(member.elevation, member.endElevation) - member.height);
		}
		// 柱（下端と上端）。
		for (const ColumnCommand& column : document.columns)
		{
			take(column.elevation);
			take(column.elevation + column.height);
		}
		// 屋根組（垂木の両端・野地板の軒）。
		for (const RafterCommand& rafter : document.rafters)
		{
			take(rafter.elevation);
			take(rafter.endElevation);
		}
		for (const RoofCommand& roof : document.roofs)
			take(roof.elevation);
		// 基礎の底盤（天端と、コンクリート厚のぶん下がった下端）。立上りは高さを絶対値で
		// 持たない（レベルへのバインドで表す）ので、底盤とストーリで下端を代表させる。
		for (const SlabCommand& slab : document.slabs)
		{
			take(slab.elevation);
			take(slab.elevation - slab.thickness);
			// 地中梁（底盤にぶら下がる台形プリズム）。**モデルの最深部はふつう底盤の下端では
			// なく地中梁の下端**なので、これを見ないと余白（kSectionHeightMargin）より深い
			// 地中梁が軸組図の足元で切れる。断面原点が梁下端（v=0）で origin.z が絶対 Z
			// なので、プロファイルの v をそのまま足せば上下端になる（ModifierCommand 参照）。
			for (const ModifierCommand& modifier : slab.modifiers)
			{
				for (const Vec2& vertex : modifier.profile)
					take(modifier.origin.z + vertex.y);
			}
		}
		// ストーリ高さ（要素が 1 つも無い階でも範囲に含める）。
		for (const StoryCommand& story : document.stories)
			take(story.elevation);

		if (!any)
			return false;
		start = low - kSectionHeightMargin;
		end = high + kSectionHeightMargin;
		return true;
	}

	ModifierCommand raiseModifierTop(const ModifierCommand& modifier, double bite)
	{
		if (bite <= 0.0 || modifier.profile.empty())
			return modifier;

		// 天端＝最大 v。そこから kModifierTopVertexTol 以内の頂点を天端の辺とみなす。
		double vMax = modifier.profile.front().y;
		for (const Vec2& p : modifier.profile)
			vMax = std::max(vMax, p.y);
		const auto isTop = [&](std::size_t i)
		{ return modifier.profile[i].y >= vMax - kModifierTopVertexTol; };

		const std::size_t n = modifier.profile.size();
		ModifierCommand raised = modifier;
		for (std::size_t i = 0; i < n; ++i)
		{
			if (!isTop(i))
				continue;
			const Vec2& top = modifier.profile[i];
			// 隣接する 2 頂点のうち**下端側**（側辺の相手）を探し、その斜辺の延長線上へ
			// 動かす。見つからない（天端が水平に分割された中間頂点）／側辺がほぼ水平なら
			// 真上へ上げる。
			double du = 0.0;
			for (const std::size_t j : {(i + n - 1) % n, (i + 1) % n})
			{
				if (isTop(j))
					continue;
				const double dv = top.y - modifier.profile[j].y;
				if (std::abs(dv) > kModifierTopVertexTol)
					du = ((top.x - modifier.profile[j].x) / dv) * bite;
				break;
			}
			raised.profile[i] = Vec2{top.x + du, top.y + bite};
		}
		return raised;
	}

	namespace
	{
		// スタック最下段（背面）へ回すレベル種別か（Python 版 _BACKGROUND_LEVEL_TYPES）。
		// 床（FL）・野地板のレイヤは伏図ビューポートで柱・梁を覆い隠さないよう全ストーリ
		// 分をまとめて背面へ集める（野地板レベルは M6 で追加済み。この並びの適用先は
		// M13 の per-viewport 上書き。desiredStoryLayerOrder の doc コメント参照）。
		bool isBackgroundLevel(const std::string& type)
		{
			return type == kLevelFL || type == kLevelNojiita;
		}
	} // namespace

	std::vector<std::string> desiredStoryLayerOrder(const std::vector<StoryCommand>& stories,
													const std::vector<std::string>& topLayers)
	{
		std::vector<std::string> order;
		// 通り芯レイヤ "共通"（core::kGridLayer。GridCommand::layer の既定値と同じ）を
		// スタック最上段に置き、続けて topLayers を積む。
		order.emplace_back(kGridLayer);
		order.insert(order.end(), topLayers.begin(), topLayers.end());

		// stories は Elevation 昇順（最下階→最上階）。スタックは最上階→最下階なので逆順に辿る。
		std::vector<std::string> background;
		for (const StoryCommand& command : std::views::reverse(stories))
		{
			for (const LevelCommand& level : command.levels)
			{
				if (isBackgroundLevel(level.type))
					background.push_back(level.layer);
				else
					order.push_back(level.layer);
			}
		}
		order.insert(order.end(), background.begin(), background.end());
		return order;
	}
} // namespace HomeskzIfcImport::core
