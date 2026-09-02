//
//	parse/Footing.h
//
//	Phase 1（IFC 解析）の基礎モジュール（docs/DEV-NOTES.md M9「基礎」・M21「基礎を独自 PIO
//	へ」）。ホームズ君 IFC の基礎要素（IfcFooting と底盤の IfcSlab）を Name で分類し、
//	**基礎全体を 1 つの命令（core::FoundationCommand）**へ組み立てる。
//
//	部品は**同一仕様ごとに外形の多角形でまとめたグループ**（core/Foundation.h 冒頭）:
//	  * 立上り（基礎梁。Name が "基礎梁" 始まりの IfcFooting）→ **天端の面**の外形を天端の
//	    高さごとに（FoundationRiserGroup）
//	  * 底盤（Name に "底盤" を含む IfcSlab / IfcFooting）→ 外形を厚さと天端の高さごとに
//	    （FoundationSlabGroup）
//	  * 地中梁（Name に "地中梁" を含む IfcFooting）→ 台形断面を当てはめて（core::
//	    fitFoundationBeam）**底の面**の外形を、底の高さと斜め寸法ごとに（FoundationBeamGroup）
//	同じグループの中で繋がる外形は和で 1 枚へ畳む（core::mergePolygons。畳めない＝穴ができる
//	・複数に分かれる成分はそのまま残す）。
//
//	M9〜M17 はこれらを壁・スラブ・モディファイアと**別々の VW オブジェクト**にしていたが、
//	M21 で **1 つの自作 PIO**（Extensions/ExtFoundation）にまとめた。解析側の仕事は
//	「IFC から部品の実寸を正しく取り出すこと」に絞られ、壁結合・端部のキャップ・床付けの
//	組み立てといった**描き方の都合**はここから無くなった（床付けは PIO が描くたびに
//	core/Foundation が組み立てる）。
//
//	加えて、基礎要素があるときだけ**基礎ストーリ**（"基礎" / suffix "F" / GL=0）を組み立てる。
//	buildDocument はこれを stories の**先頭**（最下層）へ置く（parse/BuildDocument）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）だけで完結し、
//	通常の C++ ツールチェインでコンパイル・単体テストできる（CLAUDE.md「Phase 1」）。
//
//	【基礎ストーリのレベルは 3 つ】スタック順（上→下）に 基礎天端（アンカーボルト）→ GL
//	（基礎の PIO）→ 床束。基礎天端・床束は M11 のシンボルの高さ基準で、シンボル自身は高さを
//	持たない。M9〜M19 にあった "底盤天端"（"F-底盤"）は M21 で無くなった——底盤は基礎の PIO の
//	中にあり、レイヤを分ける相手がいない。
//
//	【立上りの後処理は 3 段】ホームズ君 IFC の立上りは通り芯の交点等で細かく分断され、かつ
//	自由端が柱芯までの長さで入力されている。そこで
//	  1. mergeWallCommands  … 同一直線上・同一断面の立上りを 1 本へ統合する
//	  2. extendFreeWallEnds … 他の立上りと交差しない端点を「柱芯 + 半壁厚」へ延長する
//	  3. applyWallOpenings  … 人通口の区間で立上りを分割／天端を切り下げる（M10）
//	の順に通してから部品にする。**人通口は統合・延長の後に当てはめる**ので、開口を跨いで
//	統合された立上りも開口位置で正しく分割され、開口境界の端は実寸法のまま（延長しない）に
//	なる。交差する立上りどうしは PIO の中で**ソリッドが重なる**だけで、結合の命令は要らない
//	（IFC の立上りはコーナーで相手の外面までモデル化されている）。
//
//	【底盤の後処理は 2 段】
//	  1. mergeSlabCommands      … 同厚・同高で連続する底盤を多角形の和で 1 枚へ統合する
//	  2. alignSlabsToWallFaces  … 外形が立上りの**壁心**に一致しているので、辺ごとに沿う
//	                              立上りの**外面**（壁心 + 半壁厚）まで外側へ広げる
//	地中梁の底盤への振り分けと床付け（M10 / M17）は PIO 側（core::foundationSolids）へ移った。
//

#pragma once

#include "core/Document.h"
#include "core/Foundation.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 基礎要素を分類する Name の接頭辞・部分文字列。
	inline constexpr const char* kFoundationWallPrefix = "基礎梁";
	inline constexpr const char* kGroundBeamToken = "地中梁";
	inline constexpr const char* kBaseSlabToken = "底盤";

	// 基礎ストーリの名前・接尾辞。
	inline constexpr const char* kStoryFoundation = "基礎";
	inline constexpr const char* kFoundationSuffix = "F";

	// 基礎ストーリのレベル種別。**文字列の定義は core/Document.h（命令セットの語彙）**に
	// あり、ここはその再公開（parse/Story の kLevelFL 等と同じ流儀）。GL は基礎の PIO
	// （M21）、基礎天端・床束は M11（アンカーボルト・床束のシンボル）が使う。
	inline constexpr const char* kLevelGL = core::kLevelGL;
	inline constexpr const char* kLevelFoundationTop = core::kLevelFoundationTop;
	inline constexpr const char* kLevelFloorPost = core::kLevelFloorPost;

	// 基礎のデザインレイヤ名。**一般階のように"{接頭辞}-{レベル種別}" では組み立てられない**
	// （レベル "GL" に対してレイヤは "F-基礎"）ので、規約ではなく名前そのものをここに
	// 1 つずつ置く。
	//
	// kLayerFoundation は基礎の PIO（立上り・底盤・地中梁・床付けをまとめた 1 つのオブジェクト）
	// を置くレイヤで、GL レベル（高さ 0）に紐づく。**高さ 0 でなければならない**——PIO は
	// 部品の Z を GL 基準の絶対値で持ち、レイヤの高さを読まない（core/Foundation.h）。
	inline constexpr const char* kLayerFoundation = "F-基礎";
	// 同じく M11 のシンボルの配置先（アンカーボルト＝基礎天端レベル、床束＝床束レベル）。
	// **シンボル（parse/AnchorBolt・parse/FloorPost）が配置先を名乗るときと、基礎ストーリが
	// そのレベルを作るときの両方がこの定数を通る**（規約がズレると、命令はあるのに配置先が
	// 見つからず 1 つも描かれない形になる）。
	inline constexpr const char* kLayerFoundationAnchor = "F-アンカーボルト";
	inline constexpr const char* kLayerFoundationFloorPost = "F-床束";

	// 立上りのマージ・自由端判定の許容値（mm / sin 角）。同一直線判定の直交距離・
	// 区間の重なり／接触の隙間・断面キーの丸め桁に使う。
	inline constexpr double kWallMergeDistTol = 1.0;
	inline constexpr double kWallMergeAngleTol = 1e-3;
	inline constexpr double kWallEndpointTol = 1.0;

	// 人通口（立上りに開けた人が通る開口）の許容値（mm）。順に「開口が壁芯上に乗っているとみ
	// なす直交距離」と「分割で残す区間の最小長」（これ以下の切れ端は作らない）。
	inline constexpr double kOpeningMatchTol = 2.0;
	inline constexpr double kOpeningMinSegment = 1.0;

	// 地中梁のマージ許容値。順に 距離（mm）・平行判定（sin 角）・断面キーの丸め（mm）・
	// 方位角キーの丸め（度）。
	inline constexpr double kGroundBeamMergeTol = 1.0;
	inline constexpr double kGroundBeamMergeAngleTol = 1e-3;
	inline constexpr double kGroundBeamProfileTol = 1.0;
	inline constexpr double kGroundBeamAzimuthTol = 0.1;

	// 自由端の終端柱（柱芯）を探す許容値（mm）。沿軸距離は土台の半材せい（≤ ~75mm）を覆いつつ、
	// 隣接する 1 モジュール（≥455mm）先の柱を拾わない値。直交距離は半壁厚に加える許容。
	inline constexpr double kFreeEndColumnAlongTol = 150.0;
	inline constexpr double kFreeEndColumnPerpTol = 20.0;

	// 底盤のマージ・外面合わせの許容値。順に 距離（mm）・平行判定（sin 角）・境界辺の「すぐ右
	// （外側）」を見るサンプル距離（mm。部材寸法より十分小さく、頂点丸めより十分大きい）。
	inline constexpr double kSlabMergeTol = 1.0;
	inline constexpr double kSlabAngleTol = 1e-3;
	inline constexpr double kSlabSideEps = 1e-2;

	// --- 解析の中間表現（命令ではない）---------------------------------------------
	//
	// 立上り・底盤は IFC から取り出した後に統合・延長・外面合わせを経て部品になる。その
	// 途中の形をここに持つ（core::FoundationCommand の部品と同じ意味の値だが、後処理の
	// 関数群が使う名前——壁芯・壁厚——で呼ぶ）。

	// 立上り（基礎梁）1 本。壁芯（センタリング済み）・壁厚・下端／天端の絶対 Z。
	//
	// 【下端は IFC 実形状のまま】ホームズ君は基礎梁を**底盤の底面まで**の全高でモデリングする
	// （実測: 伏図次郎・サンプル1 は全本が Z=−100＝底盤天端 50 − 底盤厚 150、スキップフロアは
	// −100 と −150 が混在）。したがって下端はソリッドの下端をそのまま使い、呑み込み等の
	// 補正はしない。深さの差（外周が深い等）は地中梁が持つので、基礎梁側で作り込まない。
	struct RiserPiece
	{
		core::Vec2 start;
		core::Vec2 end;
		double thickness = 0.0;
		double bottom = 0.0;
		double top = 0.0;
	};

	// 底盤 1 枚。平面外形（センタリング済み・末尾に始点を重複させない）・コンクリート厚
	// （整数 mm に丸めたもの）・天端の絶対 Z。
	struct SlabPiece
	{
		std::vector<core::Vec2> boundary;
		double thickness = 0.0;
		double elevation = 0.0;
	};

	// Name による基礎要素の判別。
	// **述語はここが唯一の定義**で、解析も判定（hasFoundation）も同じ関数を通る。
	bool isFoundationWall(const std::string& name);
	bool isGroundBeam(const std::string& name);
	bool isBaseSlab(const std::string& name);

	// 基礎の対象要素（IfcFooting すべてと、底盤の IfcSlab）の #id を返す。IfcFooting →
	// IfcSlab の順・型内は #id 昇順で決定的。
	std::vector<int> collectFootingElements(const Model& model);

	// 底盤天端の絶対 Z。底盤の天端 Z ごとに平面面積を合計し、**合計面積が最大**の天端 Z
	// を採る（最初に見つかった値ではないので、エンティティ列挙順に依存しない決定的な高さにな
	// る。同一面積なら高い方）。底盤が 1 枚も無ければ false（out は変更しない）。
	bool resolveSlabTopElevation(const Model& model, double& out);

	// 基礎天端＝**立上り（基礎梁）の天端**の絶対 Z。アンカーボルト（M11）の高さ基準になる。
	// 立上りの天端 Z のうち**最大値**を採る（最初に見つかった値ではないので、エンティティ列挙
	// 順に依存しない決定的な高さ）。立上りが 1 つも無い基礎（底盤のみ）は false で、
	// 呼び出し側が底盤天端へフォールバックする。
	bool resolveFoundationTopElevation(const Model& model, double& out);

	// 基礎（立上り・底盤・地中梁）が 1 つでもあるか。
	bool hasFoundation(const Model& model);

	// 基礎ストーリの story 命令を組み立てる。基礎要素が 1 つも無ければ false（out
	// は変更しない）。ストーリ高さは GL=0（常に）で、levels の並びは**希望するデザインレイヤ
	// のスタック順（上→下）**に基礎天端（立上り天端の絶対 Z・"F-アンカーボルト"）→ GL（0・
	// "F-基礎"）→ 床束（底盤天端の絶対 Z・"F-床束"）の 3 つ（ヘッダ冒頭「基礎ストーリの
	// レベルは 3 つ」）。立上りが無い基礎は基礎天端を底盤天端へフォールバックする。
	bool buildFoundationStoryCommand(const Model& model, core::StoryCommand& out);

	// 立上り（基礎梁）を組み立てる。
	//
	// 壁芯は配置原点から押し出し方向へ伸ばした線、壁厚は矩形断面の幅（XDim）。**非矩形断面の
	// 立上りは壁厚が定まらないのでスキップする**。下端・天端はソリッドの実寸（絶対 Z）。
	// 座標は通り芯と同じグリッド中心オフセット。
	//
	// 組み立てたあと mergeWallCommands → extendFreeWallEnds → extendDeeperCollinearEnds →
	// applyWallOpenings（人通口）を通す。自由端を柱芯へ寄せるのに柱命令（columns）を使う
	// （未指定なら端点から半壁厚延長する＝後方互換）。
	std::vector<RiserPiece> buildWallCommands(const Model& model);
	std::vector<RiserPiece> buildWallCommands(Context& context,
											  const std::vector<core::ColumnCommand>& columns);

	// 同一直線上にあり同一断面（壁厚・下端・天端が一致）の立上りを 1 本へ統合する。
	// 断面キーごとにグループ化し、各グループ内で Union-Find により「同一直線上で区間が重なる／
	// 接触する」立上りの連結成分をまとめ、成分ごとに先頭の壁芯方向へ全端点を射影した最小〜最
	// 大区間の 1 本にする。
	//
	// **統合しないもの**: 断面が違う（壁厚・高さの違う）立上り／同一直線上でも隙間がある
	// 立上り／平行だが別の線上（直交距離が壁厚ぶんある側並び）の立上り。隙間を橋渡しして
	// 実在しない壁を作らないため。グループ化・成分処理とも入力順に対して決定的。
	std::vector<RiserPiece> mergeWallCommands(const std::vector<RiserPiece>& walls);

	// 他の立上りと交差しない端点（自由端）を、柱芯を基準に半壁厚だけ外側へ延長する。
	// ホームズ君 IFC の自由端は基本的に柱芯までの長さで入力されているが、実際の立上りはそこか
	// ら半壁厚だけ長い。交差する端点（コーナー・T 字）は相手壁の外面までモデル化済みなので触
	// らない。
	//
	// **同一直線上で突き合わせになっている端は自由端ではない**（延長すると隣へ食い込む）。
	// 交点判定は平行な立上りを除外するので、上端／下端が違って統合できなかった隣どうしが
	// 端で接している場合、そのままでは両端とも自由端に見えて互いに半壁厚ずつ重なる
	// （実データで 75mm / 150mm の重なりとして現れた。docs/DEV-NOTES.md M10）。
	//
	// **半島状の立上り**（スラブの取り付かない外部へ突き出す自由端）は、端部を受ける管柱の
	// 柱芯より外側に土台の半材せい（約 50mm）ぶん長く入力されていることがある。そのまま
	// 延長すると柱芯から「半材せい + 半壁厚」ぶん突き出して長くなりすぎるので、columns が
	// 与えられたときは終端柱の柱芯を壁芯へ射影した点を基準にしてから延長する（＝柱芯 +
	// 半壁厚に揃える）。終端柱が見つからない自由端は端点から半壁厚延長する。
	std::vector<RiserPiece> extendFreeWallEnds(const std::vector<RiserPiece>& walls,
											   const std::vector<core::ColumnCommand>& columns);

	// 人通口（立上りに開けた人が通る開口）の削り取り区間。start / end は削り取りソリッドの壁
	// 芯方向の両端（センタリング済みの平面座標）、zBottom / zTop はワールド絶対 Z
	// の下端／上端。人通口は立上りの**天端から下方へ**削り取られるので、zBottom
	// が「削り残る立上りの新しい天端」になる。
	struct WallOpening
	{
		core::Vec2 start;
		core::Vec2 end;
		double zBottom = 0.0;
		double zTop = 0.0;
	};

	// 立上り（基礎梁）に設定された人通口をすべて集める。人通口は立上りソリッドから差演算
	// （IfcBooleanResult の DIFFERENCE）で削り取られた
	// **第 2 オペランド**として表される（人通口は通り芯ではなく実寸法でモデル化される）。
	//
	// **端部が他材で削られた全高の差演算を人通口と誤認しない**ため、(1) 天端が素の立上りの
	// 天端まで届き、(2) 下端が素の立上りの底面までは届かない削りだけを人通口とみなす。
	// 押し出しが鉛直（壁芯が水平でない）な削りも人通口ではないので落とす。
	// center は通り芯のセンタリング中心（buildWallCommands と同じ補正を掛けるため）。
	std::vector<WallOpening> collectWallOpenings(const Model& model, const core::Vec2& center);

	// 人通口を、統合・自由端延長まで済んだ立上りに当てはめて分割／切り下げる。各開口が乗る立
	// 上り（壁芯と平行・直交距離が kOpeningMatchTol 以内・開口の中点が区間内）を探し、その
	// 1 本を分割後の列に置き換える。
	//
	//   * 開口の下端（zBottom）が**底盤天端以下**なら、その区間には立上りが生じない
	//     （底盤だけになる）ので**区間を空けて両側の立上りだけ**を出す。
	//   * それより高ければ、その区間だけ**天端を開口下端へ切り下げた**立上りを挟む。
	//
	// 開口境界の端は**長さ補正しない**（人通口は実寸法でモデル化されているため）。乗る
	// 立上りが見つからない開口は無視する。1 本に複数の開口があっても、更新後の列に順に
	// 当てはめるので正しく処理される。入力順に対して決定的。
	std::vector<RiserPiece> applyWallOpenings(const std::vector<RiserPiece>& walls,
											  const std::vector<WallOpening>& openings,
											  double slabTopAbs);

	// 同一直線上で線が続いている端のうち、**深いほうの立上り**を直交する立上りの半壁厚だけ
	// 伸ばす（＝相手の壁芯を越えさせる）。伸ばした結果を返す。
	//
	// 【なぜ要るか】上端が同じで**下端だけ違う**立上りは統合できない（底盤厚が違う箇所で
	// 起きる。mergeWallCommands の統合キーに下端が入る）。この 2 本がちょうど直交する立上りの
	// 壁芯上で突き合わさると、どちらも「そこで終わる壁」になり、**直交する立上りの反対側の
	// 面まで届く壁が 1 本も無い交点**になってしまう——コーナーの外側の四角が欠ける。
	// 一直線に並ぶ 2 本のうち**下端が低い＝深いほうを勝たせて**相手の半壁厚だけ伸ばすと、
	// その壁が交点を通り抜けて欠けが埋まる（伸ばす量は extendFreeWallEnds の自由端延長と
	// 同じ考え方＝相手の外面まで）。**上端の違う隣は対象にしない**（低い側の端部は段差として
	// 実在する）。同じ深さなら添字の小さいほうが勝つ（決定的）。
	std::vector<RiserPiece> extendDeeperCollinearEnds(const std::vector<RiserPiece>& walls);

	// 地中梁を台形プリズム（core::BeamPrism）へ変換する。各地中梁は水平押し出しの台形断面
	// ソリッドなので、押し出し方向の方位角と、幅軸 u（走る向きを +90 度回した水平単位ベクトル）
	// ・鉛直軸 v で取り直した断面にする。組み立てたあと mergeGroundBeamPrisms を通す。
	// center は通り芯のセンタリング中心。押し出しが水平でない要素は落とす。
	std::vector<core::BeamPrism> buildGroundBeamPrisms(const Model& model,
													   const core::Vec2& center);

	// 同一直線上に並ぶ同一断面形状の地中梁を 1 本の台形プリズムへ統合する。グループキー（高さ
	// ＝下端 z・方位角・断面形状）ごとにまとめ、グループ内で同一軸線上・区間が連続するものを
	// Union-Find で連結成分にし、成分ごとに先頭の軸方向へ全端点を射影した最小〜最大区間の
	// 1 本にする（断面・向き・高さは先頭を引き継ぐ）。
	//
	// **統合しないもの**: 断面が違う／向き（方位角）が違う／別の軸線上（直交距離がある）／
	// 高さが違う／同一直線上でも隙間がある地中梁（隙間を橋渡しして実在しない梁を作らない）。
	// 断面キーは頂点の絶対 (u, v) 位置を保つので、軸に対する横位置の違う地中梁も別扱い。
	// 入力順に対して決定的。
	std::vector<core::BeamPrism> mergeGroundBeamPrisms(const std::vector<core::BeamPrism>& prisms);

	// 底盤を組み立てる。平面外形をグリッド中心オフセットで補正して格納し、天端の絶対 Z を
	// elevation に、Z 厚を整数 mm に丸めた値を thickness に入れる。
	//
	// 組み立てたあと mergeSlabCommands → alignSlabsToWallFaces を通す（外面合わせに使う立上りは
	// walls）。
	std::vector<SlabPiece> buildSlabCommands(const Model& model);
	std::vector<SlabPiece> buildSlabCommands(Context& context,
											 const std::vector<RiserPiece>& walls);

	// 同じ厚さ・同じ高さで連続する底盤を 1 枚へ統合する。断面キーごとにグループ化し、
	// グループごとに core::mergePolygons（連結成分 → 和）へ渡す。軸平行の矩形に限らず、
	// 傾いた底盤や 45 度取合いの斜め辺も統合できる。
	//
	// **統合しないもの**: 単独の底盤／和が穴を含む・複数の外形に分かれる成分（＝布基礎の
	// 升目状ラティス。ベタで埋めると部屋の下までコンクリートになり誤り）／和の計算に
	// 失敗した成分（開ループ）。判断は core::mergePolygons が持つ。入力順に対して決定的で、
	// **並びは断面キーの現れた順**（統合の有無で位置が入れ替わらない）。厚さ・高さは
	// グループの先頭の値に揃う（同じキー＝許容内で同じ値）。
	std::vector<SlabPiece> mergeSlabCommands(const std::vector<SlabPiece>& slabs);

	// 底盤の外周を立上りの外面へ合わせて外側へ広げる。ホームズ君 IFC の底盤外形は立上りの**壁
	// 心**に一致しているため、各辺に沿う立上りを探して（辺と壁芯が平行・同一直線上で区間が重
	// なる。最も重なりの大きいものを採る）、その**半壁厚**だけ辺を外向き法線方向へ平行移動し、
	// 隣接する移動後の辺の交点を新しい頂点にする（凸角は外へ伸び、入隅は詰まる）。
	// 立上りに沿う辺が 1 つも無い底盤（独立基礎底盤等）は動かさない。walls が空なら無変更。
	// 入力順に対して決定的。
	std::vector<SlabPiece> alignSlabsToWallFaces(const std::vector<SlabPiece>& slabs,
												 const std::vector<RiserPiece>& walls);

	// 基礎全体を 1 つの命令（core::FoundationCommand）へ組み立てる（M21）。立上り
	// （Context::walls）・底盤（buildSlabCommands）・地中梁（buildGroundBeamPrisms →
	// core::fitFoundationBeam）を**同一仕様ごとの外形多角形のグループ**にし（ヘッダ冒頭）、
	// 配置先レイヤ "F-基礎"・クラス（PIO 本体と底盤・地中梁＝基礎スラブ、立上り＝立ち上がり、
	// 床付け＝素材クラス）・代表値（core::foundationBaseParams）を入れる。基礎要素が無い／
	// 部品が 1 つも取れない IFC は空（std::nullopt）。
	//
	// **高さの取り合いは命令に持たせない**——立上りの下端と地中梁の天端は「その真下／真上に
	// 来る底盤の底面」なので、描くときに決まる（core::foundationSlabBottom）。解析側が持つのは
	// 立上りの天端・地中梁の底という**実測できる値**だけ。
	std::optional<core::FoundationCommand> buildFoundationCommand(Context& context);
	std::optional<core::FoundationCommand> buildFoundationCommand(const Model& model);
} // namespace HomeskzIfcImport::parse
