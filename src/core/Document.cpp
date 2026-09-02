//
//	core/Document.cpp
//
//	validateDocument の実装。SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//
//	現状はバージョンの妥当性と、stories（M3）・floors（M5）・members（M7）・columns（M8）・
//	foundation（M20。立上り・底盤・地中梁の部品）・rafters / roofs（M6）・
//	grids（M1）・シンボル置換系（M11: anchorBolts / floorPosts / fireBraces / joints）・
//	sheets（M13。シートレイヤ上のグラフィック凡例を含む）・sections（M14）・
//	ビューポート注釈の断面寸法データタグ（M13）の
//	各命令の必須フィールド・値域を見る。命令リストが追加されるたびに、対応する検証規則
//	（必須フィールドの有無・参照整合性・値域）をここへ足していく。
//
//	加えて、描画側から切り離せる純計算をここに置く（desiredStoryLayerOrder＝レイヤの希望
//	スタック順、rafterEaveEnd＝垂木の軒先側の材端）。基礎のソリッドの組み立ては
//	core/Foundation にある。SDK を触らないので無 SDK テストで検証できる（CLAUDE.md
//	「テスト方針」）。
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
		// 表示レイヤ 0 枚は「何も映らないビューポート」なので作らせない。伏図（isValidSheet）
		// と軸組図（isValidSection）が同じ規則で見る。
		bool hasDrawableLayers(const ViewportCommand& viewport)
		{
			return !viewport.layers.empty() &&
				   std::ranges::none_of(viewport.layers,
										[](const std::string& layer) { return layer.empty(); });
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

		// 横架材 1 本が妥当か。配置先レイヤ名・クラス名・構造材 ID が非空で、断面（幅・せい）
		// が正で、天端中央線の始端・終端が縮退していないこと（判定は core/Geometry の
		// samePoint）。始端・終端の高さ基準のレベル種別も非空（空だと SetObjectStoryBound
		// が解決できず、高さがレイヤ基準へリセットされる）。elevation / endElevation
		// は数値（double なので常に成立）。
		bool isValidMember(const MemberCommand& member)
		{
			return !member.layer.empty() && !member.drawClass.empty() && !member.memberId.empty() &&
				   member.width > 0.0 && member.height > 0.0 &&
				   !samePoint(member.start, member.end) && !member.startBound.level.empty() &&
				   !member.endBound.level.empty();
		}

		// 柱 1 本が妥当か。配置先レイヤ名（span レイヤ）・クラス名・構造材 ID・構造用途が非空
		// で、断面（幅・せい）と柱高さが正で、上下端の高さ基準のレベル種別が非空であること
		// （空だと SetObjectStoryBound が解決できず、高さがレイヤ基準へリセットされる）。
		// elevation は数値（double なので常に成立）。
		bool isValidColumn(const ColumnCommand& column)
		{
			return !column.layer.empty() && !column.drawClass.empty() && !column.memberId.empty() &&
				   !column.structuralUse.empty() && column.width > 0.0 && column.depth > 0.0 &&
				   column.height > 0.0 && !column.bottomBound.level.empty() &&
				   !column.topBound.level.empty();
		}

		// 基礎の底盤 1 枚が妥当か。外形が 3 点以上（面になる）で、コンクリート厚が正であること。
		// top は数値（double なので常に成立）。
		bool isValidFoundationSlab(const FoundationSlab& slab)
		{
			return slab.boundary.size() >= 3 && slab.thickness > 0.0;
		}

		// 基礎の立上り 1 本が妥当か。幅が正で、壁芯が縮退しておらず（判定は core/Geometry の
		// samePoint）、天端が下端より上にあること（高さ 0 の立上りは実体を持たない）。
		bool isValidFoundationRiser(const FoundationRiser& riser)
		{
			return riser.width > 0.0 && !samePoint(riser.start, riser.end) &&
				   riser.top > riser.bottom;
		}

		// 地中梁 1 本が妥当か。下端の中心線が縮退しておらず、下端幅・せいが正で、張り出し・
		// 斜め部の高さが負でないこと（斜め部の高さがせいを超える分は組み立て側がクランプする）。
		bool isValidFoundationBeam(const FoundationBeam& beam)
		{
			return !samePoint(beam.start, beam.end) && beam.bottomWidth > 0.0 && beam.depth > 0.0 &&
				   beam.haunchLeft >= 0.0 && beam.haunchRight >= 0.0 && beam.haunchHeight >= 0.0;
		}

		// 基礎（M20）が妥当か。配置先レイヤ名・PIO 本体のクラス名・ソリッドの素材クラス名 4 つが
		// 非空で（クラス名は PIO のレコードへ保存され、空だと描いたソリッドが既定クラスに
		// 散る）、部品がすべて妥当で、**部品が 1 つ以上ある**こと（部品の無い基礎は空の PIO
		// になるだけなので、解析側は命令を出さない＝std::optional を空にする）。代表値
		// （params）は数値なので値域は見ない（0 も「取り込み時にその部品が無かった」として正常）。
		bool isValidFoundation(const FoundationCommand& foundation)
		{
			return !foundation.layer.empty() && !foundation.drawClass.empty() &&
				   !foundation.slabClass.empty() && !foundation.riserClass.empty() &&
				   !foundation.leanConcreteClass.empty() && !foundation.gravelClass.empty() &&
				   (!foundation.slabs.empty() || !foundation.risers.empty() ||
					!foundation.beams.empty()) &&
				   std::ranges::all_of(foundation.slabs, isValidFoundationSlab) &&
				   std::ranges::all_of(foundation.risers, isValidFoundationRiser) &&
				   std::ranges::all_of(foundation.beams, isValidFoundationBeam);
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
		// samePoint）、材厚と軸組内法（下端 < 上端）が正であること。
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
				wall.clearSpan <= 0.0)
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

		// 基礎（M20）: 配置先レイヤ名・クラス名が非空で、底盤・立上り・地中梁の部品がすべて
		// 妥当であること（isValidFoundation 参照。docs/DEV-NOTES.md M20）。
		if (document.foundation.has_value() && !isValidFoundation(*document.foundation))
			return false;

		// 垂木・野地板: 配置先レイヤ名・クラス名が非空で、垂木は断面が正・平面が非縮退、
		// 野地板は外形 3 点以上・厚みが正であること（docs/DEV-NOTES.md M6）。
		if (!std::ranges::all_of(document.rafters, isValidRafter))
			return false;
		if (!std::ranges::all_of(document.roofs, isValidRoof))
			return false;

		// シンボル置換系（アンカーボルト・床束・火打・仕口）: 配置先レイヤ名とシンボル名が非
		// 空であること（isValidSymbol 参照。docs/DEV-NOTES.md M11）。4 種は同じ命令型なので同
		// じ規則で見る。
		if (!std::ranges::all_of(document.anchorBolts, isValidSymbol) ||
			!std::ranges::all_of(document.floorPosts, isValidSymbol) ||
			!std::ranges::all_of(document.fireBraces, isValidSymbol) ||
			!std::ranges::all_of(document.joints, isValidSymbol))
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

		// 通り芯: 配置先レイヤ名が空でなく、始点と終点が異なる（縮退していない）こと
		// （isValidGrid 参照）。1 本でも不正なら描画しない（docs/DEV-NOTES.md M1）。
		return std::ranges::all_of(document.grids, isValidGrid);
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
		// 基礎（M20）。底盤は天端と、その下の砕石（kSlabBeddingThickness）の底まで。立上りは
		// 天端と下端。地中梁は天端（底盤の底面）と、下端の下に敷く床付けの底まで——**モデルの
		// 最深部はふつう底盤の下端ではなく床付けの下端**なので、これを見ないと余白
		// （kSectionHeightMargin）より深い足元が軸組図で切れる。
		if (document.foundation.has_value())
		{
			const FoundationCommand& foundation = *document.foundation;
			for (const FoundationSlab& slab : foundation.slabs)
			{
				take(slab.top);
				take(slab.top - slab.thickness - kSlabBeddingThickness);
			}
			for (const FoundationRiser& riser : foundation.risers)
			{
				take(riser.top);
				take(riser.bottom);
			}
			for (const FoundationBeam& beam : foundation.beams)
			{
				take(beam.top);
				take(beam.top - beam.depth - kSlabBeddingThickness);
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
		// 基礎（M20）は部品ごとに平面の座標を持つ（底盤の外形・立上りの壁芯・地中梁の中心線）。
		// どれも同じ "F-基礎" レイヤ上の 1 つの PIO に入る。
		if (document.foundation.has_value())
		{
			const FoundationCommand& foundation = *document.foundation;
			for (const FoundationSlab& slab : foundation.slabs)
				takeBoundary(foundation.layer, slab.boundary);
			for (const FoundationRiser& riser : foundation.risers)
				takeSegment(foundation.layer, riser.start, riser.end);
			for (const FoundationBeam& beam : foundation.beams)
				takeSegment(foundation.layer, beam.start, beam.end);
		}
		for (const RoofCommand& roof : document.roofs)
			takeBoundary(roof.layer, roof.boundary);
		for (const MemberCommand& member : document.members)
			takeSegment(member.layer, member.start, member.end);
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
		// シンボル置換系 4 種は同じ命令型（SymbolCommand）なので同じ扱いで畳む。
		for (const std::vector<SymbolCommand>* list :
			 {&document.anchorBolts, &document.floorPosts, &document.fireBraces, &document.joints})
		{
			for (const SymbolCommand& symbol : *list)
				takePoint(symbol.layer, symbol.position);
		}

		if (!any)
			return false;
		min = Vec2{minX - kPlanContentMargin, minY - kPlanContentMargin};
		max = Vec2{maxX + kPlanContentMargin, maxY + kPlanContentMargin};
		return true;
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
		// スタック最下段（背面）へ回すレベル種別か。床（FL）・野地板のレイヤは伏図
		// ビューポートで柱・梁を覆い隠さないよう全ストーリ分をまとめて背面へ集める（野地板
		// レベルは M6 で追加済み。この並びの適用先は M13 の per-viewport 上書き。
		// desiredStoryLayerOrder の doc コメント参照）。
		bool isBackgroundLevel(const std::string& type)
		{
			return type == kLevelFL || type == kLevelNojiita;
		}

		// 逆に、スタック最上段（前面）へ回すレベル種別か。耐力壁（M19）の伏図記号は
		// **横架材・柱と同じ場所に重ねて読ませる注記**なので、実体（材）の絵に隠されると
		// 用を成さない。実機で「記号が横架材の後ろに隠れる」ことを確認して前面へ回した
		// （desiredStoryLayerOrder の doc コメント）。
		bool isForegroundLevel(const std::string& type)
		{
			return type == kLevelShearWall;
		}
	} // namespace

	std::vector<Vec2> shearWallBracePolygon(double clearStart, double clearEnd, double bottom,
											double top, double width, bool risesToEnd)
	{
		const double span = clearEnd - clearStart;
		const double height = top - bottom;
		if (span <= 0.0 || height <= 0.0 || width <= 0.0)
			return {};

		// 帯の中心線（内法の対角線）。
		const Vec2 low{risesToEnd ? clearStart : clearEnd, bottom};
		const Vec2 high{risesToEnd ? clearEnd : clearStart, top};
		// 上で内法の幅と高さが正だと確かめてあるので、対角線の長さも必ず正になる
		// （length ≥ height > 0）。ゼロ除算の番人は要らない。
		const Vec2 along{high.x - low.x, high.y - low.y};
		const double length = std::hypot(along.x, along.y);

		// 中心線に直交する半幅ぶんのオフセット。
		const Vec2 offset{-along.y / length * width / 2.0, along.x / length * width / 2.0};
		const std::vector<Vec2> band = {low - offset, high - offset, high + offset, low + offset};
		const Vec2 clipMin{std::min(clearStart, clearEnd), bottom};
		const Vec2 clipMax{std::max(clearStart, clearEnd), top};
		return clipPolygonToRect(band, clipMin, clipMax);
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
