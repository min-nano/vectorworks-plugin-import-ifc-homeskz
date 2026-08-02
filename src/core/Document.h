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
//	現状は「バージョン＋ stories / grids / floors / rafters / roofs」を持つ（M1 通り芯・
//	M3 ストーリ・M5 床板・M6 屋根組ぶん）。残りの命令リスト（members / columns / walls /
//	slabs …）は、対応するマイルストーンで要素を移植するたびに 1 つずつ追加する（ROADMAP.md）。
//

#pragma once

#include "core/Geometry.h"

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
	// 上端は elevation + height。加えて上下端をストーリレベルへバインドする（bottomBound /
	// topBound）。柱（管柱・通し柱）は下端を当階・上端を上階（storyOffset=1）の横架材天端
	// （最上階は軒高）へ、小屋束は上下端とも当階の横架材天端へバインドする。**小屋束の
	// 上端 offset は下端と同値**にする（VW の構造材ツールは上下端 offset 差を部材長へ
	// 加算するため、天端相当の値を入れると柱高さが二重加算される。parse/Column.h 参照）。
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

	// 命令セット本体。プレーンな構造体の集約（std::vector / std::string / double /
	// enum 等）で表す。
	//
	// TODO: 要素ごとに命令リストを追加していく（フィールド名は Python 版のキーに対応）。
	//   * M9 walls / slabs …
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
	};

	// Document を描画前に検証する（Python 版 validateDocument 相当）。draw/ は
	// 検証を通った Document だけを SDK API へ渡す。現状はバージョンと stories / floors /
	// rafters / roofs / grids を見る（規則は Document.cpp の各 isValid* 参照。空の Document は妥当）。
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
