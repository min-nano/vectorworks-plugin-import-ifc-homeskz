//
//	core/Document.h
//
//	命令セット（Document）の構造体定義。IFC 解析フェーズ（parse/）と VW 描画
//	フェーズ（draw/）を結ぶ唯一の境界であり、両フェーズはこの構造体だけで接続する。
//	SDK ハンドルや STEP エンティティポインタ等「フェーズ間で運べないもの」は
//	絶対にここへ載せない（CLAUDE.md「アーキテクチャ: 2 フェーズ分離」）。
//
//	Python 版 document.py の TypedDict 群に対応させる。フィールド名・意味は
//	Python 版に合わせ、追跡しやすさと仕様のブレ防止を優先する。予約語（class 等）は
//	drawClass / className のように機械的に置換する。
//
//	現状は「バージョン＋ stories / grids / floors」を持つ（M1 通り芯・M3 ストーリ・
//	M5 床板ぶん）。残りの命令リスト（members / columns / walls / slabs …）は、対応する
//	マイルストーンで要素を移植するたびに 1 つずつ追加する（ROADMAP.md）。
//

#pragma once

#include "core/Geometry.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 命令セットのスキーマバージョン。JSON ダンプ（DocumentJson・任意）や Python 版
	// 期待値とのゴールデン比較で、スキーマ変更を検出できるように持たせておく。
	inline constexpr int kDocumentVersion = 1;

	// 通り芯（グリッド）1 本の描画命令。Python 版 document.py の GridCommand（dict）に
	// 対応する。draw/Grid がこれを GridAxis オブジェクトへ変換する（ROADMAP.md M1）。
	//
	// Python 版キーとの対応:
	//   label     ← 'label'  … 軸名（IfcGridAxis の AxisTag。例 "X1" / "Y1"）
	//   layer     ← 'layer'  … 配置デザインレイヤ名（通り芯は常に "共通"）
	//   drawClass ← 'class'  … クラス名（X 通り / Y 通り。予約語 class を機械置換）
	//   start     ← 'start'  … 始点（bbox 中心でセンタリング済みの平面座標）
	//   end       ← 'end'    … 終点（同上）
	// start/end は 3D ではなく平面（Vec2）: 通り芯は配置行列・断面・ストーリを要さず、
	// ポリラインの端点だけで決まる（M1 が最初の縦切りである理由。ROADMAP.md M1）。
	struct GridCommand
	{
		std::string label;
		std::string layer = "共通";
		std::string drawClass;
		Vec2 start;
		Vec2 end;
	};

	// story 命令内の 1 ストーリレベル。Python 版 document.py の LevelCommand（dict）に
	// 対応する。ストーリレベルとそれに紐づくデザインレイヤ 1 枚を表す。
	//
	// Python 版キーとの対応:
	//   type   ← 'type'   … レベル種別（"FL" / "横架材天端" / "軒高"。CreateLayerLevelType
	//                        へ登録し、GetLayerForStory でレイヤを取り直す鍵になる）
	//   offset ← 'offset' … ストーリ原点（FL／軒高）からの相対高さ（mm。負値=下）
	//   layer  ← 'layer'  … このレベルに紐づくデザインレイヤの意図した名前（"1-FL" 等）
	struct LevelCommand
	{
		std::string type;
		double offset = 0.0;
		std::string layer;
	};

	// ストーリ・ストーリレベル・デザインレイヤを生成する命令。Python 版 document.py の
	// StoryCommand（dict）に対応する。draw/Story がこれを CreateStory＋レベルテンプレート
	// によるレイヤ生成へ変換する（ROADMAP.md M3）。
	//
	// Python 版キーとの対応:
	//   name      ← 'name'      … VectorWorks のストーリ名（"1階" / "2階" / "屋根"）
	//   suffix    ← 'suffix'    … CreateStory の接尾辞（"1" / "2" / "R"。空文字は 2 回目
	//                             以降の CreateStory が失敗するため非空必須）
	//   elevation ← 'elevation' … ストーリ高さ（IfcBuildingStorey.Elevation。mm）
	//   levels    ← 'levels'    … 生成するストーリレベルの列。並び順は**希望する
	//                             デザインレイヤのスタック順（上→下）**を表し、draw/Story が
	//                             その順にレイヤを並べ替える（レベルの高さには依存しない）。
	struct StoryCommand
	{
		std::string name;
		std::string suffix;
		double elevation = 0.0;
		std::vector<LevelCommand> levels;
	};

	// 高さ基準（ストーリレベルへのバインド）1 端分。Python 版 document.py の
	// StoryBoundCommand（dict）に対応する。床・構造材・柱・壁・スラブが共通で使う。
	//
	// Python 版キーとの対応:
	//   storyOffset ← 'story_offset' … 配置先レイヤのストーリからの相対階数（0=自階・1=上階）
	//   level       ← 'level'        … そのストーリのレベル種別名（"横架材天端" / "軒高"）
	//   offset      ← 'offset'       … レベルからの距離（mm。負値=下）
	// SetObjectStoryBound（VS: SetObjectStoryBound）へそのまま渡す 3 つ組。
	struct StoryBoundCommand
	{
		int storyOffset = 0;
		std::string level;
		double offset = 0.0;
	};

	// スラブの高さ基準（elevation と bound が指すスラブ上の面）。VW のスラブは高さの
	// 基準面（データム）を持ち、命令の高さはこの面の絶対 Z を表す。
	//   Top    … スラブ天端（床仕上げ上端）。一般階の床はこちら（＝ FL）。
	//   Bottom … スラブ底面（床下地下端）。ロフト（屋根階）の床はこちら（＝ 横架材天端＝軒高。
	//            ロフトの FL は「軒高 + 36mm」という仮定値なので、確かな構造面を基準にする）。
	enum class SlabDatum
	{
		Top,
		Bottom,
	};

	// スラブの構成層（コンポーネント）1 枚。床は「床仕上げ」「床下地」の 2 層で構成する。
	//
	//   name      … 層の名前（"床仕上げ" / "床下地"）
	//   thickness … 層厚（mm。0 以上）
	// 並び順は**上から下**（先頭が最上層＝床仕上げ）。層厚の合計がスラブの総厚になる。
	struct SlabComponentCommand
	{
		std::string name;
		double thickness = 0.0;
	};

	// 床板（IfcSlab "床版"）を描く命令。Python 版 document.py の FloorCommand（dict）に
	// 対応する。draw/Floor がこれをスラブオブジェクトへ変換する（ROADMAP.md M5。Python 版は
	// 床ツールで描くが、本移植は BIM 機能の充実したスラブを使う。draw/Floor.h 参照）。
	//
	// 【高さの持ち方】elevation は datum が指す**基準面**の絶対 Z（一般階＝床仕上げ上端、
	// ロフト＝床下地下端）。一般階の基準面は一般部で FL と同じ高さで、部分的に床レベルを
	// 指定している場合（スキップフロア等）は FL ± 差分になる。高さ基準は配置先ストーリの
	// レベル（一般階＝FL、ロフト＝軒高）にバインドし、その差分を bound.offset に入れる
	// （一般部は offset 0、段差床は段差ぶんずれる）。
	//
	// 【スラブ構成】components は上から順に 床仕上げ ＋ 床下地（24mm 固定）:
	//   一般階 … 床仕上げ＝FL 高さ − 横架材天端高さ − 床下地厚。合計＝FL 高さ − 横架材天端
	//            高さ、すなわちスラブ下端は（一般部では）横架材天端に一致する。
	//   ロフト … 合計＝軒高からの標準床レベル（36mm。仮定値）なので 床仕上げ 12 ＋ 床下地 24。
	// 構成は**階ごとのスラブスタイル**（styleName）として作り、床はそのスタイルを適用して
	// 描く（階により構成が異なることが多いため、スタイルは階ごとに 1 つ）。
	//
	// Python 版キーとの対応（Python 版は床ツール＋厚み 24mm 固定なので構成が異なる）:
	//   layer      ← 'layer'     … 配置先デザインレイヤ名（"1-FL" 等。既存のみ・無ければスキップ）
	//   drawClass  ← 'class'     … クラス名（床板。予約語 class を機械置換）
	//   boundary   ← 'boundary'  … 床の平面外形（mm・グリッド中心オフセット済み。閉じた
	//                              ポリゴンの頂点列で、末尾に始点を重複させない）
	//   styleName  （Python 版に対応なし）… スラブスタイル名（"1F-床スタイル" 等）
	//   components （Python 版に対応なし）… スタイルの構成層（上から）
	//   datum      （Python 版に対応なし）… 高さ基準の面（一般階＝Top・ロフト＝Bottom）
	//   elevation  ← 'elevation' … **基準面**の絶対 Z（mm。Python 版は床下端）
	//   bound      ← 'bound'     … 基準面の高さ基準（一般階＝FL レベル、ロフト＝軒高レベル、
	//                              ＋段差 offset。Python 版は床下端を横架材天端レベルへ）
	struct FloorCommand
	{
		std::string layer;
		std::string drawClass;
		std::vector<Vec2> boundary;
		std::string styleName;
		std::vector<SlabComponentCommand> components;
		SlabDatum datum = SlabDatum::Top;
		double elevation = 0.0;
		StoryBoundCommand bound;
	};

	// 命令セット本体。プレーンな構造体の集約（std::vector / std::string / double /
	// enum 等）で表す。
	//
	// TODO: 要素ごとに命令リストを追加していく（フィールド名は Python 版のキーに対応）。
	//   * M6 rafters / roofs : std::vector<RafterCommand> / <RoofCommand>
	//   * M7 members         : std::vector<MemberCommand>
	//   * M8 columns         : std::vector<ColumnCommand>
	//   * M9 walls / slabs …
	//   スキーマを変えるときは構造体・validateDocument・テスト・JSON ダンプを同時更新する。
	struct Document
	{
		int version = kDocumentVersion;

		// M3 ストーリ。IfcBuildingStorey を解析して得た StoryCommand の列（Elevation
		// 昇順・最上階が末尾。parse/Story が組み立てる）。以降の要素は配置先レイヤを
		// このストーリのレベルから得るため、描画は grids より先に stories を処理する。
		std::vector<StoryCommand> stories;

		// M1 通り芯。IfcGridAxis を解析して得た GridCommand の列（入力順に依存しない
		// 決定的な並び。parse/Grid が #id 昇順で組み立てる）。
		std::vector<GridCommand> grids;

		// M5 床板。床版（IfcSlab "床版"）を解析して得た FloorCommand の列（階＝Elevation
		// 昇順・階内は #id 昇順で決定的。parse/Floor が組み立てる）。配置先の FL レイヤは
		// stories が作るので、描画は stories の後に処理する。
		std::vector<FloorCommand> floors;
	};

	// Document を描画前に検証する（Python 版 validateDocument 相当）。draw/ は
	// 検証を通った Document だけを SDK API へ渡す。現状はバージョン・stories・floors・
	// grids を見る（規則は Document.cpp の各 isValid* 参照。空の Document は妥当）。
	// 各命令リストの追加に合わせて検証規則を足していく。
	bool validateDocument(const Document& document);

	// 希望するデザインレイヤのスタック順（ナビゲーション上→下）を返す
	// （Python 版 vw/story.py desired_layer_order の SDK 非依存な計算部分）。draw/Story が
	// この順を適用する（レベルの高さには依存しない）。SDK を触らない純計算なので core に
	// 置いて無 SDK で単体テストする（CLAUDE.md「テスト方針」: レイヤ順の並べ替え計算のような
	// SDK から切り離せる部分は core へ寄せてテストする）。
	//
	// ただし**適用先は未定のまま**: VW 2026 ISDK にデザインレイヤの重ね順を変更する呼び出しが
	// 無いため、draw/Story は並べ替えを行わない。目的（伏図で床が柱・梁を覆い隠さない）は
	// M13 の per-viewport 上書き（SetViewportLayerStackingOverride）で満たす（draw/Story.h 参照）。
	//
	// 並び: 最上段に通り芯レイヤ "共通" → topLayers（伏図記号レイヤ等・ストーリ非依存の
	// 独立レイヤ。M12 以降で渡す）→ **最上階→最下階**の順に各ストーリのレイヤ（stories は
	// Elevation 昇順＝最下階→最上階なので逆順に辿る）。各ストーリ内は levels の並び順。
	// ただし床（FL）・野地板レベルのレイヤは全ストーリ分をまとめてスタック最下段（背面）へ
	// 回す（伏図ビューポートで柱・梁を覆い隠さないため）。
	std::vector<std::string> desiredStoryLayerOrder(const std::vector<StoryCommand>& stories,
													const std::vector<std::string>& topLayers = {});
} // namespace HomeskzIfcImport::core
