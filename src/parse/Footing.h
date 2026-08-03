//
//	parse/Footing.h
//
//	Phase 1（IFC 解析）の基礎モジュール。Python 版 ifc/footing.py に対応する
//	（ROADMAP.md M9「基礎（立上り＝壁・底盤＝スラブ）＋基礎ストーリ」）。ホームズ君 IFC の
//	基礎要素（IfcFooting と底盤の IfcSlab）を Name で分類し、別々のオブジェクトへ変換する。
//
//	  * 立上り（基礎梁。Name が "基礎梁" 始まりの IfcFooting）→ **壁**（core::WallCommand）
//	  * 底盤（Name に "底盤" を含む IfcSlab / IfcFooting）→ **スラブ**（core::SlabCommand）
//	  * 地中梁（Name に "地中梁" を含む IfcFooting）→ **底盤のモディファイア**
//	    （core::ModifierCommand。台形断面のため単一のスラブでは描けず、底盤に噛み合う
//	    台形プリズム＋可視ソリッドとして表す。M10）
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
//	【立上りの後処理は 3 段】ホームズ君 IFC の立上りは通り芯の交点等で細かく分断され、かつ
//	自由端が柱芯までの長さで入力されている。そこで
//	  1. mergeWallCommands  … 同一直線上・同一断面の立上りを 1 本へ統合する
//	  2. extendFreeWallEnds … 他の立上りと交差しない端点を「柱芯 + 半壁厚」へ延長する
//	  3. applyWallOpenings  … 人通口の区間で立上りを分割／天端を切り下げる（M10）
//	の順に通してから命令にする。**人通口は統合・延長の後に当てはめる**ので、開口を跨いで
//	統合された立上りも開口位置で正しく分割され、開口境界の端は実寸法のまま（延長しない）に
//	なる。交差する立上りどうしの壁結合（buildWallJoinCommands）はこの結果に対して求める。
//
//	【底盤の後処理も 3 段】
//	  1. mergeSlabCommands      … 同厚・同高で連続する底盤を多角形の和で 1 枚へ統合する
//	  2. alignSlabsToWallFaces  … 外形が立上りの**壁心**に一致しているので、辺ごとに沿う
//	                              立上りの**外面**（壁心 + 半壁厚）まで外側へ広げる
//	  3. attachGroundBeamModifiers … 地中梁（台形プリズム）を平面で最も重なる底盤へ振り分ける
//	                                 （M10。地中梁を単独のスラブ命令にはしない）
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

	// 人通口（立上りに開けた人が通る開口）の許容値（mm。Python 版 _OPENING_MATCH_TOL /
	// _OPENING_MIN_SEGMENT）。順に「開口が壁芯上に乗っているとみなす直交距離」と「分割で
	// 残す区間の最小長」（これ以下の切れ端は作らない）。
	inline constexpr double kOpeningMatchTol = 2.0;
	inline constexpr double kOpeningMinSegment = 1.0;

	// 壁結合のジャンクション（同一交点に集まる立上りの集合）をまとめる距離許容（mm。
	// Python 版 _JOIN_CLUSTER_TOL）。3 本以上が 1 点に集まるとき、全ペアの交点は数学的に
	// 同一点になるので、この許容内の交点を 1 つのジャンクションへ束ねる。
	inline constexpr double kJoinClusterTol = 1.0;

	// ピック点を交点から「残す側」へ寄せる量の上限（交点〜遠い端点の距離に対する割合。
	// Python 版 _PICK_OFFSET_FRAC）。寄せすぎて遠い端点が最寄りになると VW が残す／詰める
	// 側を取り違えるので、控えめに寄せる。
	inline constexpr double kPickOffsetFrac = 0.4;

	// 地中梁のマージ許容値（Python 版 _GROUND_BEAM_MERGE_TOL / …_ANGLE_TOL /
	// …_PROFILE_TOL / …_AZIMUTH_TOL）。順に 距離（mm）・平行判定（sin 角）・断面キーの
	// 丸め（mm）・方位角キーの丸め（度）。
	inline constexpr double kGroundBeamMergeTol = 1.0;
	inline constexpr double kGroundBeamMergeAngleTol = 1e-3;
	inline constexpr double kGroundBeamProfileTol = 1.0;
	inline constexpr double kGroundBeamAzimuthTol = 0.1;

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
	// 組み立てたあと mergeWallCommands → extendFreeWallEnds → applyWallOpenings（人通口）を
	// 通す。自由端を柱芯へ寄せるのに柱命令（columns）を使う（未指定なら端点から半壁厚延長
	// する＝後方互換）。
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
	// **同一直線上で突き合わせになっている端は自由端ではない**（延長すると隣へ食い込む）。
	// 交点判定は平行な立上りを除外するので、上端／下端が違って統合できなかった隣どうしが
	// 端で接している場合、そのままでは両端とも自由端に見えて互いに半壁厚ずつ重なる
	// （実データで 75mm / 150mm の重なりとして現れた。ROADMAP.md M10）。
	//
	// **半島状の立上り**（スラブの取り付かない外部へ突き出す自由端）は、端部を受ける管柱の
	// 柱芯より外側に土台の半材せい（約 50mm）ぶん長く入力されていることがある。そのまま
	// 延長すると柱芯から「半材せい + 半壁厚」ぶん突き出して長くなりすぎるので、columns が
	// 与えられたときは終端柱の柱芯を壁芯へ射影した点を基準にしてから延長する（＝柱芯 +
	// 半壁厚に揃える）。終端柱が見つからない自由端は端点から半壁厚延長する。
	std::vector<core::WallCommand>
	extendFreeWallEnds(const std::vector<core::WallCommand>& walls,
					   const std::vector<core::ColumnCommand>& columns);

	// 人通口（立上りに開けた人が通る開口）の削り取り区間（Python 版 _WallOpening）。
	// start / end は削り取りソリッドの壁芯方向の両端（センタリング済みの平面座標）、
	// zBottom / zTop はワールド絶対 Z の下端／上端。人通口は立上りの**天端から下方へ**
	// 削り取られるので、zBottom が「削り残る立上りの新しい天端」になる。
	struct WallOpening
	{
		core::Vec2 start;
		core::Vec2 end;
		double zBottom = 0.0;
		double zTop = 0.0;
	};

	// 立上り（基礎梁）に設定された人通口をすべて集める（Python 版 _collect_wall_openings）。
	// 人通口は立上りソリッドから差演算（IfcBooleanResult の DIFFERENCE）で削り取られた
	// **第 2 オペランド**として表される（人通口は通り芯ではなく実寸法でモデル化される）。
	//
	// **端部が他材で削られた全高の差演算を人通口と誤認しない**ため、(1) 天端が素の立上りの
	// 天端まで届き、(2) 下端が素の立上りの底面までは届かない削りだけを人通口とみなす。
	// 押し出しが鉛直（壁芯が水平でない）な削りも人通口ではないので落とす。
	// center は通り芯のセンタリング中心（buildWallCommands と同じ補正を掛けるため）。
	std::vector<WallOpening> collectWallOpenings(const Model& model, const core::Vec2& center);

	// 人通口を、統合・自由端延長まで済んだ立上りに当てはめて分割／切り下げる（Python 版
	// _apply_wall_openings）。各開口が乗る立上り（壁芯と平行・直交距離が kOpeningMatchTol
	// 以内・開口の中点が区間内）を探し、その 1 本を分割後の列に置き換える。
	//
	//   * 開口の下端（zBottom）が**底盤天端以下**なら、その区間には立上りが生じない
	//     （底盤だけになる）ので**区間を空けて両側の立上りだけ**を出す。
	//   * それより高ければ、その区間だけ**天端を開口下端へ切り下げた**立上りを挟む
	//     （中間区間の topBound.offset を「開口下端 Z − 横架材天端の絶対 Z」にする）。
	//
	// 開口境界の端は**長さ補正しない**（人通口は実寸法でモデル化されているため）。乗る
	// 立上りが見つからない開口は無視する。1 本に複数の開口があっても、更新後の列に順に
	// 当てはめるので正しく処理される。入力順に対して決定的。
	std::vector<core::WallCommand> applyWallOpenings(const std::vector<core::WallCommand>& walls,
													 const std::vector<WallOpening>& openings,
													 double slabTopAbs, double beamTopAbs);

	// 同一直線上で線が続いている端のうち、**深いほうの立上り**を直交する立上りの半壁厚だけ
	// 伸ばす（＝相手の壁芯を越えさせる）。伸ばした結果を返す。
	//
	// 【なぜ要るか】上端が同じで**下端だけ違う**立上りは統合できない（底盤厚が違う箇所で
	// 起きる。mergeWallCommands の統合キーに下端が入る）。この 2 本がちょうど直交する立上りの
	// 壁芯上で突き合わさると、どちらも「そこで終わる壁」になり、**通し壁が 1 本も無い交点**に
	// なってしまう。すると直交する立上りは「終わっている壁」への T 結合になり、T 結合は相手が
	// 通し壁でないと成立しないので VW が拒否する（実データの拒否 1 件 (6370,1820)）。
	//
	// 一直線に並ぶ 2 本のうち**下端が低い＝深いほうを勝たせて**相手の半壁厚だけ伸ばすと、
	// その壁が交点を通り抜けて通し壁になり、直交する立上りが正しく T 結合できる（伸ばす量は
	// extendFreeWallEnds の自由端延長と同じ考え方＝相手の外面まで）。**上端の違う隣は対象に
	// しない**（低い側の端部は閉じるので 1 本に見せる必要が無い）。同じ深さなら添字の小さい
	// ほうが勝つ（決定的）。
	//
	// より正確なのは「同一直線上の立上りを 1 本に統合して下端の違いを切り欠きで表す」ことだが、
	// VW の壁は下端の切り欠きを構成層でも高さバインドでも表せず、別途ソリッドで削る仕組みが
	// 要るため採らない（ROADMAP.md M10）。
	std::vector<core::WallCommand>
	extendDeeperCollinearEnds(const std::vector<core::WallCommand>& walls);

	// 交差する立上りどうしの壁結合命令を組み立てる（Python 版 build_wall_join_commands）。
	// walls は buildWallCommands が返した（＝Document::walls と同じ並びの）立上りで、命令の
	// a / b はその添字をそのまま指す。
	//
	// 壁芯が交差する立上りを**同一交点ごとのジャンクション**にまとめ、ジャンクションごとに
	//   * 内部で交わる通し壁があれば … 天端が最も高い通し壁をバックボーンにして他の通し壁を
	//     X 結合（交差結合。**十字は縦横 2 本の壁のまま**で、分割はしない）、端点で突き当たる
	//     壁を T 結合（stem＝a・through＝b）
	//   * 通し壁が無い端点コーナーなら … 天端高さ降順ではじめの 2 本を L 結合、それ以降を
	//     T 結合（バックボーンへ突き当てる）
	// とし、天端高さの違う壁どうしは capped=true（低いほうを a にして高いほうへ結合し端部を
	// 閉じる）、同じ高さなら capped=false（コンクリートで一体なので閉じない）にする。命令は
	// capped=false を先に並べる。同一直線上（平行）の立上りは mergeWallCommands が統合済みで
	// 結合対象にしない。入力順に対して決定的。
	std::vector<core::WallJoinCommand>
	buildWallJoinCommands(const std::vector<core::WallCommand>& walls);

	// 立上りの端部を閉じるか（capStart / capEnd）を壁結合命令から決めて walls へ書き戻す。
	//
	// **VW の壁は端部のキャップを壁ごとに持つ**ので、結合（JoinWalls）の副作用に任せず
	// 明示的に決める（core/Document.h「端部を閉じるかは解析側が決める」）。規則は
	// 「その端に**閉じない結合**（capped=false＝同じ天端の立上りと一体になる結合）が
	// 1 つでもあれば閉じない、無ければ閉じる」。加えて、**同一直線上で突き合わせになる隣**
	// （交点判定に掛からない平行な隣）が同じ天端なら、コンクリートは連続しているので閉じない
	// ——下端だけが違って統合できなかった立上りが平面で 1 本に見えるようにする。したがって
	//   * 自由端                        … 結合が無い → 閉じる
	//   * 同じ天端どうしの L / T / X    … capped=false の結合がある → 閉じない
	//   * 天端の違う相手とだけ結合する端 … capped=true しか無い → 閉じる
	// になる（3 本以上が集まる交点で、高い者どうしが閉じずに繋がり、低い者の端部だけが
	// 閉じる形も自然に出る）。joins は buildWallJoinCommands の戻り＝walls の添字を指す
	// 前提で、範囲外の添字は無視する。
	void applyWallCaps(std::vector<core::WallCommand>& walls,
					   const std::vector<core::WallJoinCommand>& joins);

	// 地中梁を台形プリズムのモディファイアへ変換する（Python 版 _build_ground_beam_modifiers
	// ＋ _ground_beam_modifier）。各地中梁は水平押し出しの台形断面ソリッドなので、押し出し
	// 方向の方位角と、幅軸 u（走る向きを +90 度回した水平単位ベクトル）・鉛直軸 v で
	// 取り直した断面を命令にする。組み立てたあと mergeGroundBeamModifiers を通す。
	// center は通り芯のセンタリング中心。押し出しが水平でない要素は落とす。
	std::vector<core::ModifierCommand> buildGroundBeamModifiers(const Model& model,
																const core::Vec2& center);

	// 同一直線上に並ぶ同一断面形状の地中梁を 1 本の台形プリズムへ統合する（Python 版
	// _merge_ground_beam_modifiers）。グループキー（高さ＝下端 z・方位角・断面形状）ごとに
	// まとめ、グループ内で同一軸線上・区間が連続するものを Union-Find で連結成分にし、成分
	// ごとに先頭の軸方向へ全端点を射影した最小〜最大区間の 1 本にする（断面・向き・高さは
	// 先頭を引き継ぐ）。
	//
	// **統合しないもの**: 断面が違う／向き（方位角）が違う／別の軸線上（直交距離がある）／
	// 高さが違う／同一直線上でも隙間がある地中梁（隙間を橋渡しして実在しない梁を作らない）。
	// 断面キーは頂点の絶対 (u, v) 位置を保つので、軸に対する横位置の違う地中梁も別扱い。
	// 入力順に対して決定的。
	std::vector<core::ModifierCommand>
	mergeGroundBeamModifiers(const std::vector<core::ModifierCommand>& modifiers);

	// 地中梁（台形プリズム）の平面外形＝断面の u 範囲を軸方向へ depth だけ掃引した矩形
	// （Python 版 _modifier_footprint）。底盤への振り分け判定に使う。
	std::vector<core::Vec2> modifierFootprint(const core::ModifierCommand& modifier);

	// 地中梁を、平面外形が最も重なる底盤の modifiers へ振り分ける（Python 版
	// _attach_ground_beam_modifiers）。代表点（重心・各頂点・各辺の中点）が外形内に入る数が
	// 最大の底盤を選び、どの底盤にも入らない（継目・下屋等の）地中梁は重心が最も近い底盤へ
	// フォールバックして取りこぼさない。底盤が 1 枚も無ければ付けられないので捨てる。
	// 入力順に対して決定的。
	void attachGroundBeamModifiers(std::vector<core::SlabCommand>& slabs,
								   const std::vector<core::ModifierCommand>& modifiers);

	// 底盤から slab 命令を組み立てる（Python 版 build_slab_commands）。平面外形をグリッド
	// 中心オフセットで補正して格納し、天端の絶対 Z を elevation に、Z 厚を整数 mm に丸めた
	// 値を thickness（＝スラブスタイルのコンクリート厚）に入れる。天端は底盤天端レベルへ
	// バインドし、offset は実天端 Z と底盤天端の絶対 Z の差（主たる底盤は ≈0）。
	//
	// 組み立てたあと mergeSlabCommands → alignSlabsToWallFaces → attachGroundBeamModifiers を
	// 通す（外面合わせに使う立上りは walls）。**地中梁はスラブにせず**、統合・外面合わせの
	// 済んだ底盤の modifiers へ振り分ける（台形断面は単一のスラブで描けないため）。
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
