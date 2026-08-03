//
//	parse/Footing.h
//
//	Phase 1（IFC 解析）の基礎モジュール。Python 版 ifc/footing.py に対応する
//	（ROADMAP.md M9「基礎（立上り＝壁・底盤＝スラブ）＋基礎ストーリ」）。ホームズ君 IFC の
//	基礎要素（IfcFooting と底盤の IfcSlab）を Name で分類し、別々のオブジェクトへ変換する。
//
//	  * 立上り（基礎梁。Name が "基礎梁" 始まりの IfcFooting）→ **壁**（core::WallCommand）
//	  * 底盤（Name に "底盤" を含む IfcSlab / IfcFooting）→ **スラブ**（core::SlabCommand）
//	  * 地中梁（Name に "地中梁" を含む IfcFooting）→ **M10**。本 M では読み飛ばす
//	    （台形断面のため単一のスラブでは描けず、底盤のモディファイア＋可視ソリッドになる）。
//
//	加えて、基礎要素があるときだけ**基礎ストーリ**（"基礎" / suffix "F" / GL=0）を組み立てる。
//	buildDocument はこれを stories の**先頭**（最下層）へ置く（parse/BuildDocument）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）だけで完結し、
//	通常の C++ ツールチェインでコンパイル・単体テストできる（CLAUDE.md「Phase 1」）。
//
//	【基礎ストーリのレベルは 4 つ】スタック順（上→下）に 基礎天端（アンカーボルト）→
//	GL（立上り）→ 床束 → 底盤天端 で、Python 版と同じ構成。M9 の時点では描画対象のある
//	2 つ（GL・底盤天端）だけを作っていたが、**M11 でアンカーボルト・床束を導入したので
//	残る 2 つを挿し込んだ**（「描画対象の無いレベルは先に作らない＝空レイヤを作らない」
//	方針の下で、対象が入った時点で足す）。基礎天端・床束は M11 のシンボルの高さ基準で、
//	シンボル自身は高さを持たない。
//
//	【立上りの後処理は 2 段】ホームズ君 IFC の立上りは通り芯の交点等で細かく分断され、かつ
//	自由端が柱芯までの長さで入力されている。そこで
//	  1. mergeWallCommands  … 同一直線上・同一断面の立上りを 1 本へ統合する
//	  2. extendFreeWallEnds … 他の立上りと交差しない端点を「柱芯 + 半壁厚」へ延長する
//	の順に通してから命令にする。人通口（立上りの切り下げ）と壁結合は M10。
//
//	【底盤の後処理も 2 段】
//	  1. mergeSlabCommands      … 同厚・同高で連続する底盤を多角形の和で 1 枚へ統合する
//	  2. alignSlabsToWallFaces  … 外形が立上りの**壁心**に一致しているので、辺ごとに沿う
//	                              立上りの**外面**（壁心 + 半壁厚）まで外側へ広げる
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 基礎要素を分類する Name の接頭辞・部分文字列（Python 版 _WALL_PREFIX /
	// _GROUND_BEAM_TOKEN / _BASE_SLAB_TOKEN）。
	inline constexpr const char* kFoundationWallPrefix = "基礎梁";
	inline constexpr const char* kGroundBeamToken = "地中梁";
	inline constexpr const char* kBaseSlabToken = "底盤";

	// 基礎ストーリの名前・接尾辞（Python 版 STORY_FOUNDATION / FOUNDATION_SUFFIX）。
	inline constexpr const char* kStoryFoundation = "基礎";
	inline constexpr const char* kFoundationSuffix = "F";

	// 基礎ストーリのレベル種別。**文字列の定義は core/Document.h（命令セットの語彙）**に
	// あり、ここはその再公開（parse/Story の kLevelFL 等と同じ流儀）。GL・底盤天端は M9
	// （立上り・底盤）、基礎天端・床束は M11（アンカーボルト・床束のシンボル）が使う。
	inline constexpr const char* kLevelGL = core::kLevelGL;
	inline constexpr const char* kLevelSlabTop = core::kLevelSlabTop;
	inline constexpr const char* kLevelFoundationTop = core::kLevelFoundationTop;
	inline constexpr const char* kLevelFloorPost = core::kLevelFloorPost;

	// 基礎のデザインレイヤ名（Python 版 LAYER_FOUNDATION_WALL / …_SLAB）。**一般階のように
	// "{接頭辞}-{レベル種別}" では組み立てられない**（レベル "GL" に対してレイヤは "F-立上り"）
	// ので、規約ではなく名前そのものをここに 1 つずつ置く。
	inline constexpr const char* kLayerFoundationWall = "F-立上り";
	inline constexpr const char* kLayerFoundationSlab = "F-底盤";
	// 同じく M11 のシンボルの配置先（アンカーボルト＝基礎天端レベル、床束＝床束レベル）。
	// **シンボル（parse/AnchorBolt・parse/FloorPost）が配置先を名乗るときと、基礎ストーリが
	// そのレベルを作るときの両方がこの定数を通る**（規約がズレると、命令はあるのに配置先が
	// 見つからず 1 つも描かれない形になる）。
	inline constexpr const char* kLayerFoundationAnchor = "F-アンカーボルト";
	inline constexpr const char* kLayerFoundationFloorPost = "F-床束";

	// 立上りのマージ・自由端判定の許容値（mm / sin 角。Python 版 _WALL_MERGE_DIST_TOL /
	// _WALL_MERGE_ANGLE_TOL / _JOIN_ENDPOINT_TOL）。同一直線判定の直交距離・区間の
	// 重なり／接触の隙間・断面キーの丸め桁に使う。
	inline constexpr double kWallMergeDistTol = 1.0;
	inline constexpr double kWallMergeAngleTol = 1e-3;
	inline constexpr double kWallEndpointTol = 1.0;

	// 自由端の終端柱（柱芯）を探す許容値（mm。Python 版 _FREE_END_COLUMN_ALONG_TOL /
	// _FREE_END_COLUMN_PERP_TOL）。沿軸距離は土台の半材せい（≤ ~75mm）を覆いつつ、隣接する
	// 1 モジュール（≥455mm）先の柱を拾わない値。直交距離は半壁厚に加える許容。
	inline constexpr double kFreeEndColumnAlongTol = 150.0;
	inline constexpr double kFreeEndColumnPerpTol = 20.0;

	// 底盤のマージ・外面合わせの許容値（Python 版 _SLAB_MERGE_TOL / _SLAB_ANGLE_TOL /
	// _SLAB_SIDE_EPS）。順に 距離（mm）・平行判定（sin 角）・境界辺の「すぐ右（外側）」を
	// 見るサンプル距離（mm。部材寸法より十分小さく、頂点丸めより十分大きい）。
	inline constexpr double kSlabMergeTol = 1.0;
	inline constexpr double kSlabAngleTol = 1e-3;
	inline constexpr double kSlabSideEps = 1e-2;

	// 基礎の構成層の名前。立上りは コンクリート 1 層、底盤は 上から コンクリート → 捨てコン →
	// 砕石。コンクリート厚は要素ソリッドの実寸（整数 mm に丸めたもの）で、捨てコン・砕石は既定値。
	inline constexpr const char* kConcreteComponentName = "コンクリート";
	inline constexpr const char* kSlabLeanConcreteName = "捨てコン";
	inline constexpr const char* kSlabGravelName = "砕石";
	inline constexpr double kSlabLeanConcreteThickness = 30.0;
	inline constexpr double kSlabGravelThickness = 100.0;

	// 底盤のスラブスタイル名（"基礎スラブ - コンクリート 150mm / 捨てコン 30mm / 砕石
	// 100mm"）。**コンクリート厚ごとに 1 つ**で、厚みの違う底盤は別スタイルになる。
	std::string foundationSlabStyleName(double concreteThickness);

	// 底盤スラブの構成層を組み立てる（上から コンクリート → 捨てコン → 砕石）。
	std::vector<core::ComponentCommand> foundationSlabComponents(double concreteThickness);

	// 立上りの壁スタイル名（"基礎立上り - コンクリート 150mm"）。**壁厚ごとに 1 つ**で、
	// 厚みの違う立上りは別スタイルになる（実データの壁厚は 120 / 150 / 300mm と混在するため、
	// Python 版のように 150mm 固定の既製スタイルを全てへ当てると厚みが合わない）。
	std::string foundationWallStyleName(double thickness);

	// 立上りの壁の構成層を組み立てる（コンクリート 1 層。総厚＝壁厚）。
	std::vector<core::ComponentCommand> foundationWallComponents(double thickness);

	// Name による基礎要素の判別（Python 版 _is_wall / _is_ground_beam / _is_base_slab）。
	// **述語はここが唯一の定義**で、解析も判定（hasFoundation）も同じ関数を通る。
	bool isFoundationWall(const std::string& name);
	bool isGroundBeam(const std::string& name);
	bool isBaseSlab(const std::string& name);

	// 基礎の対象要素（IfcFooting すべてと、底盤の IfcSlab）の #id を返す（Python 版
	// _iter_footing_elements）。IfcFooting → IfcSlab の順・型内は #id 昇順で決定的。
	std::vector<int> collectFootingElements(const Model& model);

	// 底盤天端の絶対 Z（Python 版 resolve_slab_top_elevation）。底盤の天端 Z ごとに平面
	// 面積を合計し、**合計面積が最大**の天端 Z を採る（最初に見つかった値ではないので、
	// エンティティ列挙順に依存しない決定的な高さになる。同一面積なら高い方）。底盤が
	// 1 枚も無ければ false（out は変更しない）。
	bool resolveSlabTopElevation(const Model& model, double& out);

	// 基礎天端＝**立上り（基礎梁）の天端**の絶対 Z（Python 版
	// resolve_foundation_top_elevation）。アンカーボルト（M11）の高さ基準になる。立上りの
	// 天端 Z のうち**最大値**を採る（最初に見つかった値ではないので、エンティティ列挙順に
	// 依存しない決定的な高さ）。立上りが 1 つも無い基礎（底盤のみ）は false で、呼び出し側が
	// 底盤天端へフォールバックする。
	bool resolveFoundationTopElevation(const Model& model, double& out);

	// 基礎（立上り・底盤・地中梁）が 1 つでもあるか（Python 版 has_foundation）。
	bool hasFoundation(const Model& model);

	// 基礎ストーリの story 命令を組み立てる（Python 版 build_foundation_story_command）。
	// 基礎要素が 1 つも無ければ false（out は変更しない）。ストーリ高さは GL=0（常に）で、
	// levels の並びは**希望するデザインレイヤのスタック順（上→下）**に
	//   基礎天端（立上り天端の絶対 Z・"F-アンカーボルト"）→ GL（0・"F-立上り"）→
	//   床束（底盤天端の絶対 Z・"F-床束"）→ 底盤天端（同・"F-底盤"）
	// の 4 つ（ヘッダ冒頭「基礎ストーリのレベルは 4 つ」）。立上りが無い基礎は基礎天端を
	// 底盤天端へフォールバックする。
	bool buildFoundationStoryCommand(const Model& model, core::StoryCommand& out);

	// 立上り（基礎梁）から wall 命令を組み立てる（Python 版 build_wall_commands）。
	//
	// 壁芯は配置原点から押し出し方向へ伸ばした線、壁厚は矩形断面の幅（XDim）。**非矩形断面の
	// 立上りは壁厚が定まらないのでスキップする**。下端は基礎（自階）の GL、上端は 1 階
	// （上階＝storyOffset 1）の横架材天端へバインドし、offset は実 Z とバインド先レベルの
	// 絶対 Z の差。座標は通り芯と同じグリッド中心オフセット。
	//
	// 【下端は IFC 実形状のまま】ホームズ君は基礎梁を**底盤の底面まで**の全高でモデリングする
	// （実測: 伏図次郎・サンプル1 は全本が Z=−100＝底盤天端 50 − 底盤厚 150、スキップフロアは
	// −100 と −150 が混在）。したがって下端はソリッドの下端をそのまま使い、**Python 版の
	// 呑み込み（_SLAB_BITE = 10mm 下げ）は行わない**。Python 版のねらいは「底盤に少し
	// 呑み込ませて coplanar による断面の境界線を防ぐ」ことだったが、下端は既に底盤の底面と
	// 一致しているので下へ 10mm 伸ばしても**底盤の下に突き出すだけ**で、意図と逆の結果に
	// なっていた（ローカル確認で判明。ROADMAP.md M9）。深さの差（外周が深い等）は
	// 地中梁（M10）が持つので、基礎梁側で作り込まない。
	//
	// 組み立てたあと mergeWallCommands → extendFreeWallEnds を通す。自由端を柱芯へ寄せるのに
	// 柱命令（columns）を使う（未指定なら端点から半壁厚延長する＝後方互換）。
	//
	// 1 階（最下階の FL ストーリ）が無い IFC では上端のバインド先が決まらないので空を返す。
	std::vector<core::WallCommand> buildWallCommands(const Model& model);
	std::vector<core::WallCommand>
	buildWallCommands(Context& context, const std::vector<core::ColumnCommand>& columns);

	// 同一直線上にあり同一断面（壁厚・上下端の高さ基準が一致）の立上りを 1 本へ統合する
	// （Python 版 merge_wall_commands）。断面キーごとにグループ化し、各グループ内で
	// Union-Find により「同一直線上で区間が重なる／接触する」立上りの連結成分をまとめ、
	// 成分ごとに先頭の壁芯方向へ全端点を射影した最小〜最大区間の 1 本にする。
	//
	// **統合しないもの**: 断面が違う（壁厚・高さの違う）立上り／同一直線上でも隙間がある
	// 立上り／平行だが別の線上（直交距離が壁厚ぶんある側並び）の立上り。隙間を橋渡しして
	// 実在しない壁を作らないため。グループ化・成分処理とも入力順に対して決定的。
	std::vector<core::WallCommand> mergeWallCommands(const std::vector<core::WallCommand>& walls);

	// 他の立上りと交差しない端点（自由端）を、柱芯を基準に半壁厚だけ外側へ延長する
	// （Python 版 _extend_free_wall_ends）。ホームズ君 IFC の自由端は基本的に柱芯までの
	// 長さで入力されているが、実際の立上りはそこから半壁厚だけ長い。交差する端点
	// （コーナー・T 字）は相手壁の外面までモデル化済みなので触らない。
	//
	// **半島状の立上り**（スラブの取り付かない外部へ突き出す自由端）は、端部を受ける管柱の
	// 柱芯より外側に土台の半材せい（約 50mm）ぶん長く入力されていることがある。そのまま
	// 延長すると柱芯から「半材せい + 半壁厚」ぶん突き出して長くなりすぎるので、columns が
	// 与えられたときは終端柱の柱芯を壁芯へ射影した点を基準にしてから延長する（＝柱芯 +
	// 半壁厚に揃える）。終端柱が見つからない自由端は端点から半壁厚延長する。
	std::vector<core::WallCommand>
	extendFreeWallEnds(const std::vector<core::WallCommand>& walls,
					   const std::vector<core::ColumnCommand>& columns);

	// 底盤から slab 命令を組み立てる（Python 版 build_slab_commands）。平面外形をグリッド
	// 中心オフセットで補正して格納し、天端の絶対 Z を elevation に、Z 厚を整数 mm に丸めた
	// 値を thickness（＝スラブスタイルのコンクリート厚）に入れる。天端は底盤天端レベルへ
	// バインドし、offset は実天端 Z と底盤天端の絶対 Z の差（主たる底盤は ≈0）。
	//
	// 組み立てたあと mergeSlabCommands → alignSlabsToWallFaces を通す（外面合わせに使う
	// 立上りは walls）。地中梁はスラブにしない（M10 でモディファイアにする）。
	std::vector<core::SlabCommand> buildSlabCommands(const Model& model);
	std::vector<core::SlabCommand> buildSlabCommands(Context& context,
													 const std::vector<core::WallCommand>& walls);

	// 同じ厚さ・同じ高さで連続する底盤を 1 枚へ統合する（Python 版 merge_slab_commands）。
	// 断面キーごとにグループ化し、各グループ内で「辺を共有／面で重なる」底盤の連結成分を
	// 求め、成分ごとに**任意向きの単純多角形の和**を 1 枚にする（軸平行の矩形に限らず、
	// 傾いた底盤や 45 度取合いの斜め辺も統合できる）。
	//
	// **統合しないもの**: 単独の底盤／和が穴を含む・複数の外形に分かれる成分（＝布基礎の
	// 升目状ラティス。ベタで埋めると部屋の下までコンクリートになり誤り）／和の計算に
	// 失敗した成分（開ループ）。入力順に対して決定的。
	std::vector<core::SlabCommand> mergeSlabCommands(const std::vector<core::SlabCommand>& slabs);

	// 底盤の外周を立上りの外面へ合わせて外側へ広げる（Python 版 align_slabs_to_wall_faces）。
	// ホームズ君 IFC の底盤外形は立上りの**壁心**に一致しているため、各辺に沿う立上りを
	// 探して（辺と壁芯が平行・同一直線上で区間が重なる。最も重なりの大きいものを採る）、
	// その**半壁厚**だけ辺を外向き法線方向へ平行移動し、隣接する移動後の辺の交点を新しい
	// 頂点にする（凸角は外へ伸び、入隅は詰まる）。立上りに沿う辺が 1 つも無い底盤
	// （独立基礎底盤等）は動かさない。walls が空なら無変更。入力順に対して決定的。
	std::vector<core::SlabCommand>
	alignSlabsToWallFaces(const std::vector<core::SlabCommand>& slabs,
						  const std::vector<core::WallCommand>& walls);
} // namespace HomeskzIfcImport::parse
