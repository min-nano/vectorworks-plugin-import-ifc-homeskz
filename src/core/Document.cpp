//
//	core/Document.cpp
//
//	validateDocument の実装。SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//
//	現状はバージョンの妥当性と、stories（M3）・floors（M5）・members（M7）・columns（M8）・
//	walls / slabs（M9）・wallJoins / 底盤の modifiers＝地中梁（M10）・rafters / roofs（M6）・
//	grids（M1）・シンボル置換系（M11: anchorBolts / floorPosts / fireBraces / joints、M33: splices）・
//	sheets（M13。シートレイヤ上のグラフィック凡例を含む）・sections（M14）・
//	ビューポート注釈の断面寸法データタグ（M13）の
//	各命令の必須フィールド・値域を見る。命令リストが追加されるたびに、対応する検証規則
//	（必須フィールドの有無・参照整合性・値域）をここへ足していく。
//
//	加えて、描画側から切り離せる純計算をここに置く（desiredStoryLayerOrder＝レイヤの希望
//	スタック順、raiseModifierTop＝地中梁の可視ソリッドの呑み込み、modifierBasePolygon＝
//	地中梁の押し出しの基面、rafterEaveEnd＝垂木の軒先側の材端）。SDK を触らないので無 SDK テストで検証できる（CLAUDE.md「テスト方針」）。
//

#include "core/Document.h"
#include "core/Layout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <numbers>
#include <ranges>
#include <string>
#include <utility>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// ストーリレベル 1 つが妥当か。種別・レイヤ名が非空であること。offset は数値（C++
		// では double なので常に成立）。
		bool isValidLevel(const LevelCommand& level)
		{
			return !level.type.empty() && !level.layer.empty();
		}

		// ストーリ 1 つが妥当か。名前・接尾辞が非空で（空 suffix は VW 2026 で 2 回目以降の
		// CreateStory が失敗するため不可）、各レベルが妥当であること。elevation
		// は数値（double なので常に成立）。
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

		// 構成層の並びが妥当か。1 枚以上あり、各層が妥当で、総厚（＝スラブ厚・壁厚）が正で
		// あること（厚み 0 の複合オブジェクトは VW が受け付けない）。床板・立上り・底盤が
		// 同じ関門を通る——かつて 3 者が同じ 3 条件を各々書いていた。
		bool hasValidComponents(const std::vector<ComponentCommand>& components)
		{
			return !components.empty() && std::ranges::all_of(components, isValidComponent) &&
				   totalThickness(components) > 0.0;
		}

		// ビューポートが表示レイヤを 1 つ以上持ち、そのレイヤ名がどれも非空であること。
		// 表示レイヤ 0 枚は「何も映らないビューポート」なので作らせない。非表示にするクラス名・
		// グレーで重ねるレイヤ名も非空であること（0 個は可）。グレーで重ねるレイヤは表示
		// レイヤと重ねない（同じレイヤを表示とグレーの両方に挙げると、描画側が後から当てた
		// 方で決まり、どちらのつもりかが命令から読めない）。伏図（isValidSheet）と軸組図
		// （isValidSection）が同じ規則で見る。
		bool hasDrawableLayers(const ViewportCommand& viewport)
		{
			const auto isEmpty = [](const std::string& name) { return name.empty(); };
			return !viewport.layers.empty() && std::ranges::none_of(viewport.layers, isEmpty) &&
				   std::ranges::none_of(viewport.hiddenClasses, isEmpty) &&
				   std::ranges::none_of(viewport.grayedLayers, isEmpty) &&
				   std::ranges::none_of(viewport.grayedLayers,
										[&viewport](const std::string& name) {
											return std::ranges::find(viewport.layers, name) !=
												   viewport.layers.end();
										});
		}

		// 床板 1 枚が妥当か。配置先レイヤ名・クラス名が非空で、平面外形が 3 点以上（面になる）
		// で、高さ基準のレベル種別が非空で、構成層が妥当（1 枚以上・総厚が正）であること。
		// elevation / bound.offset は数値（double なので常に成立）。
		bool isValidFloor(const FloorCommand& floor)
		{
			return !floor.layer.empty() && !floor.drawClass.empty() && floor.boundary.size() >= 3 &&
				   !floor.bound.level.empty() && hasValidComponents(floor.components);
		}

		// 垂木 1 本が妥当か。配置先レイヤ名・クラス名が非空で、断面（幅・せい）が正で、
		// 平面の始点（軒側＝支持点）と終点（棟側）が縮退していないこと（縮退＝始点と終点が同
		// じ点。判定は core/Geometry の samePoint）、そして**両端の高さ基準のレベル種別が
		// 非空**であること（構造材ツールは両端をストーリレベルへバインドして高さを決めるので、
		// レベル名が空だと高さが崩れる。横架材・柱と同じ関門）。elevation / endElevation /
		// overhang / embedment は数値（double なので常に成立）。型で保証できるもの（数値で
		// あること等）は見ず、「描けない値」を弾く幾何の関門に絞る（床板と同じ方針）。
		bool isValidRafter(const RafterCommand& rafter)
		{
			return !rafter.layer.empty() && !rafter.drawClass.empty() && rafter.width > 0.0 &&
				   rafter.height > 0.0 && !samePoint(rafter.start, rafter.end) &&
				   !rafter.startBound.level.empty() && !rafter.endBound.level.empty();
		}

		// 横架材が**実際に描かれる長さ**（mm）。パス（天端中央線）の平面長に両端の端部
		// オフセットを足したもの（オフセットは負で短く・正で長くする。core/Document.h
		// 「端部オフセット」）。
		double drawnLength(const MemberCommand& member)
		{
			return distance(member.start, member.end) + member.startOffset + member.endOffset;
		}

		// 横架材 1 本が妥当か。配置先レイヤ名・クラス名・構造材 ID が非空で、断面（幅・せい）
		// が正で、天端中央線の始端・終端が縮退していないこと（判定は core/Geometry の
		// samePoint）。始端・終端の高さ基準のレベル種別も非空（空だと SetObjectStoryBound
		// が解決できず、高さがレイヤ基準へリセットされる）。elevation / endElevation
		// は数値（double なので常に成立）。
		// **端部オフセットは材を消してはならない**: 端部オフセットは負値で材を短くするので
		// （core/Document.h「端部オフセット」）、パス長に両端のオフセットを足した「実際に
		// 描かれる長さ」が正であることを確かめる。ここが 0 以下だと、命令はあるのに材が
		// 1mm も描かれない（＝図面に出ない）。正値（材を伸ばす向き）は長さを増やすだけなので
		// この関門には掛からない。
		bool isValidMember(const MemberCommand& member)
		{
			return !member.layer.empty() && !member.drawClass.empty() && !member.memberId.empty() &&
				   member.width > 0.0 && member.height > 0.0 &&
				   !samePoint(member.start, member.end) && !member.startBound.level.empty() &&
				   !member.endBound.level.empty() && drawnLength(member) > 0.0;
		}

		// 柱 1 本が妥当か。配置先レイヤ名（span レイヤ）・クラス名・構造材 ID・構造用途が非空
		// で、断面（幅・せい）とパス長（height）が正で、上下端の高さ基準のレベル種別が非空で
		// あること（空だと SetObjectStoryBound が解決できず、高さがレイヤ基準へリセットされ
		// る）。elevation は数値（double なので常に成立）。端部オフセットを足した「実際に
		// 描かれる高さ」も正であること（isValidMember と同じ理由）。
		bool isValidColumn(const ColumnCommand& column)
		{
			return !column.layer.empty() && !column.drawClass.empty() && !column.memberId.empty() &&
				   !column.structuralUse.empty() && column.width > 0.0 && column.depth > 0.0 &&
				   column.height > 0.0 && !column.bottomBound.level.empty() &&
				   !column.topBound.level.empty() &&
				   column.height + column.startOffset + column.endOffset > 0.0;
		}

		// 基礎の立上り 1 本が妥当か。配置先レイヤ名・クラス名が非空で、壁厚が正で、
		// 壁芯の始点と終点が縮退していないこと（判定は core/Geometry の samePoint）。
		// 上下端の高さ基準のレベル種別も非空（空だと SetWallOverallHeights が解決できず、
		// レイヤの「壁の高さ」設定に落ちる）。構成層も妥当であること（スラブと同じ関門。
		// 構成層の合計＝壁厚）。
		bool isValidWall(const WallCommand& wall)
		{
			return !wall.layer.empty() && !wall.drawClass.empty() && wall.thickness > 0.0 &&
				   !samePoint(wall.start, wall.end) && !wall.bottomBound.level.empty() &&
				   !wall.topBound.level.empty() && hasValidComponents(wall.components);
		}

		// 床付け（捨てコン・砕石）1 区間が妥当か。断面が 3 点以上（面になる）で、素材クラス名が
		// 非空で、押し出し長が正であること（長さ 0 のプリズムは描けない。向きと断面の座標系は
		// 地中梁と共有するのでここでは見ない）。
		bool isValidBedding(const BeddingCommand& bedding)
		{
			return bedding.profile.size() >= 3 && !bedding.drawClass.empty() && bedding.depth > 0.0;
		}

		// 地中梁（台形プリズム）1 本が妥当か。断面が 3 点以上（面になる）で、押し出し長が正で
		// あること（長さ 0 のプリズムは描けない）。origin / azimuth は数値（double
		// なので常に成立）。ぶら下がる床付けもすべて妥当であること。
		bool isValidModifier(const ModifierCommand& modifier)
		{
			return modifier.profile.size() >= 3 && modifier.depth > 0.0 &&
				   std::ranges::all_of(modifier.beddings, isValidBedding);
		}

		// 基礎の底盤 1 枚が妥当か。床板と同じ関門（レイヤ名・クラス名が非空／外形
		// 3 点以上／高さ基準のレベル種別が非空／構成層が妥当）に、コンクリート厚が正で
		// あることと、噛み合う地中梁がすべて妥当であることを足す。
		bool isValidSlab(const SlabCommand& slab)
		{
			return !slab.layer.empty() && !slab.drawClass.empty() && slab.boundary.size() >= 3 &&
				   !slab.bound.level.empty() && slab.thickness > 0.0 &&
				   hasValidComponents(slab.components) &&
				   std::ranges::all_of(slab.modifiers, isValidModifier);
		}

		// 壁結合 1 件が妥当か。結合する 2 本が**異なる**立上りで、どちらも walls
		// の範囲内を指すこと（範囲外の添字は描画側でハンドルを引けず、黙って結合されないだけ
		// になるので検証で弾く）。結合種別は enum なので値域は型が保証する。ピック点・
		// 交点は数値（double なので常に成立）。
		bool isValidWallJoin(const WallJoinCommand& join, std::size_t wallCount)
		{
			return join.a != join.b && join.a < wallCount && join.b < wallCount;
		}

		// 野地板 1 枚が妥当か。配置先レイヤ名・クラス名が非空で、平面外形が 3 点以上（面にな
		// る）で、厚みが正であること。勾配（rise/run）と高さは数値（double なので常に成立）で、
		// 退化した勾配は描画側がフォールバックで扱うためここでは弾かない（1 枚の異常で文書全
		// 体を描かないのは過剰）。
		bool isValidRoof(const RoofCommand& roof)
		{
			return !roof.layer.empty() && !roof.drawClass.empty() && roof.boundary.size() >= 3 &&
				   roof.thickness > 0.0;
		}

		// シート（伏図）1 枚が妥当か。ビューポート注釈の断面寸法データタグ 1 つが妥当か。
		// 関連付け先の横架材が members の範囲内であること（範囲外の添字は「どの部材にも
		// 付かないタグ」＝図面に寸法の出ない空のタグが残る）。position / angle は数値
		// （double なので常に成立）で値域の制限は無い。**スタイル名は見ない**——タグは
		// スタイルを持たないため（core/Document.h の TagCommand）。
		bool isValidTag(const TagCommand& tag, std::size_t memberCount)
		{
			return tag.memberIndex < memberCount;
		}

		// ビューポート 1 枚のタグがすべて妥当か。伏図・軸組図が同じ規則で見る。
		bool areValidTags(const ViewportCommand& viewport, std::size_t memberCount)
		{
			return std::ranges::all_of(viewport.tags, [memberCount](const TagCommand& tag)
									   { return isValidTag(tag, memberCount); });
		}

		// 寸法の列 1 つが妥当か（M31）。測点が 2 つ以上・有限で**狭義の昇順**（同じ点が
		// 並ぶと長さ 0 の寸法ができ、逆順だと向きの反転した寸法ができる）、base が有限、
		// side が ±1、tier が 0 以上であること。
		bool isValidDimensionChain(const DimensionChainCommand& chain)
		{
			if (chain.stops.size() < 2 || !std::isfinite(chain.base))
				return false;
			if (chain.side != 1 && chain.side != -1)
				return false;
			if (chain.tier < 0)
				return false;
			if (!std::ranges::all_of(chain.stops, [](double v) { return std::isfinite(v); }))
				return false;
			return std::ranges::adjacent_find(chain.stops, std::greater_equal<>()) ==
				   chain.stops.end();
		}

		bool areValidDimensions(const ViewportCommand& viewport)
		{
			return std::ranges::all_of(viewport.dimensions, isValidDimensionChain);
		}

		// レベル記号 1 つが妥当か（M31）。表示名・結ぶストーリ・レベル種別が非空で、高さ・
		// 図の左右の端が有限で右端が左端より左に無く、段が -1 以上であること。
		bool isValidLevelMark(const LevelMarkCommand& level)
		{
			return !level.name.empty() && !level.story.empty() && !level.levelType.empty() &&
				   std::isfinite(level.elevation) && std::isfinite(level.x) &&
				   std::isfinite(level.right) && level.right >= level.x &&
				   level.dimensionTier >= -1;
		}

		// シートレイヤ番号（＝レイヤ名）とタイトルが非空で、ビューポートが表示レイヤを持つ
		// こと（hasDrawableLayers）。図面タイトル・図番は空でも描ける（ラベルが空になる
		// だけ）ので弾かない。
		bool isValidSheet(const SheetCommand& sheet)
		{
			// グラフィック凡例（M13）は**載せるか載せないか**しか持たない（配置点は用紙座標
			// なので値域の縛りが無く、スタイル名も持たない＝スタイル無しで置く。
			// core/Document.h の LegendCommand）。したがって凡例そのものに検証する項目は無い。
			return !sheet.number.empty() && !sheet.title.empty() &&
				   hasDrawableLayers(sheet.viewport);
		}

		// 断面ビューポート（軸組図）1 枚が妥当か。表示レイヤを持ち（hasDrawableLayers。
		// 伏図と同じ理由＝何も映らないビューポートを作らせない）、**断面指示線が縮退して
		// いない**（始点≠終点。縮退した線からは切断面が決まらない）こと。断面の範囲も配置先の
		// シートレイヤも命令が持たない（core/Document.h の SectionCommand 参照）ので見ない
		// ——シートレイヤの通し方は文書に 1 つの SectionSheetCommand が持ち、下の
		// isValidSectionSheet が見る。
		bool isValidSection(const SectionCommand& section)
		{
			return hasDrawableLayers(section.viewport) &&
				   !samePoint(section.lineStart, section.lineEnd);
		}

		// 軸組図のシートレイヤの通し方が妥当か（軸組図が 1 枚でもあるときだけ見る）。
		// 番号の始まりが正（シートレイヤ名になるので 0 や負では伏図の続きにならない）で、
		// タイトルの基が非空であること。
		bool isValidSectionSheet(const SectionSheetCommand& sheet)
		{
			return sheet.startNumber > 0 && !sheet.title.empty();
		}

		// シンボル配置 1 件が妥当か。配置先レイヤ名とシンボル名が非空であること。position /
		// angle は数値（double なので常に成立）で、値域の制限は無い（角度は 0〜360 に正規化し
		// ない。VW 側が受け取る）。
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

		// 耐力壁 1 枚が妥当か。PIO を置くレイヤ名・作図クラス名が非空で、軸（柱芯どうし）が
		// 縮退しておらず（縮退した軸からは向きも長さも決まらない。判定は core/Geometry の
		// samePoint）、材厚と軸組内法（下端 < 上端。上端は始点側・終点側の両方）が正であること。
		//
		// **柱を探すレイヤ名（targetLayers）は空を許す**——柱の無い階（柱レイヤが 1 つも
		// 生成されなかった）でも耐力壁そのものは描けるべきで、そのとき PIO は控えの内法
		// （clearSpan）で描く。空を弾くと「柱が無いと耐力壁が丸ごと消える」という、
		// 図面としては黙って欠ける最悪の形になる。
		// 筋かいは見付け幅が正であること（幅 0 の帯は描けない）。面材は幅を使わない。
		bool isValidShearWall(const ShearWallCommand& wall)
		{
			if (wall.layer.empty() || wall.drawClass.empty() || samePoint(wall.start, wall.end) ||
				wall.thickness <= 0.0 || wall.topHeight <= wall.bottomHeight ||
				wall.topHeightEnd <= wall.bottomHeight || wall.clearSpan <= 0.0)
				return false;
			return wall.kind != ShearWallKind::Brace || wall.width > 0.0;
		}

		// 通り芯 1 本が妥当か。配置先レイヤ名が空でなく、始点と終点が異なる（縮退していない）
		// こと。同一判定は parse/Grid の重複線除去と同じ core/Geometry の samePoint を通す
		// （閾値がズレると「畳まれた線が検証では非縮退」のような食い違いが起こる）。クラス名は
		// 空でもよい（無クラス＝既定クラスへ）。
		bool isValidGrid(const GridCommand& grid)
		{
			return !grid.layer.empty() && !samePoint(grid.start, grid.end);
		}
	} // namespace

	bool validateDocument(const Document& document)
	{
		if (document.version != kDocumentVersion)
			return false;

		// ストーリ: 名前・接尾辞が非空で、各ストーリレベルの種別・レイヤ名が非空であること
		// （docs/DEV-NOTES.md M3）。
		if (!std::ranges::all_of(document.stories, isValidStory))
			return false;

		// 床板: 配置先レイヤ名・クラス名が非空で、外形が 3 点以上、高さ基準のレベル種別が
		// 非空、構成層が妥当（1 枚以上・総厚が正）であること（isValidFloor 参照。
		// docs/DEV-NOTES.md M5。スタイルは作らない・当てないのでスタイル名は持たない）。
		if (!std::ranges::all_of(document.floors, isValidFloor))
			return false;

		// 横架材: 配置先レイヤ名・クラス名・構造材 ID が非空で、断面が正・天端中央線が非縮退、
		// 両端の高さ基準のレベル種別が非空であること（isValidMember 参照。docs/DEV-NOTES.md
		// M7）。
		if (!std::ranges::all_of(document.members, isValidMember))
			return false;

		// 柱: 配置先レイヤ名（span レイヤ）・クラス名・構造材 ID・構造用途が非空で、
		// 断面と柱高さが正、上下端の高さ基準のレベル種別が非空であること（isValidColumn 参照。
		// docs/DEV-NOTES.md M8）。
		if (!std::ranges::all_of(document.columns, isValidColumn))
			return false;

		// 基礎: 立上りは壁厚が正・壁芯が非縮退・上下端のレベル種別が非空、底盤は床板と同じ関
		// 門＋コンクリート厚が正であること（isValidWall / isValidSlab 参照。docs/DEV-NOTES.md
		// M9）。
		if (!std::ranges::all_of(document.walls, isValidWall))
			return false;
		if (!std::ranges::all_of(document.slabs, isValidSlab))
			return false;

		// 壁結合（M10）: 結合する 2 本が異なり、どちらも walls の範囲内であること
		// （isValidWallJoin 参照。docs/DEV-NOTES.md M10）。地中梁は底盤の modifiers として
		// isValidSlab が併せて見る。
		if (!std::ranges::all_of(document.wallJoins, [&document](const WallJoinCommand& join)
								 { return isValidWallJoin(join, document.walls.size()); }))
			return false;

		// 垂木・野地板: 配置先レイヤ名・クラス名が非空で、垂木は断面が正・平面が非縮退、
		// 野地板は外形 3 点以上・厚みが正であること（docs/DEV-NOTES.md M6）。
		if (!std::ranges::all_of(document.rafters, isValidRafter))
			return false;
		if (!std::ranges::all_of(document.roofs, isValidRoof))
			return false;

		// シンボル置換系（アンカーボルト・床束・火打・仕口・継手）: 配置先レイヤ名とシンボル名
		// が非空であること（isValidSymbol 参照。docs/DEV-NOTES.md M11 / M33）。5 種は同じ命令型
		// なので同じ規則で見る。
		if (!std::ranges::all_of(document.anchorBolts, isValidSymbol) ||
			!std::ranges::all_of(document.floorPosts, isValidSymbol) ||
			!std::ranges::all_of(document.fireBraces, isValidSymbol) ||
			!std::ranges::all_of(document.joints, isValidSymbol) ||
			!std::ranges::all_of(document.splices, isValidSymbol))
			return false;

		// 断面記号・伏図記号（M12）: PIO のレイヤ名・作図クラス名・検索対象レイヤ名が非空で、
		// 伏図記号はシンボル名も非空であること（isValidColumnMark 参照。docs/DEV-NOTES.md M12）。
		if (!std::ranges::all_of(document.columnMarks, isValidColumnMark))
			return false;

		// 耐力壁（M19）: レイヤ名・クラス名が非空で、軸が非縮退・材厚と軸組内法が正で
		// あること（isValidShearWall 参照。docs/DEV-NOTES.md M19）。
		if (!std::ranges::all_of(document.shearWalls, isValidShearWall))
			return false;

		// シート（伏図）: シートレイヤ番号・タイトルが非空で、ビューポートが非空のレイヤ名を
		// 1 つ以上持つこと（isValidSheet 参照。docs/DEV-NOTES.md M13）。
		if (!std::ranges::all_of(document.sheets, isValidSheet))
			return false;

		// 断面ビューポート（軸組図）: シートレイヤ番号・タイトル・表示レイヤに加え、指示線が
		// 縮退していないこと（isValidSection 参照。docs/DEV-NOTES.md M14）。
		if (!std::ranges::all_of(document.sections, isValidSection))
			return false;
		// 軸組図があるなら、その配置先シートレイヤの通し方（番号の始まり・タイトルの基）も
		// 埋まっていること（M18）。**軸組図が 1 枚も無ければ見ない**——使わない値なので、
		// 空のままでも文書は妥当。
		if (!document.sections.empty() && !isValidSectionSheet(document.sectionSheet))
			return false;

		// 断面寸法データタグ（M13）: 伏図・軸組図どちらのビューポート注釈も、関連付け先の
		// 横架材が members の範囲内であること（areValidTags 参照）。
		// タグはビューポート命令の中に住むので、シート・軸組図の関門を通った後に見る。
		const std::size_t memberCount = document.members.size();
		if (!std::ranges::all_of(document.sheets, [memberCount](const SheetCommand& sheet)
								 { return areValidTags(sheet.viewport, memberCount); }))
			return false;
		if (!std::ranges::all_of(document.sections, [memberCount](const SectionCommand& section)
								 { return areValidTags(section.viewport, memberCount); }))
			return false;

		// 寸法（M31）: 伏図・軸組図どちらの列も測点が狭義の昇順で 2 つ以上あること
		// （isValidDimensionChain 参照）。軸組図のレベル記号は表示名が非空であること。
		// **寸法規格の名前が空なのに寸法がある**文書は、描画側が何のスタイルで描くか
		// 決められないので弾く（解析側は空なら 1 つも作らない＝core/Document.h）。
		const bool anyDimension =
			std::ranges::any_of(document.sheets, [](const SheetCommand& sheet)
								{ return !sheet.viewport.dimensions.empty(); }) ||
			std::ranges::any_of(
				document.sections, [](const SectionCommand& section)
				{ return !section.viewport.dimensions.empty() || !section.levels.empty(); });
		if (anyDimension && document.dimensionStandard.empty())
			return false;
		if (!std::ranges::all_of(document.sheets, [](const SheetCommand& sheet)
								 { return areValidDimensions(sheet.viewport); }))
			return false;
		if (!std::ranges::all_of(document.sections,
								 [](const SectionCommand& section)
								 {
									 return areValidDimensions(section.viewport) &&
											std::ranges::all_of(section.levels, isValidLevelMark);
								 }))
			return false;

		// 通り芯: 配置先レイヤ名が空でなく、始点と終点が異なる（縮退していない）こと
		// （isValidGrid 参照）。1 本でも不正なら描画しない（docs/DEV-NOTES.md M1）。
		return std::ranges::all_of(document.grids, isValidGrid);
	}

	namespace
	{
		int outermostTierOf(const ViewportCommand& viewport)
		{
			int tier = -1;
			for (const DimensionChainCommand& chain : viewport.dimensions)
				tier = std::max(tier, chain.tier);
			return tier;
		}
	} // namespace

	int outermostDimensionTier(const std::vector<SheetCommand>& sheets)
	{
		int tier = -1;
		for (const SheetCommand& sheet : sheets)
			tier = std::max(tier, outermostTierOf(sheet.viewport));
		return tier;
	}

	int outermostDimensionTier(const std::vector<SectionCommand>& sections)
	{
		int tier = -1;
		for (const SectionCommand& section : sections)
			tier = std::max(tier, outermostTierOf(section.viewport));
		return tier;
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
			take(floor.elevation);
			take(floor.elevation - totalThickness(floor.components));
		}
		// 横架材（天端と、せいのぶん下がった下端。傾斜梁は両端とも見る）。
		for (const MemberCommand& member : document.members)
		{
			take(member.elevation);
			take(member.endElevation);
			take(memberBottomZ(member));
		}
		// 柱（下端と上端）。命令の上端は受ける横架材の天端＝材が実際に止まる高さより上なので、
		// **端部オフセットを戻した実際の上端**を見る（core/Document.h「端部オフセット」）。
		for (const ColumnCommand& column : document.columns)
		{
			take(columnDrawnBottom(column));
			take(columnDrawnTop(column));
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
			// 地中梁（底盤にぶら下がる台形プリズム）と、その下の床付け（捨てコン・砕石）。
			// **モデルの最深部はふつう底盤の下端ではなく床付けの下端**なので、これを見ないと
			// 余白（kSectionHeightMargin）より深い足元が軸組図で切れる。断面原点が梁下端
			// （v=0）で origin.z が絶対 Z なので、プロファイルの v をそのまま足せば上下端に
			// なる（床付けも同じ断面座標系＝ModifierCommand / BeddingCommand 参照）。
			for (const ModifierCommand& modifier : slab.modifiers)
			{
				for (const Vec2& vertex : modifier.profile)
					take(modifier.origin.z + vertex.y);
				for (const BeddingCommand& bedding : modifier.beddings)
				{
					for (const Vec2& vertex : bedding.profile)
						take(modifier.origin.z + vertex.y);
				}
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

	bool planContentBounds(const Document& document, const std::vector<std::string>& layers,
						   Vec2& min, Vec2& max)
	{
		double minX = std::numeric_limits<double>::max();
		double maxX = std::numeric_limits<double>::lowest();
		double minY = minX;
		double maxY = maxX;
		bool any = false;

		// layers が空なら全部見る（文書全体の広がり）。指定があればそのレイヤに載る命令だけ
		// ——伏図 1 枚が映す範囲になる。
		const auto wanted = [&layers](const std::string& layer)
		{ return layers.empty() || std::ranges::find(layers, layer) != layers.end(); };
		const auto take = [&](const Vec2& point)
		{
			minX = std::min(minX, point.x);
			maxX = std::max(maxX, point.x);
			minY = std::min(minY, point.y);
			maxY = std::max(maxY, point.y);
			any = true;
		};
		const auto takePoint = [&](const std::string& layer, const Vec2& point)
		{
			if (wanted(layer))
				take(point);
		};
		const auto takeSegment = [&](const std::string& layer, const Vec2& start, const Vec2& end)
		{
			if (!wanted(layer))
				return;
			take(start);
			take(end);
		};
		const auto takeBoundary = [&](const std::string& layer, const std::vector<Vec2>& boundary)
		{
			if (!wanted(layer))
				return;
			for (const Vec2& point : boundary)
				take(point);
		};

		for (const GridCommand& grid : document.grids)
			takeSegment(grid.layer, grid.start, grid.end);
		for (const FloorCommand& floor : document.floors)
			takeBoundary(floor.layer, floor.boundary);
		for (const SlabCommand& slab : document.slabs)
			takeBoundary(slab.layer, slab.boundary);
		for (const RoofCommand& roof : document.roofs)
			takeBoundary(roof.layer, roof.boundary);
		// 横架材の端点は取り合い相手の芯線上にあるので、**実際に材が占める端**を見る
		// （垂木を軒先まで見るのと同じ理由。過大でも過小でも縮尺の判断がずれる）。
		for (const MemberCommand& member : document.members)
			takeSegment(member.layer, memberDrawnStart(member), memberDrawnEnd(member));
		for (const WallCommand& wall : document.walls)
			takeSegment(wall.layer, wall.start, wall.end);
		// 垂木は**軒先まで伸ばして描く**（M16。draw/Rafter が rafterEaveEnd でパスの始端を
		// 軒先へ送る）ので、命令の start ではなく軒先を見る——ここで実際より狭く見積もると、
		// 決めた縮尺では図が用紙に収まらない。
		for (const RafterCommand& rafter : document.rafters)
			takeSegment(rafter.layer, rafterEaveEnd(rafter).point, rafter.end);
		for (const ColumnCommand& column : document.columns)
			takePoint(column.layer, column.position);
		for (const ColumnMarkCommand& mark : document.columnMarks)
			takePoint(mark.layer, mark.position);
		// 耐力壁（M19）は柱芯どうしを結ぶ線分。伏図に映る範囲へ含める。
		for (const ShearWallCommand& wall : document.shearWalls)
			takeSegment(wall.layer, wall.start, wall.end);
		// シンボル置換系 5 種は同じ命令型（SymbolCommand）なので同じ扱いで畳む。
		for (const std::vector<SymbolCommand>* list :
			 {&document.anchorBolts, &document.floorPosts, &document.fireBraces, &document.joints,
			  &document.splices})
		{
			for (const SymbolCommand& symbol : *list)
				takePoint(symbol.layer, symbol.position);
		}

		// 断面寸法データタグ（M13）は**注釈**なのでデザインレイヤには載らないが、ビューポート
		// の中には映るので図の広がりに効く。どの伏図に出るかは**関連付け先の横架材が載る
		// レイヤ**が決める（その材が映る図に出る）ので、レイヤの絞り込みもその材のレイヤで
		// 行う——タグ自身は 'layer' を持たない（core/Document.h の TagCommand）。
		//
		// ★**軸組図（sections）のタグは見ない。** あちらの注釈空間は平面座標ではなく
		// **(切断線に沿った距離, 高さ Z)** なので、平面の広がりへ混ぜると意味を成さない
		// （TagCommand の「position は注釈空間の座標」）。
		//
		// ★**拾えるのはタグが接する点（position）まで**である。タグ自身の差し渡しは
		// タグレイアウトの中身が決める**用紙 mm** で、モデル座標へ落とすと縮尺に比例する
		// ——その見込みは kPlanContentMargin が持つ（そちらの doc コメント）。
		for (const SheetCommand& sheet : document.sheets)
		{
			for (const TagCommand& tag : sheet.viewport.tags)
			{
				// 関連付け先が引ければその材のレイヤで絞る。添字が範囲外の命令
				// （validateDocument を通っていない Document）は文書全体の広がりにだけ入れる。
				if (tag.memberIndex < document.members.size())
					takePoint(document.members[tag.memberIndex].layer, tag.position);
				else if (layers.empty())
					take(tag.position);
			}
		}

		if (!any)
			return false;
		min = Vec2{minX - kPlanContentMargin, minY - kPlanContentMargin};
		max = Vec2{maxX + kPlanContentMargin, maxY + kPlanContentMargin};
		return true;
	}

	double sectionAlongOrigin(const SectionCommand& section)
	{
		// 断面線の**終点**（画面右の端）の、切断線に沿った座標。ここが注釈空間の横方向の
		// 原点（parse/Tag.h「断面の注釈空間」）。
		return section.direction == SectionDirection::X ? section.lineEnd.y : section.lineEnd.x;
	}

	Vec2 sectionAnnotationPoint(const Vec2& plan, double elevation, SectionDirection direction,
								double alongOrigin)
	{
		// 画面右方向は視線の向きが決める（parse/Tag.h「断面の注釈空間」）。X通りは −X 方向を
		// 見るので右が +Y、Y通りは +Y 方向を見るので右が +X。**横は断面線の終点からの距離**、
		// 高さはそのまま Z。
		const double right = direction == SectionDirection::X ? plan.y : plan.x;
		return Vec2{right - alongOrigin, elevation};
	}

	Vec2 sectionLabelAnchor(const SectionCommand& section, double rangeStart)
	{
		const Vec2 middle{(section.lineStart.x + section.lineEnd.x) / 2.0,
						  (section.lineStart.y + section.lineEnd.y) / 2.0};
		return sectionAnnotationPoint(middle, rangeStart + kSectionHeightMargin, section.direction,
									  sectionAlongOrigin(section));
	}

	double sectionLabelDrop(const ViewportCommand& viewport)
	{
		int below = -1;
		for (const DimensionChainCommand& chain : viewport.dimensions)
		{
			if (chain.axis == DimensionAxis::Horizontal && chain.side < 0)
				below = std::max(below, chain.tier);
		}
		// 帯は寸法線までの距離＋文字の見込みで、寸法が無ければ 0（core::dimensionBand）。
		return dimensionBand(below) + kSectionLabelGap;
	}

	SectionBands sectionBands(const std::vector<SectionCommand>& sections, bool gridBubbles)
	{
		// 図の右端に根元がある、とみなす遊び（注釈空間・モデル mm）。
		constexpr double kEdgeTol = 1.0;

		SectionBands bands;
		for (const SectionCommand& section : sections)
		{
			const bool haveLevels = !section.levels.empty();
			double rightEdge = std::numeric_limits<double>::lowest();
			for (const LevelMarkCommand& level : section.levels)
				rightEdge = std::max(rightEdge, level.right);

			int left = -1;
			int right = -1;
			int bottom = -1;
			int top = -1;
			for (const DimensionChainCommand& chain : section.viewport.dimensions)
			{
				if (chain.axis == DimensionAxis::Horizontal)
				{
					int& tier = chain.side < 0 ? bottom : top;
					tier = std::max(tier, chain.tier);
				}
				else if (chain.side < 0)
					left = std::max(left, chain.tier);
				else if (!haveLevels || chain.base >= rightEdge - kEdgeTol)
					right = std::max(right, chain.tier);
			}

			const double label = section.viewport.drawingTitle.empty()
									 ? 0.0
									 : kSectionLabelGap + kSectionLabelAllowance;
			bands.left = std::max(bands.left, dimensionBand(left) +
												  (haveLevels ? kLevelMarkBandAllowance : 0.0));
			bands.right = std::max(bands.right, std::max(dimensionBand(right),
														 haveLevels ? kLevelLineOvershoot : 0.0));
			bands.bottom = std::max(bands.bottom, dimensionBand(bottom) + label);
			bands.top =
				std::max(bands.top, std::max(dimensionBand(top),
											 gridBubbles ? kSectionGridBubbleAllowance : 0.0));
		}
		return bands;
	}

	bool sectionContentSize(const Document& document, Vec2& size)
	{
		Vec2 min;
		Vec2 max;
		if (!planContentBounds(document, {}, min, max))
			return false;
		double start = 0.0;
		double end = 0.0;
		if (!sectionHeightRange(document, start, end))
			return false;
		// 幅は平面の広がりの**大きい方**（X通りは Y 方向を、Y通りは X 方向を映すので、
		// どちらも同じ大きさのマスに収まるように大きい方で揃える）。
		size = Vec2{std::max(max.x - min.x, max.y - min.y), end - start};
		return true;
	}

	RafterEaveEnd rafterEaveEnd(const RafterCommand& rafter)
	{
		RafterEaveEnd eave;
		eave.point = rafter.start;
		eave.z = rafter.elevation;
		eave.offset = rafter.startBound.offset;

		// 支持点から軒先までの水平距離＝差し込み（支持点→壁外面）＋軒の出（壁外面→軒先）。
		// 軒桁に乗らない垂木はどちらも 0 で、支持点がそのまま軒先（parse/Rafter.cpp）。
		const double reach = rafter.overhang + rafter.embedment;
		const double dx = rafter.end.x - rafter.start.x;
		const double dy = rafter.end.y - rafter.start.y;
		const double run = std::hypot(dx, dy);
		if (reach <= 0.0 || run <= 0.0)
			return eave;

		// 棟へ向かう単位ベクトルの**逆向き**へ reach だけ進み、勾配（下面 Z の差 ÷ 水平投影
		// 長）ぶん下げる。offset は同じ下がり幅ぶん startBound から引く（レベルは共通なので
		// 差だけで済む）。
		const double drop = (rafter.endElevation - rafter.elevation) / run * reach;
		eave.point = Vec2{rafter.start.x - (dx / run * reach), rafter.start.y - (dy / run * reach)};
		eave.z = rafter.elevation - drop;
		eave.offset = rafter.startBound.offset - drop;
		return eave;
	}

	namespace
	{
		// 端点 from を、to へ向かう向き（材の内側）へ offset だけ戻した点。offset は負値で
		// 材を短くするので、内側へ |offset| 動かすことになる（core/Document.h「端部
		// オフセット」）。長さ 0 の材はそのまま返す。
		Vec2 pullBack(const Vec2& from, const Vec2& to, double offset)
		{
			const Vec2 delta = to - from;
			const double span = length(delta);
			if (span <= 0.0)
				return from;
			return Vec2{from.x - ((delta.x / span) * offset), from.y - ((delta.y / span) * offset)};
		}
	} // namespace

	Vec2 memberDrawnStart(const MemberCommand& member)
	{
		return pullBack(member.start, member.end, member.startOffset);
	}

	Vec2 memberDrawnEnd(const MemberCommand& member)
	{
		return pullBack(member.end, member.start, member.endOffset);
	}

	double columnDrawnBottom(const ColumnCommand& column)
	{
		return column.elevation - column.startOffset;
	}

	double columnDrawnTop(const ColumnCommand& column)
	{
		return column.elevation + column.height + column.endOffset;
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

	std::vector<Vec3> modifierBasePolygon(const ModifierCommand& modifier)
	{
		const std::size_t count = modifier.profile.size();
		if (count < 3)
			return {};

		// 断面 (u, v) をワールドへ写す。u 軸は走る向きを +90 度回した水平単位ベクトル
		// （解析側 parse/Footing の groundBeamModifier の取り方と対）、v 軸はワールド Z。
		const double phi = modifier.azimuth * std::numbers::pi / 180.0;
		const Vec2 axis{std::cos(phi), std::sin(phi)};
		const Vec2 width{-axis.y, axis.x};

		std::vector<Vec3> vertices;
		vertices.reserve(count);
		for (const Vec2& p : modifier.profile)
		{
			vertices.push_back(Vec3{modifier.origin.x + (width.x * p.x),
									modifier.origin.y + (width.y * p.x), modifier.origin.z + p.y});
		}

		// 1. 巻き: 面法線（Newell 法）が軸と逆を向いていたら反転する。
		Vec3 normal{0.0, 0.0, 0.0};
		for (std::size_t i = 0; i < count; ++i)
		{
			const Vec3& a = vertices[i];
			const Vec3& b = vertices[(i + 1) % count];
			normal.x += (a.y - b.y) * (a.z + b.z);
			normal.y += (a.z - b.z) * (a.x + b.x);
			normal.z += (a.x - b.x) * (a.y + b.y);
		}
		if ((normal.x * axis.x) + (normal.y * axis.y) < 0.0)
			std::ranges::reverse(vertices);

		// 2. 始まり: +u へ最も向く辺（同じ向きなら低いほう）の始点を先頭へ回す。法線を軸へ
		// 揃えた後の巻きでは、+u へ向かう辺は断面の下端側にある。
		std::size_t start = 0;
		double bestAlong = -std::numeric_limits<double>::infinity();
		double bestZ = std::numeric_limits<double>::infinity();
		for (std::size_t i = 0; i < count; ++i)
		{
			const Vec3& a = vertices[i];
			const Vec3& b = vertices[(i + 1) % count];
			const double edgeLength = length(b - a);
			if (edgeLength <= 0.0)
				continue;
			const double along = (((b.x - a.x) * width.x) + ((b.y - a.y) * width.y)) / edgeLength;
			const double z = (a.z + b.z) / 2.0;
			if (along > bestAlong + kModifierBaseEdgeTol ||
				(along >= bestAlong - kModifierBaseEdgeTol && z < bestZ))
			{
				start = i;
				bestAlong = along;
				bestZ = z;
			}
		}
		std::ranges::rotate(vertices, vertices.begin() + static_cast<std::ptrdiff_t>(start));
		return vertices;
	}

	namespace
	{
		// スタック最下段（背面）へ回すレベル種別か。床（FL）・野地板のレイヤは伏図
		// ビューポートで柱・梁を覆い隠さないよう全ストーリ分をまとめて背面へ集める（野地板
		// レベルは M6 で追加済み。この並びの適用先は M13 の per-viewport 上書き。
		// desiredStoryLayerOrder の doc コメント参照）。
		bool isBackgroundLevel(const std::string& rawType)
		{
			// 伏図レベルの印（"FL(FL-872)"）は外して元の種別で見る（planLevelTag）。
			const std::string type = stripPlanLevelTag(rawType);
			return type == kLevelFL || type == kLevelNojiita;
		}

		// 逆に、スタック最上段（前面）へ回すレベル種別か。耐力壁（M19）の伏図記号は
		// **横架材・柱と同じ場所に重ねて読ませる注記**なので、実体（材）の絵に隠されると
		// 用を成さない。実機で「記号が横架材の後ろに隠れる」ことを確認して前面へ回した
		// （desiredStoryLayerOrder の doc コメント）。
		bool isForegroundLevel(const std::string& type)
		{
			return stripPlanLevelTag(type) == kLevelShearWall;
		}
	} // namespace

	std::vector<Vec2> shearWallBracePolygon(double clearStart, double clearEnd, double bottom,
											double topAtStart, double topAtEnd, double width,
											bool risesToEnd)
	{
		const double span = clearEnd - clearStart;
		if (span <= 0.0 || topAtStart <= bottom || topAtEnd <= bottom || width <= 0.0)
			return {};

		// 帯の中心線（内法の対角線＝低い側の下隅から高い側の上隅へ）。上辺が傾いていれば
		// 対角線の傾きもそれに従う。
		const Vec2 low{risesToEnd ? clearStart : clearEnd, bottom};
		const Vec2 high{risesToEnd ? clearEnd : clearStart, risesToEnd ? topAtEnd : topAtStart};
		// 上で内法の幅と高さが正だと確かめてあるので、対角線の長さも必ず正になる。
		// ゼロ除算の番人は要らない。
		const Vec2 along{high.x - low.x, high.y - low.y};
		const double length = std::hypot(along.x, along.y);

		// 中心線に直交する半幅ぶんのオフセット。
		const Vec2 offset{-along.y / length * width / 2.0, along.x / length * width / 2.0};
		const std::vector<Vec2> band = {low - offset, high - offset, high + offset, low + offset};
		// 内法（反時計回り）。上辺が水平なら矩形、傾いていれば台形。
		const std::vector<Vec2> frame = {Vec2{clearStart, bottom}, Vec2{clearEnd, bottom},
										 Vec2{clearEnd, topAtEnd}, Vec2{clearStart, topAtStart}};
		return clipPolygonToConvex(band, frame);
	}

	namespace
	{
		// 直線に触れただけの切れ端（面積の無いもの）を捨てる。
		bool hasArea(const std::vector<Vec2>& polygon)
		{
			double area = 0.0;
			for (std::size_t i = 0; i < polygon.size(); ++i)
				area += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
			return std::abs(area) / 2.0 >= kPointEps;
		}
	} // namespace

	std::vector<std::vector<Vec2>> shearWallBehindBracePieces(double clearStart, double clearEnd,
															  double bottom, double topAtStart,
															  double topAtEnd, double width,
															  bool risesToEnd)
	{
		const std::vector<Vec2> behind = shearWallBracePolygon(
			clearStart, clearEnd, bottom, topAtStart, topAtEnd, width, risesToEnd);
		if (behind.empty())
			return {};

		// 手前の筋かい（逆向き）の帯の 2 本の縁。帯は内法の対角線を中心に幅 width
		// （shearWallBracePolygon と同じ作り）。奥の筋かいのうち帯の外にある部分は、
		// 縁のどちらか一方の外側にある。
		const Vec2 low{risesToEnd ? clearEnd : clearStart, bottom};
		const Vec2 high{risesToEnd ? clearStart : clearEnd, risesToEnd ? topAtStart : topAtEnd};
		const Vec2 along = high - low;
		const Vec2 offset = Vec2{-along.y, along.x} * (width / 2.0 / length(along));

		// offset は along の左手なので、low − offset の縁は帯が左、low + offset の縁は
		// 帯が右にある。それぞれ帯と反対の側（前者は右＝逆向きの左、後者は左）を残す。
		std::vector<std::vector<Vec2>> pieces;
		for (std::vector<Vec2> piece :
			 {clipPolygonToHalfPlane(behind, low - offset, Vec2{} - along),
			  clipPolygonToHalfPlane(behind, low + offset, along)})
		{
			if (hasArea(piece))
				pieces.push_back(std::move(piece));
		}
		return pieces;
	}

	namespace
	{
		// 高さの符号（図面の書き方に合わせ、0 は "±"）。
		constexpr const char* kPlanLevelPlus = "+";
		constexpr const char* kPlanLevelMinus = "-";
		constexpr const char* kPlanLevelZero = "±";

		// 印の中身の基準の名前（一般階は FL・最上階は軒高）。
		const char* planLevelDatumName(bool top)
		{
			return top ? kLevelEaves : kLevelFL;
		}
	} // namespace

	std::string signedMillimetreText(long long deltaMm)
	{
		const char* sign = kPlanLevelZero;
		if (deltaMm > 0)
			sign = kPlanLevelPlus;
		else if (deltaMm < 0)
			sign = kPlanLevelMinus;
		// 3 桁ごとのコンマは下の桁から差し込む。
		const std::string digits = std::to_string(deltaMm < 0 ? -deltaMm : deltaMm);
		std::string grouped;
		for (std::size_t i = 0; i < digits.size(); ++i)
		{
			if (i > 0 && (digits.size() - i) % 3 == 0)
				grouped += ',';
			grouped += digits[i];
		}
		return sign + grouped;
	}

	std::string planLevelHeightText(long long heightMm, long long datumMm, bool top)
	{
		// 符号は必ず付ける（"FL872" と "FL-872" を読み違えない。signedMillimetreText）。
		return std::string(planLevelDatumName(top)) + signedMillimetreText(heightMm - datumMm);
	}

	std::string planLevelTag(long long heightMm, long long datumMm, bool top)
	{
		return "(" + planLevelHeightText(heightMm, datumMm, top) + ")";
	}

	std::string stripPlanLevelTag(const std::string& name)
	{
		if (name.empty() || name.back() != ')')
			return name;
		const std::size_t open = name.rfind('(');
		if (open == std::string::npos)
			return name;
		// 中身が「FL か軒高＋符号」で始まるものだけを印とみなす（"柱(通し)" のような
		// 利用者の括弧を剥がさない）。
		const std::string inner = name.substr(open + 1);
		for (const bool top : {false, true})
		{
			const std::string datum = planLevelDatumName(top);
			if (!inner.starts_with(datum))
				continue;
			const std::string rest = inner.substr(datum.size());
			for (const char* sign : {kPlanLevelPlus, kPlanLevelMinus, kPlanLevelZero})
			{
				if (rest.starts_with(sign))
					return name.substr(0, open);
			}
		}
		return name;
	}

	std::vector<std::string> desiredStoryLayerOrder(const std::vector<StoryCommand>& stories,
													const std::vector<std::string>& topLayers)
	{
		std::vector<std::string> order;
		// 通り芯レイヤ "共通"（core::kGridLayer。GridCommand::layer の既定値と同じ）を
		// スタック最上段に置き、続けて topLayers を積む。
		order.emplace_back(kGridLayer);
		order.insert(order.end(), topLayers.begin(), topLayers.end());

		// stories は Elevation 昇順（最下階→最上階）。スタックは最上階→最下階なので逆順に辿る。
		// 前面へ回すレベルは order の**先頭側**（通り芯・topLayers の直後）へ、背面へ回す
		// レベルは末尾へ集める。どちらも階の並び（最上階→最下階）は崩さない。
		std::vector<std::string> foreground;
		std::vector<std::string> background;
		for (const StoryCommand& command : std::views::reverse(stories))
		{
			for (const LevelCommand& level : command.levels)
			{
				if (isForegroundLevel(level.type))
					foreground.push_back(level.layer);
				else if (isBackgroundLevel(level.type))
					background.push_back(level.layer);
				else
					order.push_back(level.layer);
			}
		}
		order.insert(order.begin() + static_cast<std::ptrdiff_t>(1 + topLayers.size()),
					 foreground.begin(), foreground.end());
		order.insert(order.end(), background.begin(), background.end());
		return order;
	}
} // namespace HomeskzIfcImport::core
