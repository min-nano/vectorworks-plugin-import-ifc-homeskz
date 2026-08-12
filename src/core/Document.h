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
//	現状は「バージョン＋ stories / grids / floors / members / columns / rafters / roofs /
//	walls / wallJoins / slabs」を持つ（M1 通り芯・M3 ストーリ・M5 床板・M6 屋根組・M7 横架材・
//	M8 柱・M9 基礎・M10 基礎の高度化ぶん）。残りの命令リスト（anchorBolts …）は、対応する
//	マイルストーンで要素を移植するたびに 1 つずつ追加する（ROADMAP.md）。
//

#pragma once

#include "core/Geometry.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 命令セットのスキーマバージョン。解析側と描画側でスキーマの世代が食い違ったこと
	// （dev/stable 混在や部分的な再ビルド）を検出できるように持たせておく。
	inline constexpr int kDocumentVersion = 1;

	// 通り芯を置くデザインレイヤ名（Python 版 vw/story.py GRID_LAYER）。GridCommand の
	// 既定値と desiredStoryLayerOrder の最上段が同じ名前を指す必要があるので、ここに 1 つだけ置く。
	inline constexpr const char* kGridLayer = "共通";

	// ストーリレベルの種別名。**命令セットの語彙なのでここが唯一の定義**で、
	// LevelCommand::type / StoryBoundCommand::level に入る値と、デザインレイヤ名の接尾辞
	// （"1-FL" の "FL"）がこれになる。parse/ 側（parse/Story.h・parse/Rafter.h・parse/Roof.h）は
	// 要素ごとの読みやすい名前でこれを再公開するだけで、文字列を自前で持たない。
	//
	// core が持つ理由: desiredStoryLayerOrder は「床（FL）・野地板のレイヤを背面へ回す」
	// 判定でこの名前を要するが、core/ は parse/ を include できない（依存の向き。
	// CLAUDE.md「依存の向きは厳守する」）。かつては core/Document.cpp が "FL" / "野地板" を
	// 独自に書いており、parse 側で名前を変えると背面送りが黙って効かなくなる形だった。
	inline constexpr const char* kLevelFL = "FL";
	inline constexpr const char* kLevelBeamTop = "横架材天端";
	inline constexpr const char* kLevelEaves = "軒高";
	inline constexpr const char* kLevelTaruki = "垂木";
	inline constexpr const char* kLevelNojiita = "野地板";
	// M7 横架材のうち、梁（小屋梁・軒桁）と重なって見にくい小屋組の材を分離する
	// 専用レベル。母屋・棟木は "母屋"、登り梁は "登り梁"（parse/Member.h 参照）。
	inline constexpr const char* kLevelMoya = "母屋";
	inline constexpr const char* kLevelNoboribari = "登り梁";
	// M9/M11 基礎ストーリのレベル。GL は基礎ストーリの原点（常に 0）で立上り（"F-立上り"）を、
	// 底盤天端は底盤コンクリートの天端で底盤（"F-底盤"）を載せる（M9）。基礎天端は立上りの
	// 天端でアンカーボルト（"F-アンカーボルト"）を、床束は底盤天端に揃えて床束（"F-床束"）を
	// 載せる（M11。シンボルは高さを持たず、この 2 つのレベルが Z を決める）。
	// スタック順は 基礎天端 → GL → 床束 → 底盤天端（parse/Footing の
	// buildFoundationStoryCommand）。
	inline constexpr const char* kLevelGL = "GL";
	inline constexpr const char* kLevelSlabTop = "底盤天端";
	inline constexpr const char* kLevelFoundationTop = "基礎天端";
	inline constexpr const char* kLevelFloorPost = "床束";

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
		std::string layer = kGridLayer;
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

	// 複合オブジェクト（スラブ・壁）の構成層（VW の「構成要素」）1 枚。
	//   スラブ … 床は「床仕上げ」「床下地」、基礎の底盤は「コンクリート」「捨てコン」「砕石」
	//   壁     … 基礎の立上りは「コンクリート」1 層
	//
	//   name      … 層の名前（"床仕上げ" / "コンクリート" …）
	//   thickness … 層厚（mm。0 以上）
	// 並び順は**上から下**（スラブ）／**外から内**（壁）で、先頭が最初の層。層厚の合計が
	// そのオブジェクトの総厚（スラブ厚・壁厚）になる。
	struct ComponentCommand
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
		std::vector<ComponentCommand> components;
		SlabDatum datum = SlabDatum::Top;
		double elevation = 0.0;
		StoryBoundCommand bound;
	};

	// 垂木（軸組ツール FramingMember、部材種別 rafter）を描く命令。Python 版 document.py の
	// RafterCommand（dict）に対応する。draw/Rafter がこれを軸組ツールへ変換する
	// （ROADMAP.md M6）。垂木はホームズ君 IFC に一切出力されないため、**屋根版（IfcSlab の
	// 屋根面）の勾配・外形から導出**した結果がこの命令になる（parse/Rafter.h 参照）。
	//
	// 【高さと両端の持ち方】start は**軒側＝支持点**（屋根面が横架材天端／軒高の Z レベルと
	// 交わる点＝受ける軒桁の芯線の真上）、end は**棟側**（高い端）。elevation /
	// endElevation はそれぞれの天端 Z（絶対値）。描画フェーズはこの 2 点と両端 Z から
	// 水平投影長・平面方位角・勾配（pitch）を求める。命令の高さは屋根面由来の絶対 Z なので、
	// 配置先レイヤ（"n-垂木"）のレベルのオフセットには依存しない。
	//
	// Python 版キーとの対応:
	//   layer         ← 'layer'          … 配置先デザインレイヤ名（"n-垂木"）
	//   drawClass     ← 'class'          … クラス名（小屋組-垂木。予約語 class を機械置換）
	//   width         ← 'width'          … 断面幅（既定 45mm）
	//   height        ← 'height'         … 断面せい（既定 45mm）
	//   start         ← 'start'          … 軒側＝支持点の平面座標（センタリング済み）
	//   end           ← 'end'            … 棟側の平面座標（同上）
	//   elevation     ← 'elevation'      … 支持点の天端 Z（絶対値＝横架材天端／軒高）
	//   endElevation  ← 'end_elevation'  … 棟側の天端 Z（絶対値）
	//   overhang      ← 'overhang'       … 軒の出（壁外面から軒先までの水平距離）
	//   embedment     ← 'embedment'      … 支持部分の差し込み（受ける軒桁の桁幅の半分）
	//   label         ← 'label'          … 仕様ラベル（"45×45@455"）
	struct RafterCommand
	{
		std::string layer;
		std::string drawClass;
		double width = 0.0;
		double height = 0.0;
		Vec2 start;
		Vec2 end;
		double elevation = 0.0;
		double endElevation = 0.0;
		double overhang = 0.0;
		double embedment = 0.0;
		std::string label;
	};

	// 野地板（屋根の下地合板）を屋根面オブジェクト（Roof Face）として描く命令。Python 版
	// document.py の RoofCommand（dict）に対応する。draw/Roof がこれを屋根面オブジェクトへ
	// 変換する（ROADMAP.md M6。Python 版の BeginRoof に相当する呼び出しは ISDK に無いため、
	// VWFC の VWRoofFaceObj を組み立てる。draw/Roof.cpp 冒頭参照）。屋根版（IfcSlab の
	// 屋根面）**1 面につき 1 枚**で、垂木のように間隔で割らない。
	//
	// 【高さの持ち方】elevation は**軒（屋根軸）の天端 Z の絶対値**。野地板は垂木の上に
	// 載る（野地板下端＝垂木上端）ため、屋根版の平面（＝垂木下面）から垂木せいを鉛直換算して
	// 持ち上げた値になる（parse/Roof.h 参照）。屋根オブジェクトはレイヤ基準（レイヤ相対）の
	// 座標系を持つため、描画フェーズが「絶対 Z − レイヤ Z」へ読み替える。
	//
	// Python 版キーとの対応:
	//   layer     ← 'layer'       … 配置先デザインレイヤ名（"n-野地板"）
	//   drawClass ← 'class'       … クラス名（耐力面材-屋根。予約語 class を機械置換）
	//   boundary  ← 'boundary'    … 屋根の水平投影外形（閉じたポリゴンの頂点列。末尾に
	//                               始点を重複させない。センタリング済み）
	//   axisStart ← 'axis_start'  … 軒（最も低い辺）に沿う屋根軸の始点
	//   axisEnd   ← 'axis_end'    … 同 終点（軸の向きが軒の向き）
	//   upslope   ← 'upslope'     … 棟（高い）側を指す upslope 定義点
	//   rise      ← 'rise'        … 勾配の rise（屋根面の単位法線の水平成分 dh）
	//   run       ← 'run'         … 勾配の run（同 鉛直成分 nz。slope = rise/run = tanθ）
	//   thickness ← 'thickness'   … 野地板厚（12mm 固定）
	//   elevation ← 'elevation'   … 軒（屋根軸）の天端 Z（絶対値）
	struct RoofCommand
	{
		std::string layer;
		std::string drawClass;
		std::vector<Vec2> boundary;
		Vec2 axisStart;
		Vec2 axisEnd;
		Vec2 upslope;
		double rise = 0.0;
		double run = 0.0;
		double thickness = 0.0;
		double elevation = 0.0;
	};

	// 横架材（構造材ツール StructuralMember）を描く命令。Python 版 document.py の
	// MemberCommand（dict）に対応する。draw/Member がこれを構造材オブジェクトへ変換する
	// （ROADMAP.md M7）。土台・梁・桁のほか、母屋・棟木・登り梁もこの命令で表し、
	// 配置先レイヤと高さ基準レベルだけが専用のもの（"n-母屋" / "n-登り梁"）になる。
	//
	// 【高さと両端の持ち方】start / end は**天端中央線**（断面基準点＝左右中央・上端が
	// 通る線）の平面座標で、elevation / endElevation がそれぞれの天端 Z（絶対値）。
	// ホームズ君 IFC の配置点は断面中心なので、解析側が背/2 だけ持ち上げてここへ入れる
	// （parse/Member.h「基準点補正」）。両者が異なれば傾斜梁（登り梁・隅木等）。
	//
	// 【傾斜はバインドの offset 差だけで表す】描画側は**パスに Z 成分を持たせない**。
	// 構造材ツールの高さバインドはパス由来の部材長へ加算されるため、パスにも傾斜を
	// 持たせると二重に適用される（Python 版 #54 と同種。draw/Member.cpp 冒頭）。
	//
	// Python 版キーとの対応:
	//   layer        ← 'layer'          … 配置先デザインレイヤ名（"1-横架材天端" / "R-軒高" /
	//                                     "n-母屋" / "n-登り梁"）
	//   memberId     ← 'member_id'      … 構造材 ID（"120×180 - 杉…"。材種が無ければ "120×180"）
	//   drawClass    ← 'class'          … クラス名（土台／床梁／軒桁／母屋…。予約語 class を機械置換）
	//   start        ← 'start'          … 天端中央線の始端（センタリング済みの平面座標）
	//   end          ← 'end'            … 同 終端
	//   width        ← 'width'          … 断面幅（mm）
	//   height       ← 'height'         … 断面せい（背。mm）
	//   elevation    ← 'elevation'      … 始端の天端 Z（絶対値）
	//   endElevation ← 'end_elevation'  … 終端の天端 Z（絶対値）
	//   startBound   ← 'start_bound'    … 始端の高さ基準（配置先レベル＋天端との差）
	//   endBound     ← 'end_bound'      … 終端の高さ基準（同上）
	struct MemberCommand
	{
		std::string layer;
		std::string memberId;
		std::string drawClass;
		Vec2 start;
		Vec2 end;
		double width = 0.0;
		double height = 0.0;
		double elevation = 0.0;
		double endElevation = 0.0;
		StoryBoundCommand startBound;
		StoryBoundCommand endBound;
	};

	// 柱（構造材ツール StructuralMember）を鉛直材として描く命令。Python 版 document.py の
	// ColumnCommand（dict）に対応する。draw/Column がこれを構造材オブジェクトへ変換する
	// （ROADMAP.md M8）。管柱・通し柱・小屋束をこの命令で表し、種別の違いは drawClass と
	// structuralUse（構造用途）に出る。
	//
	// 【配置先レイヤは span（またぐレベル区間）】柱は階のレイヤではなく "{from}to{to}-柱"
	// という**span 専用レイヤ**に置く（"1to2-柱" / "2to2.5-柱"）。from は柱が立つ床レベル
	// （1 始まり）、to は上端が届く床／屋根面レベル（管柱＝次階の整数・屋根束＝屋根面で
	// 止まる半整数・通し柱＝複数階ぶん上）。伏図が切断レベルで表示レイヤを絞れるようにする
	// ための分け方で、下屋の小屋束が上階の小屋伏図へ写り込まない（parse/Column.h 参照）。
	//
	// 【高さの持ち方】elevation は**柱下端**の絶対 Z、height は柱高さ（押し出し Depth）で、
	// 上端は elevation + height。描画側はこの 2 つから鉛直パスを作り、加えて上下端を
	// ストーリレベルへバインドする（bottomBound / topBound）——**どちらが欠けても正しい
	// 高さで描かれない**（parse/Column.h 参照）。柱（管柱・通し柱）は下端を当階・上端を
	// 上階（storyOffset=1）の横架材天端（最上階は軒高）へ、小屋束は上下端とも当階の
	// 横架材天端へバインドし、offset にはそれぞれ実際の下端／上端の絶対 Z までの距離を
	// 入れる（＝**バウンドの差は常に柱高さ**になる）。
	//
	// Python 版キーとの対応:
	//   layer          ← 'layer'           … 配置先デザインレイヤ名（"1to2-柱" 等）
	//   memberId       ← 'member_id'       … 構造材 ID（"105×105 - 管柱 / 柱頭金物:(ろ)"）
	//   drawClass      ← 'class'           … クラス名（通し柱／管柱／小屋束。予約語 class を機械置換）
	//   structuralUse  ← 'structural_use'  … 構造用途（柱="4" / 小屋束="5"）
	//   position       ← 'position'        … 断面中心の平面座標（センタリング済み）
	//   width          ← 'width'           … 断面幅（mm）
	//   depth          ← 'depth'           … 断面せい（mm）
	//   height         ← 'height'          … 柱高さ（mm）
	//   elevation      ← 'elevation'       … 柱下端の絶対 Z（mm）
	//   topHardware    ← 'top_hardware'    … 柱頭金物の仕様（無ければ空文字）
	//   bottomHardware ← 'bottom_hardware' … 柱脚金物の仕様（同上）
	//   bottomBound    ← 'bottom_bound'    … 下端の高さ基準
	//   topBound       ← 'top_bound'       … 上端の高さ基準
	struct ColumnCommand
	{
		std::string layer;
		std::string memberId;
		std::string drawClass;
		std::string structuralUse;
		Vec2 position;
		double width = 0.0;
		double depth = 0.0;
		double height = 0.0;
		double elevation = 0.0;
		std::string topHardware;
		std::string bottomHardware;
		StoryBoundCommand bottomBound;
		StoryBoundCommand topBound;
	};

	// 基礎の立上り（基礎梁）を壁オブジェクトとして描く命令。Python 版 document.py の
	// WallCommand（dict）に対応する。draw/Footing がこれを壁へ変換する（ROADMAP.md M9）。
	//
	// 【高さの持ち方】立上りは基礎ストーリの GL（下端）と 1 階（上階）の横架材天端（上端）に
	// バインドし、実形状の絶対 Z との差を各 offset に入れる。**壁だけは高さ基準に汎用の
	// SetObjectStoryBound ではなく壁専用の SetWallOverallHeights を使う**（前者ではレイヤの
	// 「壁の高さ」設定に引きずられる。Python 版 CLAUDE.md「基礎」節）。したがって
	// bottomBound / topBound の storyOffset は SetWallOverallHeights の story 引数
	// （0=自階・1=上階）とそのまま一致する。
	//
	// 【下端は IFC 実形状のまま】ホームズ君は基礎梁を底盤の底面までの全高でモデリングするので、
	// bottomBound.offset にはソリッドの下端がそのまま入る（呑み込み等の補正はしない。
	// parse/Footing.h「下端は IFC 実形状のまま」）。
	//
	// Python 版キーとの対応（reinforcement＝配筋は M10）:
	//   layer       ← 'layer'        … 配置先デザインレイヤ名（"F-立上り"）
	//   drawClass   ← 'class'        … クラス名（立ち上がり。予約語 class を機械置換）
	//   start       ← 'start'        … 壁芯の始点（センタリング済みの平面座標）
	//   end         ← 'end'          … 壁芯の終点（同上）
	//   thickness   ← 'thickness'    … 壁厚（矩形断面の幅。mm）
	//   styleName   （Python 版は描画側の定数）… 壁スタイル名（"基礎立上り - コンクリート 150mm"）
	//   components  （Python 版に対応なし）… スタイルの構成層（コンクリート 1 層）
	//   bottomBound ← 'bottom_bound' … 下端の高さ基準（基礎の GL レベル）
	//   topBound    ← 'top_bound'    … 上端の高さ基準（1 階の横架材天端レベル）
	//   capStart    （Python 版に対応なし）… 壁芯**始点側**の端部を閉じるか
	//   capEnd      （同上）          … 壁芯**終点側**の端部を閉じるか
	//
	// 【端部を閉じるかは解析側が決める】VW の壁は端部の「キャップ」（端を閉じる線）を
	// 壁ごとに持ち、既定値はドキュメントの壁ツール設定に従う。**結合（JoinWalls）任せに
	// すると、どの端が閉じるかがその設定と VW の結合実装に左右される**ので、解析側で
	// 「その端が何と取り合うか」から決めて命令に載せ、描画側は SetWallCaps でそのまま
	// 設定する（Python 版は JoinWalls の capped 引数だけに任せていた部分）。規則は
	//   * 自由端（何とも結合しない端）              … 閉じる
	//   * 同じ天端の立上りと結合する端              … 閉じない（コンクリートで一体）
	//   * 天端の違う立上りとだけ結合する端          … 閉じる（低いほうの端部が見える）
	// で、これは壁結合命令の capped（WallJoinCommand）と同じ判断を**端ごと**に写したもの。
	// 算出は parse/Footing の applyWallCaps。
	//
	// 【壁スタイルは厚みごとに 1 つ】Python 版は既製の `基礎 - 木造ベタ基礎150mm` を全ての
	// 立上りへ当てるが、本移植は**壁厚ごとのスタイルを解析側が名乗り、描画側がコード上の
	// 構成から新規作成する**（実データの壁厚は 120 / 150 / 300mm と混在するので、150mm 固定の
	// 既製スタイルでは厚みが合わない）。既存リソースは上書きしない（draw/Footing.cpp 参照）。
	struct WallCommand
	{
		std::string layer;
		std::string drawClass;
		Vec2 start;
		Vec2 end;
		double thickness = 0.0;
		std::string styleName;
		std::vector<ComponentCommand> components;
		StoryBoundCommand bottomBound;
		StoryBoundCommand topBound;
		bool capStart = true;
		bool capEnd = true;
	};

	// 交差する立上り（壁）同士を VW の壁結合（JoinWalls）で結合する種別。値は SDK の
	// JoinModifierType（kTWallJoin=1 / kLWallJoin=2 / kXWallJoin=3 / kAutoWallJoin=4）と
	// 一致させてあり、draw/Footing はそのまま JoinWalls へ渡す
	// （Python 版 _JOIN_T / _JOIN_L / _JOIN_X。Auto は Python 版に無い）。
	// T 字結合・隅（L）結合・交差（X）結合は VW の壁結合の 3 モードで、**交差結合は T 字結合
	// 2 つとは別処理**。十字は縦横 2 本の壁のままにして X で繋ぐ（切って T 2 件に置き換えるのは
	// モデルとして誤り。ROADMAP.md M10）。
	enum class WallJoinType
	{
		T = 1, // 端点で突き当たる壁（stem）を通し壁（through）へ延長して繋ぐ
		L = 2, // 端点どうしのコーナー（隅結合）
		X = 3, // 内部どうしの十字（交差結合）
		Auto = 4, // ピック点を無視して VW に種別を判断させる（parse/Footing.cpp の makeT。
		// 同じ通し壁の同じ交点に 2 本目の stem が取り付くときだけ使う）
	};

	// 交差する立上り（壁）2 本を結合する命令。Python 版 document.py の WallJoinCommand
	// （dict）に対応する。draw/Footing がこれを JoinWalls へ変換する（ROADMAP.md M10）。
	//
	// 【壁ハンドルは命令インデックスで受け渡す】a / b は Document::walls の**添字**で、
	// 描画側は立上りを描くときに「命令インデックス → 壁ハンドル」の対応表を作り、ここから
	// 引く。**SDK ハンドルは Document に載せない**（フェーズ間で運べない。CLAUDE.md
	// 「所有権」）。どちらかの壁が未配置（レイヤ未生成・フォールバック描画）の命令は
	// 描画側がスキップする。
	//
	// Python 版キーとの対応:
	//   a        ← 'a'         … 結合する壁の walls 内インデックス（T 結合では stem＝延長される側）
	//   b        ← 'b'         … 同上（T 結合では through＝通し壁）
	//   point    ← 'point'     … 2 壁の壁芯の交点（参照用。センタリング済み）
	//   pickA    ← 'pick_a'    … a に渡すピック点（**交点ではなく「残す側」へ寄せた点**）
	//   pickB    ← 'pick_b'    … b に渡すピック点（同上）
	//   joinType ← 'join_type' … 結合種別（JoinWalls の joinModifier）
	//   capped   ← 'capped'    … 結合部を閉じるか（天端高さの違う立上りどうしは閉じる）
	//
	// 【ピック点を交点からずらす理由】壁芯の交点は相手壁の壁芯上にも乗るため「どちら側を
	// 残すか」が曖昧になり、VW が L 結合でコーナーを詰めず立上りが相手壁の外面まで伸びた
	// まま残る（Python 版 #84）。交点から**遠い端点の方向**へ控えめに寄せた点を渡して
	// 残す区間を明示する（算出は parse/Footing の keptSidePick）。
	struct WallJoinCommand
	{
		std::size_t a = 0;
		std::size_t b = 0;
		Vec2 point;
		Vec2 pickA;
		Vec2 pickB;
		WallJoinType joinType = WallJoinType::L;
		bool capped = false;
	};

	// 地中梁（台形断面プリズム）1 本。Python 版 document.py の ModifierCommand（dict）に
	// 対応し、底盤（SlabCommand::modifiers）にぶら下がる（ROADMAP.md M10）。
	//
	// 【なぜスラブ命令にしないか】地中梁は**台形断面**（下端が狭く上端が広い下り梁）なので、
	// 一様な厚みしか持てない単一のスラブでは描けない。底盤のコンクリートに噛み合う
	// 台形プリズムとして表し、描画側はこれを **2 回**作る（draw/Footing.h 参照）:
	//   1. 削り取りモディファイア … スラブのプロファイル群へ渡して底盤を clip する
	//   2. 可視の 3D ソリッド     … 削り取った位置を地中梁のコンクリートで埋める
	//
	// 【断面の座標系】profile は断面の 2D 頂点列 (u, v) で、u＝幅軸（押し出し方向を +90 度
	// 回した水平軸）・v＝鉛直軸（v=0 が断面原点＝梁下端）。origin は断面原点のワールド
	// 絶対座標（XY はセンタリング済み・z は絶対値）で、azimuth は押し出し方向（梁の走る
	// 向き）の方位角（度・+X から反時計回り）。**u 軸の取り方は描画側の復元規約と対で
	// 決まっている**ので、片方だけ変えてはいけない（parse/Footing の groundBeamModifier と
	// draw/Footing の ModifierPrism）。
	//
	// Python 版キーとの対応:
	//   profile ← 'profile' … 断面の 2D 頂点列（u, v）
	//   depth   ← 'depth'   … 押し出し長（軸方向。mm）
	//   origin  ← 'origin'  … 断面原点のワールド絶対座標（[x, y, z]）
	//   azimuth ← 'azimuth' … 押し出し方向の方位角（度）
	struct ModifierCommand
	{
		std::vector<Vec2> profile;
		double depth = 0.0;
		Vec3 origin;
		double azimuth = 0.0;
	};

	// 地中梁の**可視ソリッド**用に、天端（profile の最大 v）を底盤側へ bite だけ持ち上げた
	// コピーを返す（Python 版 vw/footing.py の _bite_modifier）。地中梁の天端は底盤の底面と
	// ちょうど接する（coplanar）ため、そのままだと断面ビューポートで境界線が不安定に出る。
	// 可視ソリッドだけを少し大きくして底盤本体に重ねることで境界線を消す（**削り取り
	// モディファイアは実形状のまま**にする——削り取りも一緒に上げると底盤にできるノッチと
	// 可視ソリッドが再び面ちょうど接して境界線が残る）。
	//
	// 地中梁は台形断面で側辺が斜めなので、天端頂点を**真上へ**上げると側面の勾配が変わって
	// 削り取りの斜面とずれる。そこで各天端頂点を隣接する側辺（下端側の頂点へ向かう斜辺）の
	// 延長線上へ動かす（v を bite 上げるのに合わせて u も勾配ぶんずらす）。側辺が見つからない
	// ／ほぼ水平な頂点は真上へ上げる。bite が 0 以下なら入力をそのまま返す。
	//
	// **core に置く理由**: SDK を触らない純計算で、無 SDK テストで検証できるため
	// （desiredStoryLayerOrder と同じ立ち位置。CLAUDE.md「テスト方針」: draw から切り離せる
	// ロジックは core へ寄せる）。
	ModifierCommand raiseModifierTop(const ModifierCommand& modifier, double bite);

	// 地中梁の天端とみなす頂点の許容差（mm）。最大 v からこの差以内の頂点を天端の辺とみなす
	// （Python 版 _BITE_VERTEX_TOL）。raiseModifierTop と、その期待値を書くテストが共有する。
	inline constexpr double kModifierTopVertexTol = 0.5;

	// 基礎の底盤をスラブオブジェクトとして描く命令。Python 版 document.py の SlabCommand
	// （dict）に対応する。draw/Footing がこれをスラブへ変換する（ROADMAP.md M9）。床板
	// （FloorCommand）と描画の作法は同じで、構成層とスタイル名の中身だけが基礎向けになる。
	//
	// 【高さの持ち方】elevation は**コンクリート天端**（＝底盤天端）の絶対 Z で、datum は
	// 常に Top（最上層＝コンクリートの上端）。基礎ストーリは GL=0 なので、この絶対 Z は
	// ストーリ基準高さとも一致する。bound は底盤天端レベルへのバインドで、offset は実天端 Z と
	// 底盤天端レベル（面積最大の天端 Z）の差（主たる底盤は ≈0、独立基礎底盤等はずれる）。
	//
	// 【スラブ構成】components は上から コンクリート（thickness）＋ 捨てコン ＋ 砕石。
	// styleName はコンクリート厚ごとに 1 つ（"基礎スラブ - コンクリート 150mm / 捨てコン
	// 30mm / 砕石 100mm"）で、厚みの違う底盤は別スタイルになる（parse/Footing.h）。
	//
	// Python 版キーとの対応（reinforcement＝配筋は M10 の残り）:
	//   layer      ← 'layer'      … 配置先デザインレイヤ名（"F-底盤"）
	//   drawClass  ← 'class'      … クラス名（基礎スラブ。予約語 class を機械置換）
	//   boundary   ← 'boundary'   … 平面外形（mm・グリッド中心オフセット済み。閉じた
	//                               ポリゴンの頂点列で、末尾に始点を重複させない）
	//   styleName  （Python 版は描画側が厚みから引く）… スラブスタイル名
	//   components （同上）… スタイルの構成層（上から）
	//   datum      （同上）… 高さ基準の面（底盤は常に Top＝コンクリート天端）
	//   thickness  ← 'thickness'  … コンクリート厚（mm。整数に丸めた値）
	//   elevation  ← 'elevation'  … コンクリート天端の絶対 Z
	//   bound      ← 'bound'      … 天端の高さ基準（底盤天端レベル＋差分）
	//   modifiers  ← 'modifiers'  … この底盤に噛み合う地中梁（台形プリズム）。無ければ空
	struct SlabCommand
	{
		std::string layer;
		std::string drawClass;
		std::vector<Vec2> boundary;
		std::string styleName;
		std::vector<ComponentCommand> components;
		SlabDatum datum = SlabDatum::Top;
		double thickness = 0.0;
		double elevation = 0.0;
		StoryBoundCommand bound;
		std::vector<ModifierCommand> modifiers;
	};

	// ハイブリッドシンボルを平面座標＋回転角で置く命令。アンカーボルト・床束・火打・仕口の
	// 4 種（ROADMAP.md M11「シンボル置換系」）が共通で使う。draw/Symbol がこれをシンボル
	// オブジェクトへ変換する。
	//
	// ［Python 版との差異・意図的］Python 版は AnchorBoltCommand / FloorPostCommand /
	// FireBraceCommand / JointCommand の 4 つの TypedDict を持つが、中身は
	// (layer, symbol, position ＋ 火打・仕口だけ angle) で同型であり、描画側
	// （vw/{anchor_bolt,floor_post,fire_brace,joint}.py）に至っては「配置先レイヤが
	// 在るか確かめて vs.Symbol を呼ぶ」だけの**逐語的に同じ実装が 4 本**ある。C++ では
	// 構造体 1 つ・描画 1 つ（draw/Symbol）へまとめる（CLAUDE.md「重複を作らない置き場所」）。
	// 要素の区別は **Document のどのリストに入っているか**が担い、進捗の見出しと完了
	// ダイアログの件数は従来どおり要素ごとに出る。角度を持たない命令（アンカーボルト・
	// 床束）は angle = 0 ＝シンボルの基準姿勢。
	//
	// Python 版キーとの対応:
	//   layer    ← 'layer'    … 配置先デザインレイヤ名（既存のみ・無ければスキップ）
	//   symbol   ← 'symbol'   … 置換するハイブリッドシンボル名（"アンカーボルト_M12" 等）
	//   position ← 'position' … シンボルの基準点（センタリング済みの平面座標）
	//   angle    ← 'angle'    … 回転角（度・反時計回り。持たない命令は 0）
	// **高さは持たない**: 配置先レイヤのストーリレベル（基礎天端／底盤天端／横架材天端…）が
	// シンボルの Z を決める（Python 版と同じ設計）。
	struct SymbolCommand
	{
		std::string layer;
		std::string symbol;
		Vec2 position;
		double angle = 0.0;
	};

	// 断面記号（柱＝×・小屋束＝／）の線分 1 本。両端は柱の**実断面**（幅×せいの矩形）の
	// 角そのもので、記号の大きさは柱ごとに違う（Python 版の柱束伏図記号 PIO が
	// リセット時に実断面から描いていたのと同じ絵を、解析側で計算して持たせる）。
	struct MarkSegment
	{
		Vec2 start;
		Vec2 end;
	};

	// 断面記号（柱・小屋束を切った位置に重ねる ×／／）を**直線**で描く命令。柱 1 本につき
	// 1 つで、segments は 柱＝2 本（× の対角線）・小屋束＝1 本（／）（ROADMAP.md M12）。
	//
	// ［Python 版との差異・意図的］Python 版は姉妹プロジェクトのカスタム PIO
	// 「柱束伏図記号」を span レイヤごとに 1 つ置き、PIO がリセット時に対象レイヤの
	// 構造材を検索して記号を描く。本移植は **PIO を使わず素の直線で描く**——カスタム PIO は
	// それを導入していないマシンで図面の表示が崩れるため（M12 の方針。ROADMAP.md）。
	// 代わりに「柱を編集したら記号が追随する」性質は失われるので、伏図記号のほうは
	// VW 同梱のデータタグ（ColumnPlanMarkCommand）で追随を担わせている。
	//
	//   layer     … 配置先デザインレイヤ名（柱と同じ span レイヤ "1to2-柱"）
	//   drawClass … 作図クラス（極細実線。予約語 class を機械置換）
	//   segments  … 記号を構成する線分（柱＝2 本・小屋束＝1 本）
	struct ColumnSectionMarkCommand
	{
		std::string layer;
		std::string drawClass;
		std::vector<MarkSegment> segments;
	};

	// 伏図記号（柱・小屋束の平面記号）を **VW 標準のデータタグ**として置く命令。柱 1 本に
	// つき 1 つで、専用レイヤ "{to}-柱伏図記号" に載る（ROADMAP.md M12）。
	//
	// 【なぜデータタグか】記号の目的は「その伏図が対象とする横架材の下にある柱・小屋束の
	// 位置を示す」ことで、**柱が動いたら記号も動いてほしい**。ところが VW には素の図形
	// （線・シンボル）を他の図形へ追随させる仕組みが無い（ISDK の association は読み取りと
	// 削除だけで、追加する公開 API が無い。レコードは値の入れ物であって再計算の引き金には
	// ならない）。追随するのは「リセットされるもの」＝ PIO だけなので、**VW 本体に同梱の
	// PIO であるデータタグ**を使う——カスタム PIO と違い、どのマシンでも欠落しない。
	// 関連付けは draw 側が IDataTagSupport::AssociateWithObject で行う。
	//
	// 記号の絵（柱＝"柱伏図記号" / 小屋束＝"束伏図記号" のシンボル）は**タグスタイルに
	// 焼き込む**（Python 版がシンボル名で指定していたものを、そのままスタイル名にする）。
	// スタイルはプラグインが作らずテンプレート側の用意に従う（構造材の
	// "木質構造材_横架材" と同じ作法。draw/ColumnMark.h）。
	//
	//   layer       … 配置先デザインレイヤ名（"{to}-柱伏図記号"。ストーリに属さない独立レイヤ）
	//   styleName   … データタグスタイル名（"柱伏図記号" / "束伏図記号"）
	//   drawClass   … 作図クラス（記号クラス。予約語 class を機械置換）
	//   columnIndex … 関連付け先の **Document::columns の添字**（SDK ハンドルは Document に
	//                 載せられないので、描画側が「命令インデックス → ハンドル」の対応表で
	//                 引く。壁結合の a / b と同じ受け渡し方式）
	//   position    … タグの挿入点（柱の断面中心。センタリング済みの平面座標）
	struct ColumnPlanMarkCommand
	{
		std::string layer;
		std::string styleName;
		std::string drawClass;
		std::size_t columnIndex = 0;
		Vec2 position;
	};

	// シートレイヤに載せるビューポート 1 枚。Python 版 document.py の ViewportCommand
	// （dict）に対応する。伏図は「特定のデザインレイヤ群だけを見下げた図」なので、命令が
	// 持つのは**どのレイヤを見せるか**と図面タイトル・図番だけになる（ROADMAP.md M13）。
	//
	// Python 版キーとの対応:
	//   drawingTitle  ← 'drawing_title'  … 図面タイトル（"1階床伏図" 等）
	//   drawingNumber ← 'drawing_number' … 図番（シートレイヤ番号と同じ文字列）
	//   layers        ← 'layers'         … 表示するデザインレイヤ名（**それ以外は非表示**）
	//
	// 【並びは重ね順ではない】layers の並び順は描画側の走査順にすぎず、伏図での重なりは
	// ビューポートのレイヤ重ね順が決める。床・野地板が柱・梁を覆い隠さないようにする件は
	// **描画側が core::desiredStoryLayerOrder を per-viewport の重ね順上書きへ適用**して
	// 満たす（draw/Sheet.h。命令にレイヤ順を持たせないのは、全ビューポートで同じ 1 本の
	// 希望順を使うため——命令ごとに複製すると希望順の定義が命令の数だけ増える）。
	//
	// ［Python 版との差異・意図的］Python 版は hidden_classes（クラス単位の非表示）を持つが、
	// **どの伏図も指定していない**（汎用機構として残されているだけ）。使われない枠を先に
	// 作らない方針（空レイヤを作らないのと同じ）でここには持たせず、描画側は全クラスを
	// 表示にする。クラスで絞る伏図が実際に要るときにフィールドごと足す。
	struct ViewportCommand
	{
		std::string drawingTitle;
		std::string drawingNumber;
		std::vector<std::string> layers;
	};

	// シートレイヤ 1 枚（＋その上のビューポート 1 枚）を生成する命令。Python 版
	// document.py の SheetCommand（dict）に対応する。draw/Sheet がこれをシートレイヤと
	// ビューポートへ変換する（ROADMAP.md M13）。
	//
	// Python 版キーとの対応:
	//   number   ← 'number'   … シートレイヤ番号（**レイヤ名がこれを担う**。"1" / "2" …）
	//   title    ← 'title'    … シートレイヤのタイトル（"基礎伏図" 等）
	//   viewport ← 'viewport' … そのシートに載せるビューポート 1 枚
	struct SheetCommand
	{
		std::string number;
		std::string title;
		ViewportCommand viewport;
	};

	// 命令セット本体。プレーンな構造体の集約（std::vector / std::string / double /
	// enum 等）で表す。
	//
	// TODO: 要素ごとに命令リストを追加していく（フィールド名は Python 版のキーに対応）。
	//   * M11 anchorBolts / floorPosts / fireBraces / joints …
	//   スキーマを変えるときは構造体・validateDocument・テストを同時更新する。
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

		// M7 横架材。IfcBeam / IfcMember を解析して得た MemberCommand の列（階＝Elevation
		// 昇順・階内は要素の出現順で決定的。parse/Member が組み立て、登り梁は parse/Noboribari が
		// 屋根面へスナップ補正した後の値）。配置先レイヤ（"n-横架材天端" / "R-軒高" /
		// "n-母屋" / "n-登り梁"）は stories が作るので、描画は stories の後に処理する。
		std::vector<MemberCommand> members;

		// M6 垂木。屋根版（IfcSlab "屋根版"）から導出した RafterCommand の列（階＝Elevation
		// 昇順・階内は屋根版の #id 昇順・面内は掃引位置順で決定的。parse/Rafter が組み立てる）。
		// 配置先の "n-垂木" レイヤは stories が作るので、描画は stories の後に処理する。
		std::vector<RafterCommand> rafters;

		// M6 野地板。屋根版 1 面につき 1 枚の RoofCommand の列（並びの決定性は rafters と
		// 同じ。parse/Roof が組み立てる）。配置先の "n-野地板" レイヤは stories が作る。
		std::vector<RoofCommand> roofs;

		// M8 柱。IfcColumn を解析して得た ColumnCommand の列（階＝Elevation 昇順・階内は
		// 要素の出現順で決定的。parse/Column が組み立てる）。配置先の span レイヤ
		// （"1to2-柱" 等）は stories が作るので、描画は stories の後に処理する。
		std::vector<ColumnCommand> columns;

		// M9 基礎の立上り。IfcFooting "基礎梁…" を解析して得た WallCommand の列
		// （同一直線・同一断面のものを統合し、自由端を半壁厚延長した後の値。#id 昇順に
		// 準ずる決定的な並び。parse/Footing が組み立てる）。配置先の "F-立上り" レイヤは
		// 基礎ストーリ（stories の先頭）が作るので、描画は stories の後に処理する。
		std::vector<WallCommand> walls;

		// M10 基礎の立上りどうしの壁結合。交差する立上りのジャンクション（同一交点に集まる
		// 立上りの集合）ごとに L / T / X の結合を組み立てた列（parse/Footing が walls から
		// 導く決定的な並び）。a / b は **walls の添字**なので、walls を並べ替えたり足したり
		// したらこの列も作り直す。描画は立上りの直後・底盤の前に行う（draw/ExecuteDocument）。
		std::vector<WallJoinCommand> wallJoins;

		// M9 基礎の底盤。IfcSlab / IfcFooting の "…底盤…" を解析して得た SlabCommand の列
		// （連続する同厚・同高のものを統合し、外周を立上りの外面へ合わせた後の値。
		// parse/Footing が組み立てる）。外面合わせに立上りを参照するので walls の後に作る。
		// **地中梁（M10）は独立した命令にせず**、平面で最も重なる底盤の modifiers に付く。
		std::vector<SlabCommand> slabs;

		// M11 アンカーボルト。IfcMechanicalFastener のボルト本体（座金は除く）を
		// "アンカーボルト_M12" / "…_M16" へ置換する（parse/AnchorBolt）。配置先は基礎
		// ストーリの "F-アンカーボルト"（基礎天端レベル）で、レイヤは M9 の基礎 story 命令が
		// 作る（parse/Footing の buildFoundationStoryCommand）。
		std::vector<SymbolCommand> anchorBolts;

		// M11 床束。ホームズ君 IFC に床束は出力されないので、大引の下へ 910mm 間隔で
		// 決め打ち配置する（parse/FloorPost）。配置先は基礎ストーリの "F-床束"（床束レベル＝
		// 底盤天端に揃える）。
		std::vector<SymbolCommand> floorPosts;

		// M11 火打。Name が "火打…" の IfcBeam / IfcMember を "鋼製火打" へ置換する
		// （parse/FireBrace）。配置先は横架材と同じレイヤ（"n-横架材天端" / "R-軒高"）。
		std::vector<SymbolCommand> fireBraces;

		// M11 仕口。受ける材（別の横架材・柱）のある横架材端部へ "仕口" を置く
		// （parse/Joint）。members / columns から導出するので、その 2 つより後に組み立てる。
		// 配置先は受ける側ではなく**その横架材自身のレイヤ**。
		std::vector<SymbolCommand> joints;

		// M12 断面記号。柱 1 本につき 1 つで、その柱と同じ span レイヤへ ×（柱）／／
		// （小屋束）を直線で重ねる（parse/ColumnMark）。並びは columns と同じ順。
		std::vector<ColumnSectionMarkCommand> columnSectionMarks;

		// M12 伏図記号。柱 1 本につき 1 つで、"{to}-柱伏図記号" レイヤへデータタグを置き、
		// その柱へ関連付ける（parse/ColumnMark）。columnIndex は **columns の添字**なので、
		// columns を並べ替えたり足したりしたらこの列も作り直す。並びは columns と同じ順。
		std::vector<ColumnPlanMarkCommand> columnPlanMarks;

		// M13 シート（伏図）。基礎伏図 → 各階の柱梁伏図 → 屋根版を持つ階ごとの母屋伏図の
		// 順で、シートレイヤ番号もその順に "1" から振る（parse/Sheet が組み立てる）。
		// ビューポートが見せるデザインレイヤはすべて stories が作るので、描画は
		// **全要素の描画が済んだ後**に処理する（draw/ExecuteDocument）。
		std::vector<SheetCommand> sheets;
	};

	// Document を描画前に検証する（Python 版 validateDocument 相当）。draw/ は
	// 検証を通った Document だけを SDK API へ渡す。現状はバージョンと stories / floors /
	// rafters / roofs / grids / シンボル 4 種を見る（規則は Document.cpp の各 isValid* 参照。
	// 空の Document は妥当）。
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
