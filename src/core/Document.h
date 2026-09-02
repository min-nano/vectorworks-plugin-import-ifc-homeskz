//
//	core/Document.h
//
//	命令セット（Document）の構造体定義。IFC 解析フェーズ（parse/）と VW 描画
//	フェーズ（draw/）を結ぶ唯一の境界であり、両フェーズはこの構造体だけで接続する。
//	SDK ハンドルや STEP エンティティポインタ等「フェーズ間で運べないもの」は
//	絶対にここへ載せない（CLAUDE.md「アーキテクチャ: 2 フェーズ分離」）。
//
//	フィールド名は図面側の語彙（レイヤ・クラス・レベル・構成層…）に合わせる。C++
//	の予約語と衝突するもの（class 等）は drawClass / className のように置き換える。
//
//	現状は「バージョン＋ stories / grids / floors / members / columns / rafters / roofs /
//	foundation / シンボル 4 種 / columnMarks / sheets / sections / sectionSheet」を持つ。
//	基礎（立上り・底盤・地中梁・床付け）は M21 で **1 つの命令（FoundationCommand。
//	core/Foundation.h）**にまとめ、自作 PIO 1 つで描く（それまでの walls / wallJoins /
//	slabs は無くなった。docs/DEV-NOTES.md M21）。
//

#pragma once

#include "core/Foundation.h"
#include "core/Geometry.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 命令セットのスキーマバージョン。解析側と描画側でスキーマの世代が食い違ったこと
	// （dev/stable 混在や部分的な再ビルド）を検出できるように持たせておく。
	inline constexpr int kDocumentVersion = 1;

	// 通り芯を置くデザインレイヤ名。GridCommand の既定値と desiredStoryLayerOrder の最上段が同
	// じ名前を指す必要があるので、ここに 1 つだけ置く。
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
	// M9/M11/M21 基礎ストーリのレベル。GL は基礎ストーリの原点（常に 0）で基礎の PIO
	// （"F-基礎"。部品の Z は GL 基準の絶対値なので高さ 0 のレイヤに置く）を載せる。基礎天端は
	// 立上りの天端でアンカーボルト（"F-アンカーボルト"）を、床束は底盤天端に揃えて床束
	// （"F-床束"）を載せる（M11。シンボルは高さを持たず、この 2 つのレベルが Z を決める）。
	// スタック順は 基礎天端 → GL → 床束（parse/Footing の buildFoundationStoryCommand。
	// M21 で底盤のレベル "底盤天端" は無くなった——底盤は基礎の PIO の中にある）。
	inline constexpr const char* kLevelGL = "GL";
	inline constexpr const char* kLevelFoundationTop = "基礎天端";
	inline constexpr const char* kLevelFloorPost = "床束";

	// M19 耐力壁のレベル。筋かい・面材の PIO を載せる "n-耐力壁" レイヤの種別名で、レイヤ平面は
	// **その階の横架材天端**（最上階は軒高）に合わせる（parse/Story）。耐力壁は土台天端から
	// 梁下端までの内法に収まるので、この平面を基準にすれば命令が持つ高さ（bottomHeight /
	// topHeight）がそのまま内法になる（ShearWallCommand 参照）。
	inline constexpr const char* kLevelShearWall = "耐力壁";

	// 構造用途（構造材ツールのポップアップのキー）。**命令セットの語彙なのでここが唯一の
	// 定義**で、ColumnCommand::structuralUse に入る値と、要素ごとに固定の用途——横架材
	// （draw/Member）・垂木（draw/Rafter）——がこれになる。parse/Column.h は読みやすい名前で
	// 再公開するだけ（レベル種別名と同じ扱い）。
	//
	// core が持つ理由: 記号 PIO（Extensions/ExtColumnMark）は、対象レイヤの構造材を
	// **この値で**柱／小屋束に見分ける。PIO は SDK 側のコードなので parse/ を include
	// できず（依存の向き）、かつては "4" / "5" を自前で書き写していた——parse 側で値を
	// 変えると記号が黙って何も描かなくなる形だった。両フェーズが見てよい唯一の置き場が
	// ここ（CLAUDE.md「両者をつなぐのは core/Document.h だけ」）。
	//
	// 値は OIP のポップアップの**並び順**（`<自動>` を 0 とする 0 始まりの索引）。実機の
	// ドロップダウンを開いて確かめた全 17 項目は SDK リファレンス Findings
	// 「Parametric Objects」の表にある（ここには本プラグインが使うものだけを置く）。
	inline constexpr const char* kStructuralUseBeam = "1"; // 梁（土台・梁・桁・母屋…）
	inline constexpr const char* kStructuralUseColumn = "4";   // 柱（管柱・通し柱）
	inline constexpr const char* kStructuralUseKoyazuka = "5"; // 小屋束
	inline constexpr const char* kStructuralUseRafter = "8";   // 垂木

	// 通り芯（グリッド）1 本の描画命令。draw/Grid がこれを GridAxis オブジェクトへ変換する
	// （docs/DEV-NOTES.md M1）。
	//
	// フィールド:
	//   label                … 軸名（IfcGridAxis の AxisTag。例 "X1" / "Y1"）
	//   layer                … 配置デザインレイヤ名（通り芯は常に "共通"）
	//   drawClass            … クラス名（X 通り / Y 通り。予約語 class を機械置換）
	//   start                … 始点（bbox 中心でセンタリング済みの平面座標）
	//   end                  … 終点（同上）
	// start/end は 3D ではなく平面（Vec2）: 通り芯は配置行列・断面・ストーリを要さず、
	// ポリラインの端点だけで決まる（M1 が最初の縦切りである理由。docs/DEV-NOTES.md M1）。
	struct GridCommand
	{
		std::string label;
		std::string layer = kGridLayer;
		std::string drawClass;
		Vec2 start;
		Vec2 end;
	};

	// story 命令内の 1 ストーリレベル。ストーリレベルとそれに紐づくデザインレイヤ 1 枚を表す。
	//
	// フィールド:
	//   type              … レベル種別（"FL" / "横架材天端" / "軒高"。CreateLayerLevelType
	//                        へ登録し、GetLayerForStory でレイヤを取り直す鍵になる）
	//   offset            … ストーリ原点（FL／軒高）からの相対高さ（mm。負値=下）
	//   layer             … このレベルに紐づくデザインレイヤの意図した名前（"1-FL" 等）
	struct LevelCommand
	{
		std::string type;
		double offset = 0.0;
		std::string layer;
	};

	// ストーリ・ストーリレベル・デザインレイヤを生成する命令。draw/Story がこれを CreateStory
	// ＋レベルテンプレートによるレイヤ生成へ変換する（docs/DEV-NOTES.md M3）。
	//
	// フィールド:
	//   name                    … VectorWorks のストーリ名（"1階" / "2階" / "屋根"）
	//   suffix                  … CreateStory の接尾辞（"1" / "2" / "R"。空文字は 2 回目
	//                             以降の CreateStory が失敗するため非空必須）
	//   elevation               … ストーリ高さ（IfcBuildingStorey.Elevation。mm）
	//   levels                  … 生成するストーリレベルの列。並び順は**希望する
	//                             デザインレイヤのスタック順（上→下）**を表し、draw/Story が
	//                             その順にレイヤを並べ替える（レベルの高さには依存しない）。
	struct StoryCommand
	{
		std::string name;
		std::string suffix;
		double elevation = 0.0;
		std::vector<LevelCommand> levels;
	};

	// 高さ基準（ストーリレベルへのバインド）1 端分。床・構造材・柱・壁・スラブが共通で使う。
	//
	// フィールド:
	//   storyOffset                  … 配置先レイヤのストーリからの相対階数（0=自階・1=上階）
	//   level                        … そのストーリのレベル種別名（"横架材天端" / "軒高"）
	//   offset                       … レベルからの距離（mm。負値=下）
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
	//   スラブ … 床は「床仕上げ」「床下地」、基礎の底盤は「コンクリート」「砕石」
	//   壁     … 基礎の立上りは「コンクリート」1 層
	//
	//   name       … 層の名前（"床仕上げ" / "コンクリート" …）
	//   drawClass  … 層に割り当てる VW クラス名（"z構成要素-フローリング" 等。
	//                 parse/StructuralClass.h の CLASS_COMPONENT_*）
	//   thickness  … 層厚（mm。0 以上）
	// 並び順は**上から下**（スラブ）／**外から内**（壁）で、先頭が最初の層。層厚の合計が
	// そのオブジェクトの総厚（スラブ厚・壁厚）になる。**構成はスタイルではなく各オブジェクト
	// へ直接**与える（draw/DrawUtil.h「複合オブジェクトの構成」）。
	//
	// 【構成要素のクラス】層は素材ごとにクラスへ分けて、**描画属性（塗り・ペン）はすべて
	// そのクラスの属性に従わせる**（draw/DrawUtil の SetComponents）。オブジェクト本体の
	// クラス（drawClass = 04構造-… の構造クラス）は「その部材が何か」を表し、構成要素の
	// クラスは「その層が何でできているか」を表す——断面の見え方（ハッチング・線）を素材で
	// 揃えたいので、両者は別の軸として持つ。
	struct ComponentCommand
	{
		std::string name;
		std::string drawClass;
		double thickness = 0.0;
	};

	// 構成層の総厚（スラブ厚・壁厚）。**この計算はここに 1 つだけ置く**——検証
	// （isValid* の「総厚 > 0」）・断面の高さ範囲（床下端）・基礎の床付け深さ
	// （parse/Footing）が同じ合計を要り、かつて各所が同じループを持っていた。
	inline double totalThickness(const std::vector<ComponentCommand>& components)
	{
		double total = 0.0;
		for (const ComponentCommand& component : components)
			total += component.thickness;
		return total;
	}

	// 床板（IfcSlab "床版"）を描く命令。draw/Floor がこれをスラブオブジェクトへ変換する（床は
	// 床ツールではなく**スラブ**で描く。BIM オブジェクトとして構成層・スタイル・データ連携を
	// 持つため。draw/Floor.h 参照）。
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
	// 構成は**そのスラブへ直接**与える（スタイルは作らない・当てない。draw/DrawUtil.h
	// 「複合オブジェクトの構成」）。階により構成（床仕上げ厚）が異なるので、階ごとに
	// 命令が持つ層がそのまま床の構成になる。
	//
	// フィールド:
	//   layer                    … 配置先デザインレイヤ名（"1-FL" 等。既存のみ・無ければスキップ）
	//   drawClass                … クラス名（床板。予約語 class を機械置換）
	//   boundary                 … 床の平面外形（mm・グリッド中心オフセット済み。閉じた
	//                              ポリゴンの頂点列で、末尾に始点を重複させない）
	//   components                … スラブの構成層（上から）
	//   datum                     … 高さ基準の面（一般階＝Top・ロフト＝Bottom）
	//   elevation                … **基準面**の絶対 Z（mm）
	//   bound                    … 基準面の高さ基準（一般階＝FL レベル、ロフト＝軒高レベル、
	//                              ＋段差 offset）
	struct FloorCommand
	{
		std::string layer;
		std::string drawClass;
		std::vector<Vec2> boundary;
		std::vector<ComponentCommand> components;
		SlabDatum datum = SlabDatum::Top;
		double elevation = 0.0;
		StoryBoundCommand bound;
	};

	// 垂木（構造材ツール StructuralMember、構造用途＝垂木）を描く命令。draw/Rafter がこれを
	// 構造材オブジェクトへ変換する（docs/DEV-NOTES.md M6・M15）。垂木はホームズ君 IFC に一切
	// 出力されないため、**屋根版（IfcSlab の屋根面）の勾配・外形から導出**した結果がこの命令に
	// なる（parse/Rafter.h 参照）。
	//
	// 【高さと両端の持ち方】start は**軒側＝支持点**（屋根面が横架材天端／軒高の Z レベルと
	// 交わる点＝受ける軒桁の芯線の真上）、end は**棟側**（高い端）。elevation / endElevation
	// はそれぞれ**下面**の絶対 Z（屋根面＝垂木下面がその点を通る高さ。断面基準点が中下＝
	// 下面中央なので、この Z がそのまま部材の基準になる）。
	//
	// 【実際の材端は軒先で、start ではない】start は支持点なので、軒側へは差し込み
	// （embedment）＋軒の出（overhang）だけ材が伸びている。**描画のパスはその軒先から棟まで**で、
	// 軒先の位置・高さ・バインド offset は rafterEaveEnd が返す（軸組ツール時代は overhang /
	// bearinginset パラメータが持っていた伸び。構造材ツールにその口は無いのでパスで表す）。
	//
	// 【傾斜はバインドの offset 差だけで表す】描画側は**パスに Z 成分を持たせない**
	// （両端とも軒先の Z）。構造材ツールの高さバインドはパス由来の部材長へ加算されるため、
	// パスにも傾斜を持たせると二重に適用される（横架材の登り梁と同じ。draw/Member.cpp 冒頭）。
	//
	// フィールド:
	//   layer                            … 配置先デザインレイヤ名（"n-垂木"）
	//   drawClass                        … クラス名（小屋組-垂木。予約語 class を機械置換）
	//   width                            … 断面幅（既定 45mm）
	//   height                           … 断面せい（既定 45mm）
	//   start                            … 軒側＝支持点の平面座標（センタリング済み）
	//   end                              … 棟側の平面座標（同上）
	//   elevation                        … 支持点の下面 Z（絶対値。屋根面が横架材天端／軒高と
	//                                      交わる高さ）
	//   endElevation                     … 棟側の下面 Z（絶対値）
	//   overhang                         … 軒の出（壁外面から軒先までの水平距離）
	//   embedment                        … 支持部分の差し込み（受ける軒桁の桁幅の半分）
	//   label                            … 仕様ラベル（"45×45@455"）。構造材 ID に入れる
	//   startBound                       … 支持点の高さ基準（垂木レベル＋下面 Z との差）
	//   endBound                         … 棟側の高さ基準（同上）
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
		StoryBoundCommand startBound;
		StoryBoundCommand endBound;
	};

	// 垂木の**軒先側の材端**（＝実際に描かれる下端。支持点 start より軒側へ 差し込み ＋
	// 軒の出 だけ出た点）。draw/Rafter がパスの始端とその高さ基準に使う。
	//   point  … 平面座標（センタリング済み。start と同じ座標系）
	//   z      … 下面の絶対 Z（勾配ぶん start より下がる）
	//   offset … startBound と同じレベルから見た offset（＝ startBound.offset − 下がったぶん）
	struct RafterEaveEnd
	{
		Vec2 point;
		double z = 0.0;
		double offset = 0.0;
	};

	// 垂木の軒先側の材端を求める。支持点 start から end と**逆向き**へ（overhang ＋
	// embedment）だけ水平に伸ばし、その水平距離ぶん勾配（両端の下面 Z の差 ÷ 水平投影長）を
	// 下げた点を返す。伸びが 0（軒桁に乗らない垂木）なら start そのものを返す。
	//
	// **core に置く理由**: SDK を触らない純計算で、無 SDK テストで検証できるため
	// （desiredStoryLayerOrder・core/Foundation の foundationSolids と同じ立ち位置。CLAUDE.md「テスト方針」:
	// draw から切り離せるロジックは core へ寄せる）。
	RafterEaveEnd rafterEaveEnd(const RafterCommand& rafter);

	// 野地板（屋根の下地合板）を屋根面オブジェクト（Roof Face）として描く命令。draw/Roof がこ
	// れを屋根面オブジェクトへ変換する（ISDK に屋根作成の一連の呼び出しが無いため、VWFC の
	// VWRoofFaceObj を組み立てる。draw/Roof.cpp 冒頭参照）。屋根版（IfcSlab の屋根面）
	// **1 面につき 1 枚**で、垂木のように間隔で割らない。
	//
	// 【高さの持ち方】elevation は**軒（屋根軸）の天端 Z の絶対値**。野地板は垂木の上に
	// 載る（野地板下端＝垂木上端）ため、屋根版の平面（＝垂木下面）から垂木せいを鉛直換算して
	// 持ち上げた値になる（parse/Roof.h 参照）。屋根オブジェクトはレイヤ基準（レイヤ相対）の
	// 座標系を持つため、描画フェーズが「絶対 Z − レイヤ Z」へ読み替える。
	//
	// フィールド:
	//   layer                     … 配置先デザインレイヤ名（"n-野地板"）
	//   drawClass                 … クラス名（耐力面材-屋根。予約語 class を機械置換）
	//   boundary                  … 屋根の水平投影外形（閉じたポリゴンの頂点列。末尾に
	//                               始点を重複させない。センタリング済み）
	//   axisStart                 … 軒（最も低い辺）に沿う屋根軸の始点
	//   axisEnd                   … 同 終点（軸の向きが軒の向き）
	//   upslope                   … 棟（高い）側を指す upslope 定義点
	//   rise                      … 勾配の rise（屋根面の単位法線の水平成分 dh）
	//   run                       … 勾配の run（同 鉛直成分 nz。slope = rise/run = tanθ）
	//   thickness                 … 野地板厚（12mm 固定）
	//   elevation                 … 軒（屋根軸）の天端 Z（絶対値）
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

	// 【端部オフセット（endpoint offset）】構造材ツールは、パス（芯線）の端点から材の端面までの
	// 距離を端ごとに持つ（OIP の「始端オフセット」「終端オフセット」）。**符号つきで、負値が
	// 材を短く・正値が長くする。** 本プラグインはこれを使って「パスの端点＝接合相手の芯線上の
	// 点（＝節点）」「実際の材の端＝ IFC が描いていた位置」という持ち方をする。
	//
	// なぜ端点を芯線の交点に置くか: 横架材の芯線は**天端中央**（断面基準点＝AxisAlign）を通る
	// ので、柱が受ける梁の芯線は梁の天端にある。柱の上端を梁の下端（＝材が実際に止まる高さ）に
	// 置くと、その座標はどの部材の芯線にも乗らず、**座標だけを見て「どことどこが接合している
	// か」を言えない**。端点を芯線の交点へ置き、実際の材の範囲は端部オフセットで戻せば、
	// 命令セットがそのままフレーム（節点と部材）のモデルになる——将来フレーム解析モデルを
	// 取り込んで組み立てるときの土台になる。
	//
	// 値の決まり方:
	//   柱・束の上端      … −（受ける横架材のせい）。上に梁せい 150mm が乗るなら −150。
	//   横架材の負け側端  … −（勝ち側の半幅 ÷ 交差角の余弦）。直交する 105mm の梁なら −52.5。
	//   横架材の柱に付く端 … −（端点から柱芯までの軸方向の距離）。柱の手前の面で止まっていた
	//                       端は負値、**柱の向こうの面まで来ていた端は正値**（外周の桁が
	//                       隅柱の外面まで伸びている取り合い。実データではこちらが多い）。
	// 実際に描かれる材の長さは「パス長 ＋ 始端オフセット ＋ 終端オフセット」で、この値は
	// 端部オフセットを入れる前（＝ IFC の形）と一致する——**描かれる形は変えず、命令が持つ
	// 端点の意味だけを変える**のがこの仕組みの要点。
	//
	// 横架材（構造材ツール StructuralMember）を描く命令。draw/Member がこれを構造材
	// オブジェクトへ変換する（docs/DEV-NOTES.md M7）。土台・梁・桁のほか、母屋・棟木・
	// 登り梁もこの命令で表し、配置先レイヤと高さ基準レベルだけが専用のもの（"n-母屋" /
	// "n-登り梁"）になる。
	//
	// 【高さと両端の持ち方】start / end は**天端中央線**（断面基準点＝左右中央・上端が
	// 通る線）の平面座標で、elevation / endElevation がそれぞれの天端 Z（絶対値）。
	// ホームズ君 IFC の配置点は断面中心なので、解析側が背/2 だけ持ち上げてここへ入れる
	// （parse/Member.h「基準点補正」）。両者が異なれば傾斜梁（登り梁・隅木等）。
	//
	// 【傾斜はバインドの offset 差だけで表す】描画側は**パスに Z 成分を持たせない**。
	// 構造材ツールの高さバインドはパス由来の部材長へ加算されるため、パスにも傾斜を持たせると
	// 二重に適用される（draw/Member.cpp 冒頭）。
	//
	// フィールド:
	//   layer                           … 配置先デザインレイヤ名（"1-横架材天端" / "R-軒高" /
	//                                     "n-母屋" / "n-登り梁"）
	//   memberId                        … 構造材 ID（"120×180 - 杉…"。材種が無ければ "120×180"）
	//   drawClass                       … クラス名（土台／床梁／軒桁／母屋…。予約語 class を機械置換）
	//   start                           … 天端中央線の始端（センタリング済みの平面座標）
	//   end                             … 同 終端
	//   width                           … 断面幅（mm）
	//   height                          … 断面せい（背。mm）
	//   elevation                       … 始端の天端 Z（絶対値）
	//   endElevation                    … 終端の天端 Z（絶対値）
	//   startBound                      … 始端の高さ基準（配置先レベル＋天端との差）
	//   endBound                        … 終端の高さ基準（同上）
	//   startOffset                     … 始端の端部オフセット（mm。負＝短く・正＝長く。上記）
	//   endOffset                       … 終端の端部オフセット（同上）
	//
	// 【start / end は「芯線の交点」】勝ち側の横架材へ突き当たる端（負け側）は、相手の面では
	// なく**相手の天端中央線（＝芯線）上の点**に置き、面までの戻りを startOffset / endOffset
	// に入れる（上記「端部オフセット」。parse/Member の resolveMemberInterferences）。
	// **柱に取り付く端**も同じで、端点は柱芯の軸位置に置く（parse/Member の
	// resolveMemberColumnJoints）。取り付く相手のいない端はオフセット 0 で、端点がそのまま
	// 材の端になる。
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
		double startOffset = 0.0;
		double endOffset = 0.0;
	};

	// 横架材の実体が占める Z 範囲。elevation / endElevation は**天端** Z で傾斜梁は両端で
	// 異なるため、上端は両者の大きい方・下端は小さい方から背を引いた値になる。仕口の
	// 取り付き判定（parse/Joint）・登り梁の受け材判定（parse/Noboribari）・柱下端の算出
	// （parse/Column）が同じ式を要り、かつて各所が同じ 2 行を持っていた。
	inline double memberTopZ(const MemberCommand& member)
	{
		return std::max(member.elevation, member.endElevation);
	}

	inline double memberBottomZ(const MemberCommand& member)
	{
		return std::min(member.elevation, member.endElevation) - member.height;
	}

	// 2 つの Z 範囲 [aBottom, aTop] と [bBottom, bTop] が tol を超えて重なるか。tol は
	// 「重なりと呼ぶ最小量」で、要素ごとに意味が違う（仕口の取り付き・登り梁の受け・
	// 横架材の食い込み）ので**呼び出し側の定数のまま**渡す——ここへ統合しないこと。
	inline bool zRangesOverlap(double aBottom, double aTop, double bBottom, double bTop, double tol)
	{
		return std::min(aTop, bTop) - std::max(aBottom, bBottom) > tol;
	}

	// 柱（構造材ツール StructuralMember）を鉛直材として描く命令。draw/Column がこれを構造材
	// オブジェクトへ変換する（docs/DEV-NOTES.md M8）。管柱・通し柱・小屋束をこの命令で表し、
	// 種別の違いは drawClass と structuralUse（構造用途）に出る。
	//
	// 【配置先レイヤは span（またぐレベル区間）】柱は階のレイヤではなく "{from}to{to}-柱"
	// という**span 専用レイヤ**に置く（"1to2-柱" / "2to2.5-柱"）。from は柱が立つ床レベル
	// （1 始まり）、to は上端が届く床／屋根面レベル（管柱＝次階の整数・屋根束＝屋根面で
	// 止まる半整数・通し柱＝複数階ぶん上）。伏図が切断レベルで表示レイヤを絞れるようにする
	// ための分け方で、下屋の小屋束が上階の小屋伏図へ写り込まない（parse/Column.h 参照）。
	//
	// 【高さの持ち方】elevation は**柱下端**の絶対 Z、height は**パス長**（下端 → 上端）で、
	// 上端は elevation + height。上端は受ける横架材の天端に取るので（下記）、実際に描かれる
	// 材の高さは height + endOffset（＝ IFC の押し出し Depth）になる。描画側はこの 2 つから鉛直パスを作り、加えて上下端を
	// ストーリレベルへバインドする（bottomBound / topBound）——**どちらが欠けても正しい
	// 高さで描かれない**（parse/Column.h 参照）。柱（管柱・通し柱）は下端を当階・上端を
	// 上階（storyOffset=1）の横架材天端（最上階は軒高）へ、小屋束は上下端とも当階の
	// 横架材天端へバインドし、offset にはそれぞれ実際の下端／上端の絶対 Z までの距離を
	// 入れる（＝**バウンドの差は常に柱高さ**になる）。
	//
	// フィールド:
	//   layer                              … 配置先デザインレイヤ名（"1to2-柱" 等）
	//   memberId                           … 構造材 ID（"105×105 - 管柱 / 柱頭金物:(ろ)"）
	//   drawClass                          … クラス名（通し柱／管柱／小屋束。予約語 class を機械置換）
	//   structuralUse                      … 構造用途（柱="4" / 小屋束="5"）
	//   position                           … 断面中心の平面座標（センタリング済み）
	//   width                              … 断面幅（mm）
	//   depth                              … 断面せい（mm）
	//   height                             … 柱高さ（mm）
	//   elevation                          … 柱下端の絶対 Z（mm）
	//   topHardware                        … 柱頭金物の仕様（無ければ空文字）
	//   bottomHardware                     … 柱脚金物の仕様（同上）
	//   bottomBound                        … 下端の高さ基準
	//   topBound                           … 上端の高さ基準
	//   startOffset                        … 下端の端部オフセット（mm。負＝短く・正＝長く）
	//   endOffset                          … 上端の端部オフセット（同上）
	//
	// 【上端は受ける横架材の天端＝その芯線】柱・束の上端は、材が実際に止まる高さ（梁の下端）
	// ではなく**受ける横架材の天端**——横架材の芯線が通る高さ——に置き、梁せいぶんの戻りを
	// endOffset に入れる（上記「端部オフセット」。上に 150mm せいの梁が乗るなら −150）。
	// elevation + height は**この上端**を指し、topBound.offset もこの高さとの差を表すので、
	// 「バウンドの差＝パス長」という関係（M8）はそのまま保たれる。下端は元から受け材の天端
	// （＝土台・梁の芯線）に乗るので startOffset は 0。受ける材を特定できない柱は上端も
	// 動かさず、両オフセットとも 0 になる。
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
		double startOffset = 0.0;
		double endOffset = 0.0;
	};

	// 基礎（立上り・底盤・地中梁・床付け）は 1 つの命令 FoundationCommand で表す
	// （core/Foundation.h）。M9〜M17 の壁・スラブ・モディファイアの命令は M21 で無くなった。

	// **端部オフセットを戻した「実際に材が占める範囲」**（core/Document.h 冒頭「端部
	// オフセット」）。命令の端点は接合相手の芯線上にあるので、材そのものの端・上端が要る
	// ところ（仕口シンボルを置く位置・登り梁が受け材の面まで詰める判定・図に映るものの
	// 広がり）はこれを通す。
	//
	// **core に置く理由**: SDK を触らない純計算で、解析側（parse/Joint・parse/Noboribari）と
	// 描画側から切り離せる計算（planContentBounds・sectionHeightRange）の**両方**が同じ
	// 定義を要る（rafterEaveEnd と同じ立ち位置。CLAUDE.md「テスト方針」）。
	Vec2 memberDrawnStart(const MemberCommand& member);
	Vec2 memberDrawnEnd(const MemberCommand& member);

	// 柱・束が実際に占める下端／上端の絶対 Z。下端は elevation − startOffset、上端は
	// elevation + height + endOffset（オフセットは負で短く・正で長くする）。
	double columnDrawnBottom(const ColumnCommand& column);
	double columnDrawnTop(const ColumnCommand& column);

	// ハイブリッドシンボルを平面座標＋回転角で置く命令。アンカーボルト・床束・火打・仕口の
	// 4 種（docs/DEV-NOTES.md M11「シンボル置換系」）が共通で使う。draw/Symbol がこれをシンボル
	// オブジェクトへ変換する。
	//
	// 【4 要素を 1 つの構造体で受ける】アンカーボルト・床束・火打・仕口は、命令の中身が
	// (layer,symbol, position ＋ 火打・仕口だけ angle) で同型であり、描画も「配置先レイヤが在
	// るか確かめてシンボルを置く」だけで違いが無い。そこで構造体 1 つ・描画 1 つ（draw/Symbol）
	// にまとめる（CLAUDE.md「重複を作らない置き場所」）。要素の区別は **Document
	// のどのリストに入っているか**が担い、進捗の見出しと完了ダイアログの件数は要素ごとに出る。
	// 角度を持たない命令（アンカーボルト・床束）は angle = 0 ＝シンボルの基準姿勢。
	// フィールド:
	//   layer                 … 配置先デザインレイヤ名（既存のみ・無ければスキップ）
	//   symbol                … 置換するハイブリッドシンボル名（"アンカーボルト_M12" 等）
	//   position              … シンボルの基準点（センタリング済みの平面座標）
	//   angle                 … 回転角（度・反時計回り。持たない命令は 0）
	//   zOffset               … 配置先レイヤ平面からの相対 Z（mm。下記）
	// 【高さは「レイヤ平面からの差」だけを持つ】シンボルの Z は既定で配置先レイヤの
	// ストーリレベル（基礎天端／底盤天端／横架材天端…）が決める。そこでちょうど合う要素
	// （アンカーボルト・床束・火打）は zOffset = 0 のままでよい。
	// **レイヤ平面と実際の取り付き高さがずれる要素だけ** zOffset に差を入れる——仕口は
	// 横架材の天端（傾斜梁・段差梁・母屋／棟木では 1 本ごとに違う）に合わせる必要があり、
	// レイヤ平面に置いたままでは登り梁の仕口が軒高に落ちてしまう（parse/Joint.h）。
	//
	// **絶対 Z ではなく相対 Z で持つ理由**: シンボルはストーリバウンド（SetObjectStoryBound）を
	// 持てないので、描画側は「置いてから 3D で動かす」しかない。動かす量は相対値であり、
	// レイヤ平面の絶対 Z を描画側が引き直す必要がない形（＝解析側が持てる形）にしてある。
	// 解析側では横架材のバウンド offset（レベルの絶対 Z から天端 Z までの距離）が
	// そのままこの値になる。
	struct SymbolCommand
	{
		std::string layer;
		std::string symbol;
		Vec2 position;
		double angle = 0.0;
		double zOffset = 0.0;
	};

	// 記号の描き方（PIO の MarkStyle パラメータに対応する）。
	enum class ColumnMarkStyle
	{
		Section, // 断面記号: 柱の実断面に合わせた対角線（柱＝×・小屋束＝／）
		Plan, // 伏図記号: 各柱の位置にシンボルを 1 つ
	};

	// 柱・小屋束の記号（断面記号・伏図記号）を **PIO 1 つ**で描く命令（docs/DEV-NOTES.md M12）。
	//
	// 【柱 1 本ごとではなく span レイヤごとに 1 つ】記号は PIO がリセット時に
	// **対象レイヤ（targetLayer）の構造材を検索して描く**ので、命令は「どのレイヤの
	// 何を、どこへ、どう描くか」だけを持つ。柱が動いた・断面が変わった・増減したときは、
	// PIO のリセットで記号がまとめて描き直される——記号の位置・大きさ・本数はモデル側の
	// 実物から毎回導かれるので、**古い記号が間違った内容のまま残ることがない**。
	// （素のジオメトリやデータタグでは、位置は追えても実断面の変化に追随できず、
	// 「間違った記号が残る」＝図面としては記号が無いより悪い状態になる。）
	//
	// 【PIO はこのプラグインが提供する】Extensions/ExtColumnMark が本体で、モジュールの
	// 拡張としてメニューコマンドと一緒に登録される（別プラグインにしない）。VW は PIO が
	// **描いたジオメトリを図面に保存する**ので、プラグインを入れていない環境でも
	// 図面はそのまま表示できる（更新だけができない。実機で確認済み）。
	//
	// フィールド:
	//   layer                        … PIO を置くデザインレイヤ名（断面記号＝span レイヤ
	//                                  自身／伏図記号＝"{to}-柱伏図記号"）
	//   drawClass                    … PIO 本体の作図クラス（予約語 class を機械置換）
	//   targetLayer                  … 検索対象のデザインレイヤ名（＝span 柱レイヤ）
	//   targetClass                  … 検索対象クラス（**空＝全クラス**）
	//   style                        … 記号の描き方（断面／平面）
	//   symbol                       … 伏図記号のシンボル名（"柱伏図記号" / "束伏図記号"。
	//                                  断面記号では空）
	//   position                     … PIO の挿入点。**原点でよい**——記号は検索した柱の
	//                                  ワールド位置に描かれ、挿入点には依存しない
	//
	// 【記号サイズは持たない】PIO は断面記号を**柱の実断面から**描き、伏図記号はシンボルをそ
	// のまま置くので、寸法を外から与える経路が無い（使われない枠を先に作らない）。
	// 実断面から描くのは、柱の断面が変わったときに記号が追随するようにするため——固定サイズの
	// 記号は、間違った断面の記号が残るぶん記号が無いより悪い。
	struct ColumnMarkCommand
	{
		std::string layer;
		std::string drawClass;
		std::string targetLayer;
		std::string targetClass;
		ColumnMarkStyle style = ColumnMarkStyle::Section;
		std::string symbol;
		Vec2 position;
	};

	// 耐力壁の種別（PIO の WallKind パラメータに対応する）。
	enum class ShearWallKind
	{
		Brace, // 筋かい（IfcMember "筋かい…"）
		Panel, // 面材（IfcWall "面材…"）
	};

	// 筋かいの掛け方（PIO の BraceStyle パラメータに対応する）。
	enum class ShearWallBraceStyle
	{
		Single, // 片掛け（1 本）
		Double, // たすき掛け（2 本。ホームズ君 IFC では "筋かいダブル" が同名 2 要素で出る）
	};

	// 面材を設ける面（PIO の PanelSide パラメータに対応する）。**表／裏は「軸（start→end）を
	// 見て左手側が表」という幾何の約束**で、外部／内部といった実世界の意味は持たない
	// （耐力壁の向きは IFC からは決まらない）。両面に設けた面材をハッチングの向きで
	// 描き分けるために、どちら側かを決定的に言えればよい（parse/ShearWall.h「表と裏」）。
	enum class ShearWallPanelSide
	{
		Front, // 表（軸の左手側）
		Back,  // 裏（軸の右手側）
		Both,  // 両面
	};

	// 耐力壁（筋かい・面材）を **PIO 1 つ**で描く命令（docs/DEV-NOTES.md M19）。draw/ShearWall
	// がこれを線分 PIO（Extensions/ExtShearWall）へ変換する。
	//
	// 【なぜ PIO か】耐力壁は**両端の柱に紐付いて伸縮する**必要がある（柱を動かしたら追随
	// する）。素のジオメトリでは柱が動いた瞬間に嘘になり、記号（柱記号 M12）と同じで
	// 「間違った耐力壁が残る」＝無いより悪い状態になる。PIO はリセットのたびに対象レイヤの
	// 柱を実際に探し、見つけた柱の**内法**へ絵を描き直す。VW は PIO が描いたジオメトリを
	// 図面に保存するので、プラグインを入れていない環境でも図面はそのまま表示できる。
	//
	// 【線分 PIO】柱芯どうしを結ぶ 2 点で置く（kParametricSubType_Linear）。両端の
	// ハンドルがそのまま「どの柱とどの柱の間か」を表すので、手で伸ばしても意味が保たれる。
	//
	// 【伏図と軸組図で描き分ける】PIO は 2D（平面）と 3D の両方を描き、VW が図に応じて
	// 使い分ける（伏図＝2D、軸組図＝3D）:
	//   * 伏図 … 筋かいは三角記号、面材は壁に平行な線と丸印。**面や筋かいのポリゴンは
	//     描かない**（軸組材と被って図が読めなくなる）。
	//   * 軸組図 … 筋かいは形状どおりの帯（軸組内法へクリップした実幅）、面材は軸組内法を
	//     埋める矩形。表と裏はクラスを分け、**ハッチングの向き**で見分ける
	//     （ハッチングそのものはクラス属性なので、テンプレート側が持つ。
	//     プラグインはリソースを作らない。CLAUDE.md「既存の図面リソースを作らない」）。
	//
	// 【軸組内法は描くときに決まる】start / end は**柱芯**で、実際に絵を描く範囲（内法）は
	// PIO が見つけた柱の断面から引く。柱が見つからない図面（柱レイヤが無い・柱を消した）
	// でも絵が消えないよう、解析時に IFC から測った内法（clearSpan）を控えとして持たせる。
	//
	// フィールド:
	//   layer                  … PIO を置くデザインレイヤ名（"1-耐力壁"。parse/ShearWall）
	//   drawClass              … PIO 本体の作図クラス（予約語 class を機械置換）
	//   targetLayers           … 柱を探すデザインレイヤ名を ";" で連ねたもの
	//                            （"1to2-柱;1to3-柱"）。**その階を base とする span 柱レイヤ
	//                            すべて**を渡す——管柱と通し柱が別レイヤに分かれるため、
	//                            1 つでは片端の柱を取り逃がす（parse/Column の span レイヤ）
	//   start                  … 軸の始点＝柱芯（センタリング済みの平面座標）
	//   end                    … 同 終点。**start は (x, y) の辞書順で小さい方**に固定する
	//                            ——表／裏の左右がこの向きで決まるので、列挙順で反転しては困る
	//   kind                   … 種別（筋かい／面材）
	//   braceStyle             … 筋かいの掛け方（面材では未使用）
	//   braceRisesToEnd        … 片掛け筋かいが end 側で高くなるか（たすき掛け・面材では未使用）
	//   panelSide              … 面材を設ける面（筋かいでは未使用）
	//   width                  … 筋かいの見付け幅（mm。壁面内で筋かいの軸に直交する幅。
	//                            面材では 0）
	//   thickness              … 材厚（mm。筋かい＝壁面に直交する厚み、面材＝板厚）
	//   panelOffset            … 面材の中心面が軸から離れる距離（mm・正）。筋かいでは 0
	//     ※ thickness / panelOffset は**測った実物の値の記録**で、いまの作図には使わない。
	//        軸組図は通り芯（＝壁芯）で切った断面なので、面材の面も筋かいの帯も**壁芯の
	//        鉛直面**に置く（実物の位置へ外すと切断面の外に出て図から消える。M19）。
	//   clearSpan              … IFC から測った内法（mm）。柱が見つからないときの控え
	//   bottomHeight           … 軸組内法の下端（**配置先レイヤ平面からの相対 Z**。mm）
	//   topHeight              … 同 上端
	//
	// 【高さは「レイヤ平面からの差」で持つ】配置先の "n-耐力壁" レイヤは**その階の横架材天端**
	// に載る（parse/Story）ので、土台天端から梁下端までの内法はそのままこの 2 つで表せる。
	// シンボル置換系（SymbolCommand::zOffset）と同じ考え方で、描画側が絶対 Z を引き直す
	// 必要がない形にしてある。
	struct ShearWallCommand
	{
		std::string layer;
		std::string drawClass;
		std::string targetLayers;
		Vec2 start;
		Vec2 end;
		ShearWallKind kind = ShearWallKind::Brace;
		ShearWallBraceStyle braceStyle = ShearWallBraceStyle::Single;
		bool braceRisesToEnd = true;
		ShearWallPanelSide panelSide = ShearWallPanelSide::Front;
		double width = 0.0;
		double thickness = 0.0;
		double panelOffset = 0.0;
		double clearSpan = 0.0;
		double bottomHeight = 0.0;
		double topHeight = 0.0;
	};

	// 断面寸法データタグ 1 つ（ビューポート注釈のデータタグ。docs/DEV-NOTES.md M13）。
	// 横架材 1 本の断面寸法（"120×180"）をその図の上に表示するための命令で、**IFC
	// ではなく横架材命令（MemberCommand）から導出する**（parse/Tag）。
	//
	// 【どのビューポートに載るかは構造で決まる】タグは ViewportCommand の中に住む——
	// 「この図に載せる注釈」という関係をフィールド（レイヤ名・図番）の突き合わせではなく
	// **入れ子**で表す。タグを平らに持って「tag['layer'] がそのビューポートの表示レイヤに含ま
	// れるか」で振り分ける形は、伏図だけなら成り立つが**軸組図（断面ビューポート）
	// では成り立たない**——断面に映る横架材はレイヤでは選べず、切断面に乗るかどうかで決まるた
	// め。振り分けを解析側（parse/Tag）で済ませてしまえば、描画側は「そのビューポートの
	// tags を置く」だけになり、伏図と軸組図で 1 つの実装を共有できる。
	//
	// 【position は注釈空間の座標】ビューポート注釈はその図の**投影された 2 次元空間**に
	// 置かれる。伏図（平面ビューポート）は真上から見た図なので投影＝平面座標そのままだが、
	// 軸組図（断面ビューポート）は横から見た図なので**(切断線に沿った距離, 高さ Z)** になる
	// （どちらを x に取るかは視線の向きが決める。parse/Tag.h「断面の注釈空間」）。
	//
	// 【position は必ず注釈空間の絶対座標】どちらの図でも命令の position をそのまま注釈空間の
	// 座標として使う。伏図は注釈空間がモデルの平面座標そのもので、軸組図は横方向の原点だけが
	// 世界座標と違う——**その原点合わせは解析側（parse/Tag の sectionAlongOrigin）が済ませて
	// ある**ので、描画側は「命令の位置へ置く」だけでよい。
	//
	// **VW にタグの位置を決めさせようとしてはならない。** データタグを関連付けると VW が
	// 関連付け先へ吸着させる、という前提で「VW が置いた位置からの相対」で決める作りを一度
	// 試したが、実機の実測は**どの候補からも数 m ずれ**、前提そのものが成り立たないことが
	// 分かった（parse/Tag.h「［遠回りの記録］」）。
	//
	// 【position は「部材の辺」で、タグの大きさは描画側が測る】position はタグの中心では
	// なく**部材の辺の中央**（＝タグの下端中央を接させたい点）で、そこから offset の向きへ
	// **タグの実寸の半分**だけ押し出した位置がタグの中心になる。タグの実寸はタグレイアウトの
	// 中身（文字の大きさ・枠）が決めるので**描いてみるまで分からない**——だから命令は「どこに
	// 接するか」と「どちらへ逃がすか」だけを持ち、大きさは draw/Tag が GetObjectBounds で
	// 測って当てる（軸組図のビューポートを実寸で並べるのと同じ考え方。draw/Section）。
	//
	// **測るついでに置き直しも兼ねる**: VW は作成時の挿入点をそのまま守るとは限らない
	// （ローカル確認で、指定した点ではなく横架材の端部に置かれた）。描画側は最後に実位置を
	// 測って目標との差だけ動かすので、VW がどこへ置いたかに依らず同じ結果になる
	// （draw/Tag.cpp「置いた後に測って直す」）。
	//
	// 【スタイル名は持たない】タグの見た目も中身（断面寸法の書式）も、描画側が**タグ 1 本ずつへ
	// 直接組むタグレイアウト**が決める——データタグスタイルは作らないし当てない（draw/Tag.h の
	// ★。スラブ・壁が構成層をオブジェクトへ直接与えるのと同じ扱い）。
	//
	// フィールド:
	//   memberIndex                  … 関連付け先の横架材（Document::members の添字）
	//   position                     … 注釈空間での**部材の辺の中央**（タグの下端中央が接する点）
	//   offset                       … 部材から逃がす向き（単位ベクトル）。逃がす量は
	// **タグ自身の大きさ**（レイアウトの中身が決める）に依るので、position へ畳み込まず向きだけ
	// を持つ
	// angle                        … 文字角度（度。(-90, 90] に正規化済み）（'layer'
	// は持たない。上記のとおり振り分けは解析側で済ませるため。'style' も持たない＝上記）
	struct TagCommand
	{
		std::size_t memberIndex = 0;
		Vec2 position;
		Vec2 offset;
		double angle = 0.0;
	};

	// シートレイヤに載せるビューポート 1 枚。伏図は「特定のデザインレイヤ群だけを見下げた図」
	// なので、命令が持つのは**どのレイヤを見せるか**と図面タイトル・図番だけになる
	// （docs/DEV-NOTES.md M13）。
	//
	// フィールド:
	//   drawingTitle                     … 図面タイトル（"1階床伏図" 等）
	//   drawingNumber                    … 図番（シートレイヤ番号と同じ文字列）
	//   layers                           … 表示するデザインレイヤ名（**それ以外は非表示**）
	//
	// 【並びは重ね順ではない】layers の並び順は描画側の走査順にすぎず、伏図での重なりは
	// **ドキュメントのデザインレイヤ重ね順**が決める。床・野地板が柱・梁を覆い隠さないように
	// する件は、描画側が core::desiredStoryLayerOrder の希望順へレイヤを並べ替えて満たす
	// （draw/Story の reorderStoryLayers。命令にレイヤ順を持たせないのは、全ビューポートで
	// 同じ 1 本の希望順を使うため——命令ごとに複製すると希望順の定義が命令の数だけ増える）。
	//
	// 【クラス単位の非表示は持たない】どの伏図もクラスで絞らないので、命令には持たせず、
	// 描画側は全クラスを表示にする（使われない枠を先に作らない方針。空レイヤを作らないのと同
	// じ）。クラスで絞る伏図が実際に要るときにフィールドごと足す。
	struct ViewportCommand
	{
		std::string drawingTitle;
		std::string drawingNumber;
		std::vector<std::string> layers;

		// M13 断面寸法データタグ。この図に載せる注釈（TagCommand の doc コメント参照）。
		// 伏図・軸組図とも同じ形で持ち、描画側は種類を区別せずに置く。
		std::vector<TagCommand> tags;
	};

	// 伏図のグラフィック凡例 1 つ（VW 標準の "GraphicLegend" PIO。docs/DEV-NOTES.md M13）。
	// **シートレイヤの上**に置く——ビューポート注釈ではない（データタグとはそこが違う）。
	//
	// 【スタイルは作らないし当てない】凡例は**スタイル無しのオブジェクト**として置く
	// （スラブ・壁・データタグと同じ扱い。SDK リファレンス Findings「Graphic Legends」）。したがって
	// 命令はスタイル名を持たない——ユーザーの図面に名前付きリソースを増やさず、取り込みごとに
	// 同じ中身のスタイルが "-2"、"-3" … と並ぶこともない（CLAUDE.md 開発の基本方針 4）。
	//
	// 【どのシートに載るかは構造で決まる】凡例は SheetCommand の中に住む——タグを
	// ViewportCommand の中に持たせたのと同じ理由で、「このシートに載せる」という関係を番号の
	// 突き合わせではなく**入れ子**で表す（平らに並べて番号で突き合わせる形にすると、
	// 突き合わせ自体がバグの種になる）。
	//
	// フィールド:
	//   （フィールドは 1 つも無い。凡例が「有る」ことだけを表す＝SheetCommand の
	//   　std::optional が立っているかどうかがすべて）
	//
	// 【置き場所は持たない】凡例をどこへ置くかは**用紙の大きさが決まってから**でないと
	// 決められない（M18）。用紙の大きさはシートレイヤから SDK で読むものなので、解析側には
	// 分からない——描画側が core::planLayout の legendTopRight（＝ビューポートのために空けた
	// 右の 1 列の右上）へ寄せる。かつては固定の配置点（用紙原点）を命令が持っていたが、
	// 「ローカルの VW で最終調整する」と書き置いたまま図と重なる位置に出ていた。
	//
	// 【スタイルも縮尺も持たない】凡例は**スタイル無し**で置き、中身（ソース定義）は
	// タグ付きデータで与える（draw/Legend）。イメージの縮率はその伏図の縮尺に合わせるが、
	// その縮尺は用紙への収まりから描画時に決まる（core::planLayout）ので、解析側では
	// 決められない。
	//
	// 【載せるシンボルの一覧は持たない】凡例に何が並ぶかは凡例オブジェクト自身のソース定義
	// が決めるので、描画側が一覧を使う経路が無い（使われない枠を先に作らない。
	// ColumnMarkCommand が記号サイズを持たないのと同じ）。ただし**実際にアンカーボルトを
	// 配置したときだけ基礎伏図に凡例を出す**判断は解析側（parse/Sheet）が持つので、空の凡例が
	// 図面に出ることはない。
	//
	// 【どのビューポートで絞るかも持たない】凡例はさらに**そのシートのビューポートで
	// フィルタ**され、その図に映っているシンボルだけが並ぶ（draw/Legend の
	// ApplyViewportFilter）。絞り先は「このシートのビューポート」で構造から決まり、描画側が
	// 同じループで持っているので、命令に持たせる必要が無い（凡例を SheetCommand の中に
	// 入れ子で置いた効き目がここにも出ている）。
	struct LegendCommand
	{
	};

	// シートレイヤ 1 枚（＋その上のビューポート 1 枚）を生成する命令。draw/Sheet がこれを
	// シートレイヤとビューポートへ変換する（docs/DEV-NOTES.md M13）。
	//
	// フィールド:
	//   number                … シートレイヤ番号（**レイヤ名がこれを担う**。"1" / "2" …）
	//   title                 … シートレイヤのタイトル（"基礎伏図" 等）
	//   viewport              … そのシートに載せるビューポート 1 枚
	//   legend                … そのシートレイヤに載せるグラフィック凡例（無い伏図もある。
	//                           番号で突き合わせず入れ子で持つ理由は上記）
	struct SheetCommand
	{
		std::string number;
		std::string title;
		ViewportCommand viewport;

		// M13 グラフィック凡例。**シートレイヤの上**に 1 つ（LegendCommand の doc コメント
		// 参照）。凡例を載せない伏図（アンカーボルトを 1 本も置かなかった基礎伏図）では空。
		std::optional<LegendCommand> legend;
	};

	// 断面ビューポート（軸組図）の向き。X通り＝定 X の切断面（指示線は Y 方向へ延びる）、
	// Y通り＝定 Y の切断面（指示線は X 方向へ延びる）。文字列ではなく enum で持つ
	// （ColumnMarkStyle と同じ流儀）。
	enum class SectionDirection
	{
		X,
		Y,
	};

	// 断面ビューポート（軸組図）1 枚を**新規作成**する命令（docs/DEV-NOTES.md M14）。
	//
	// 【断面ビューポートはその場で作る】ISDK::CreateSectionViewport で**検出した通りの数だけ
	// 新規に作る**ので、図面テンプレートに断面ビューポートを仕込んでおく必要も、
	// その枚数に縛られることも無い（parse/Section.h 参照）。
	//
	// フィールド:
	//   direction                     … X通り / Y通り
	//   lineStart                     … 断面指示線の始点（切断位置。センタリング済みの平面座標）
	//   lineEnd                       … 同 終点
	//   viewPoint                  … **視線の向き**を示す点（指示線の中点から見る側へ
	//                                 離した点）。断面ビューポートを新規に作るので、視線の
	//                                 向きは命令が決める。
	//   viewport                       / 'drawing_title' … 図番（通り名 "X1" / "又い"）・
	//                                 図面タイトル（"X1通り"）と、映すデザインレイヤ
	//
	// **断面の範囲（長さ・高さ・奥行き）は持たない**: **命令ごとに変わる値が無い**ため。
	// 内訳は 奥行き＝0（＝無限）／高さ＝建物を包む実寸＋余白（core::sectionHeightRange。
	// 文書に 1 つで足りる）／長さ＝断面線の長さ（指示線を通り芯 bbox より十分外へ延ばして
	// 実質無制限にする）で、**「無限」なのは奥行きだけ**（長さ・高さを無限にする手段は
	// SDK に無い。経緯は draw/Section.cpp 冒頭）。切断面より奥を出すか・
	// プレイナー図形を出すか・2D コンポーネントを出すかという表示の作法も同じ理由で
	// **draw/Section の名前付き定数**が持つ（描き方であって、どこを切るかではない）。
	//
	// **並べる位置も持たない**: シートレイヤ上での配置は、実際にできたビューポートの大きさに
	// 合わせて詰める必要があり、大きさは描いてみるまで分からない（draw/Section が
	// GetObjectBounds で測って並べる）。
	//
	// **シートレイヤ番号・タイトルも持たない**（M18）: 軸組図は 1 枚の用紙に複数並び、
	// **何枚の用紙に分かれるかは用紙の大きさと縮尺が決める**ので、命令ごとに配置先の
	// シートレイヤを名指しできない（用紙の大きさは描くときにしか分からない）。番号の
	// 始まりとタイトルの基は文書に 1 つ（SectionSheetCommand）だけ持ち、実際の割り付けは
	// 描画側が core::sectionLayout で決める。
	struct SectionCommand
	{
		SectionDirection direction = SectionDirection::X;
		Vec2 lineStart;
		Vec2 lineEnd;
		Vec2 viewPoint;
		ViewportCommand viewport;
	};

	// 軸組図を載せるシートレイヤの**通し方**（docs/DEV-NOTES.md M18）。軸組図は 1 枚の用紙に
	// 複数並び、収まらなければシートレイヤを足していく——その**枚数は用紙の大きさと縮尺が
	// 決める**ので解析側では分からない。ここが持つのは枚数に依らない 2 つだけ:
	//
	//   startNumber           … 最初のシートレイヤ番号。**伏図の続き**（伏図が 1〜7 なら 8）。
	//                           2 枚目以降は 9・10 … と 1 ずつ増やす（描画側）。
	//   title                 … シートタイトルの基（"軸組図"）。複数枚に分かれるときは
	//                           "軸組図(1)" … と連番になる（core::sectionSheetTitle）。
	//
	// sections が空のときは使われない（既定値のまま）。
	struct SectionSheetCommand
	{
		int startNumber = 0;
		std::string title;
	};

	// 命令セット本体。プレーンな構造体の集約（std::vector / std::string / double /
	// enum 等）で表す。
	//
	// TODO: 要素を足すときは、ここに命令リストを 1 本足す。
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

		// M21 基礎。立上り・底盤・地中梁（＋床付け）を**1 つの命令**にまとめたもの
		// （core/Foundation.h の FoundationCommand。parse/Footing の buildFoundationCommand が
		// 組み立てる）。基礎要素が 1 つも無ければ空。配置先の "F-基礎" レイヤは基礎ストーリ
		// （stories の先頭）が作るので、描画は stories の後に処理する。
		std::optional<FoundationCommand> foundation;

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

		// M12 断面記号・伏図記号。**実在する span 柱レイヤごとに 2 つ**（断面記号と
		// 伏図記号）で、断面記号をすべて先に、続けて伏図記号を並べる（parse/ColumnMark）。
		// 柱の span から決まるので columns より後に組み立てる。
		std::vector<ColumnMarkCommand> columnMarks;

		// M19 耐力壁。筋かい（IfcMember "筋かい…"）と面材（IfcWall "面材…"）を、両端を柱に
		// 紐付けた線分 PIO 1 つずつで表す（parse/ShearWall）。配置先の "n-耐力壁" レイヤは
		// stories が作り、柱を探す span 柱レイヤ（targetLayers）は columns から決まるので、
		// その 2 つより後に組み立てる。
		std::vector<ShearWallCommand> shearWalls;

		// M13 シート（伏図）。基礎伏図 → 各階の柱梁伏図 → 屋根版を持つ階ごとの母屋伏図の
		// 順で、シートレイヤ番号もその順に "1" から振る（parse/Sheet が組み立てる）。
		// ビューポートが見せるデザインレイヤはすべて stories が作るので、描画は
		// **全要素の描画が済んだ後**に処理する（draw/ExecuteDocument）。
		std::vector<SheetCommand> sheets;

		// M14 断面ビューポート（軸組図）。柱と梁の両方が通る通り（柱梁の芯）を X・Y 方向
		// それぞれ検出し、切断位置の昇順に X通り → Y通り の順で並べる（parse/Section）。
		// 伏図と同じくモデルを映すので、描画は **全要素の描画が済んだ後**（伏図の後）に
		// 処理する（draw/ExecuteDocument）。
		std::vector<SectionCommand> sections;

		// M18 軸組図のシートレイヤの通し方（番号の始まり＝伏図の続き・タイトルの基）。
		// **何枚に分かれるかは描くときに決まる**ので、枚数に依らないこの 2 つだけを持つ
		// （SectionSheetCommand 参照）。sections が空なら使われない。
		SectionSheetCommand sectionSheet;
	};

	// ------------------------------------------------------------------------
	// 描画結果の件数
	// ------------------------------------------------------------------------

	// 命令セットを描画した結果、実際に**描けた**数（命令数ではない）。draw/executeDocument が
	// 返し、メニューコマンドの完了ダイアログ（parse/Summary の formatImportResult）が読む。
	// 命令はあるのに 0 なら「配置先レイヤが無い」「PIO / オブジェクトを作れなかった」等の
	// 描画側の問題だと分かり、ローカル確認で原因を解析側と切り分けられる（命令数は
	// Document から取れる）。valid は validateDocument を通ったか。
	//
	// **なぜ core にあるか**: 完了文言の整形は無 SDK 側で行いたい（要素が増えるたびに
	// SDK 側の文字列を手で伸ばす作業を無くす。docs/DEV-NOTES.md M15「完了文言の集約」）。
	// そのためには parse/Summary から件数を読めなければならないが、parse/ は draw/ を
	// include できない（依存の向き。CLAUDE.md）。命令セットと対になる「その実行結果」
	// なので、フェーズをつなぐ唯一の境界であるこのヘッダに置き、draw/ExecuteDocument.h は
	// 別名（using DrawCounts = core::DrawCounts）で受ける。**SDK 型は載せない**という
	// Document と同じ規律がここにも効く。
	struct DrawCounts
	{
		bool valid = false;
		std::size_t stories = 0;
		std::size_t grids = 0;
		std::size_t floors = 0;
		std::size_t members = 0;
		std::size_t columns = 0;
		std::size_t rafters = 0;
		std::size_t roofs = 0;
		// M21 基礎。**置けた PIO の数**（0 か 1）。命令はあるのに 0 なら PIO を作れなかった
		// （プラグインの登録漏れ・レイヤ未生成）。原因は診断行に出る（draw/Footing）。
		std::size_t foundation = 0;
		// M11 シンボル置換系。アンカーボルト・床束の配置先（"F-アンカーボルト" / "F-床束"）は
		// 基礎ストーリのレイヤなので、基礎の無いモデルでは命令自体が出ない（parse 側で空になる）。
		std::size_t anchorBolts = 0;
		std::size_t floorPosts = 0;
		std::size_t fireBraces = 0;
		std::size_t joints = 0;

		// M12 断面記号・伏図記号。**span 柱レイヤごとに置いた記号 PIO の数**（記号そのものの
		// 個数ではない——1 つの PIO がそのレイヤの柱すべてに記号を描く）。
		std::size_t columnMarks = 0;

		// M19 耐力壁。**置けた線分 PIO の枚数**（筋かい 1 組・面材 1 枚につき 1 つ）。
		std::size_t shearWalls = 0;

		// M13 シート（伏図）。**ビューポートまで作れた枚数**（シートレイヤだけできた場合は
		// 数えない）。命令はあるのに 0 なら、原因は診断行に出る（draw/Sheet）。
		std::size_t sheets = 0;

		// M14 軸組図。**作れた断面ビューポートの枚数**（＝柱梁の芯を通る通りの数）。
		// 命令はあるのに 0 なら、原因は診断行に出る（draw/Section）。
		std::size_t sections = 0;

		// 進捗ダイアログの「キャンセル」で途中打ち切りになったか。true のときは各要素の
		// 件数が命令数に届かないのが正常で、描けたところまでは図面に残る（要らなければ
		// 「取り消し」で消せる。undoArmed 参照）。完了ダイアログはこれを見て中止を明示する。
		bool cancelled = false;

		// **「取り消し」1 回で取り込みを戻せる状態にできたか。** 描画は自前の undo イベントで
		// 包み、このインポートが新しく作ったレイヤを登録する（レイヤを消せば上の図形も消える。
		// draw/DrawUtil.h「取り込み全体の Undo」）。登録できるレイヤが 1 つも無ければ false で、
		// そのときイベントごと捨てる（中途半端な取り消しは図面を壊す）。
		bool undoArmed = false;

		// 取り消しが**部分的**か。取り込み前から在ったレイヤへも描いた場合（同じ文書への
		// 2 回目の取り込み等）、その分はレイヤごと消すわけにいかないので戻らない。
		bool undoPartial = false;

		// 描画側で起きた**異常**の説明（無ければ空）。要素ごとに 1 行を改行で連ねる。実描画は
		// ローカルの VectorWorks でしか確認できないので、「命令はあるのに見えない」ときに
		// 原因を解析側と描画側で切り分ける手掛かりを診断ログへ持ち帰る。
		//
		// **ここに入るのは「起きてはいけないこと」だけ。** 完了ダイアログはこれが空かどうかで
		// 「問題あり」を判断する（M19）ので、平常でも出る説明を混ぜると毎回「問題あり」に
		// なってしまう。そちらは下の notes へ。
		std::string diagnostics;

		// 異常ではないが**後から知りたい**説明（無ければ空）。用紙の割り付けの内訳のように、
		// 平常でも必ず出るが「図が思ったより小さい」を追うときに要る値がここへ入る。
		// 完了ダイアログには出さず、**診断ログにだけ**書く（M19）。
		std::string notes;
	};

	// Document を描画前に検証する。draw/ は検証を通った Document だけを SDK API へ渡す。
	// 現状はバージョンと stories / floors / rafters / roofs / grids / シンボル 4 種を見る（規
	// 則は Document.cpp の各 isValid* 参照。空の Document は妥当）。各命令リストの追加に合わせ
	// て検証規則を足していく。
	bool validateDocument(const Document& document);

	// 断面（軸組図）の高さ範囲に足す上下の余白（mm）。基礎の底や屋根の頂部を切り落とさない
	// ための遊びで、sectionHeightRange とその期待値を書くテストが共有する。
	inline constexpr double kSectionHeightMargin = 1000.0;

	// 取り込んだ要素（床・横架材・柱・屋根組・基礎・ストーリ）の Z から、建物を包む高さ範囲
	// （絶対 Z。上下に kSectionHeightMargin の余白つき）を返す。高さの分かる要素が 1 つも
	// 無ければ false（out は変更しない）。
	//
	// **なぜ要るか**: 断面ビューポートの高さ範囲は ISDK::CreateSectionViewport の引数でしか
	// 与えられず、**「無限」を指定する手段が SDK に無い**（ObjectVariables にも該当の
	// 変数が無く、断面まわりの呼び出しは CreateSectionViewport / CreateSectionLineInstance /
	// IsSectionLineLinkedToViewport / UpdateSectionLineInstances だけ。ci-debug で確認）。
	// 0 を渡すと**奥行きは無限**になるが**高さは「有限 0〜0」**になってしまい、断面から
	// 建物が消えかねない。そこで高さだけは実寸＋余白の有限値を渡す（docs/DEV-NOTES.md M14）。
	//
	// SDK を触らない純計算なので core に置いて無 SDK でテストする（desiredStoryLayerOrder と
	// 同じ立ち位置。CLAUDE.md「テスト方針」）。
	bool sectionHeightRange(const Document& document, double& start, double& end);

	// 平面（伏図）の広がりに足す四方の余白（mm）。通り芯の丸（通り名の吹き出し）や部材の
	// 太さは命令の座標には現れないので、その分の遊びを持たせる。planContentBounds とその
	// 期待値を書くテストが共有する。
	//
	// **通り芯の線そのものは既に広がりに入っている**（GridCommand の始点・終点を見るため）
	// ので、ここで見込むのは丸と線の太さだけでよい。1/50 なら 300mm ＝ 用紙の 6mm で、
	// 丸（用紙 4mm 前後）を十分覆う。当初の 500mm は 1/50 で 10mm と過剰で、境目の建物が
	// 1 段階小さい縮尺へ落ちる原因になっていた（M18 のローカル確認）。
	inline constexpr double kPlanContentMargin = 300.0;

	// 取り込んだ要素の平面座標から、図に映るものを包む矩形（センタリング済みの平面座標。
	// 四方に kPlanContentMargin の余白つき）を返す。layers が空でなければ**そのレイヤに
	// 載る命令だけ**を見る（伏図 1 枚が映す範囲）。座標を持つ命令が 1 つも無ければ false
	// （out は変更しない）。
	//
	// **なぜ要るか**（docs/DEV-NOTES.md M18）: 用紙に合わせて縮尺を選ぶには「図がどれだけの
	// 広がりを持つか」が要る。ビューポートの実寸は描いてみるまで分からないが、**そこに何が
	// 映るかは命令セットが全部知っている**ので、実寸を測る前に縮尺を決められる。
	// 併せて、**用紙をめくっても図が動かない**ように置くのにも使う——伏図ごとに映すレイヤが
	// 違えば図の中身の広がりも違うので、シートごとの広がり（layers 指定）と文書全体の
	// 広がり（layers 空）の差が、そのシートで図をどれだけずらせばよいかを与える。
	//
	// SDK を触らない純計算なので core に置いて無 SDK でテストする（sectionHeightRange と
	// 同じ立ち位置）。
	bool planContentBounds(const Document& document, const std::vector<std::string>& layers,
						   Vec2& min, Vec2& max);

	// 軸組図 1 枚ぶんの広がり（実寸 mm）。幅は**平面の広がりの大きい方**——X通りは Y 方向の
	// 広がりを、Y通りは X 方向の広がりを映すので、どちらも同じ大きさのマスに収まるように
	// 大きい方を採る（用紙の上で段が揃う）。高さは断面の高さ範囲（sectionHeightRange）。
	// どちらかが求まらなければ false（out は変更しない）。
	bool sectionContentSize(const Document& document, Vec2& size);

	// 耐力壁の筋かい 1 本を、軸組内法に納まる多角形として返す（座標は **(軸方向, 高さ)**
	// ＝壁面内の 2D で、PIO のローカル XZ にそのまま載る）。内法の矩形は
	// [clearStart, clearEnd] × [bottom, top]。
	//
	// 【形】筋かいは内法の対角線に沿った幅 width の帯で、帯の角は内法の外へはみ出す。
	// 実物も柱・横架材へ突き当たる形で納まるので、**内法の矩形で切って**返す
	// （帯の 4 つの角がそれぞれ別の辺で落ちるので、切り口は端が斜めの八角形になる）。
	// risesToEnd が真なら clearStart 側が下・clearEnd 側が上、偽ならその逆。
	//
	// 内法が潰れている（幅または高さが 0 以下）・幅が 0 以下のときは空を返す。
	//
	// **描画側から切り離せる純計算**なので core に置いて無 SDK でテストする
	// （rafterEaveEnd・core/Foundation の foundationSolids と同じ立ち位置。CLAUDE.md「テスト方針」）。
	std::vector<Vec2> shearWallBracePolygon(double clearStart, double clearEnd, double bottom,
											double top, double width, bool risesToEnd);

	// 希望するデザインレイヤのスタック順（ナビゲーション上→下）を返す。draw/Story がこの順を適
	// 用する（レベルの高さには依存しない）。SDK を触らない純計算なので core に置いて無 SDK
	// で単体テストする（CLAUDE.md「テスト方針」: レイヤ順の並べ替え計算のような SDK
	// から切り離せる部分は core へ寄せてテストする）。
	//
	// 適用先は **draw/Story の reorderStoryLayers**（InsertObjectAfter でレイヤの並びを
	// 希望順へ揃える）ただ 1 か所。当初は per-viewport の重ね順上書きへ委ねたが実機で
	// 効かなかった（経緯は draw/Story.h の reorderStoryLayers）。
	//
	// 並び: 最上段に通り芯レイヤ "共通" → topLayers（伏図記号レイヤ "{to}-柱伏図記号" 等・
	// ストーリ非依存の独立レイヤ。M12 で reorderStoryLayers が渡すようになった）→
	// **最上階→最下階**の順に各ストーリのレイヤ（stories は
	// Elevation 昇順＝最下階→最上階なので逆順に辿る）。各ストーリ内は levels の並び順。
	// ただし 2 種類のレベルだけは階をまたいで集めて端へ回す:
	//   * 床（FL）・野地板 … スタック最下段（背面）。伏図ビューポートで柱・梁を覆い隠さない。
	//   * 耐力壁（M19） … topLayers の直後（最前面群）。耐力壁が伏図へ出すのは**注記**
	//     （筋かいの三角・面材の丸）で、横架材や柱の絵に隠されると読めない。実機で
	//     「記号が横架材の後ろに隠れる」ことを確認して前面へ回した。
	std::vector<std::string> desiredStoryLayerOrder(const std::vector<StoryCommand>& stories,
													const std::vector<std::string>& topLayers = {});
} // namespace HomeskzIfcImport::core
