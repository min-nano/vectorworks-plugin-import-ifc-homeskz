//
//	parse/FloorPost.h
//
//	Phase 1（IFC 解析）の床束モジュール（docs/DEV-NOTES.md M11「シンボル置換系」）。
//	床束をハイブリッドシンボル "床束" として配置する core::SymbolCommand を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・横架材の配置／断面（parse/Member）・構造クラス
//	（parse/StructuralClass）・基礎の有無（parse/Footing）だけで完結する。
//
//	解析の要点:
//	  * **ホームズ君 EX の IFC に床束は出力されない**（オブジェクト・型・プロパティの
//	    いずれにも現れない）。したがって IFC から抽出できず、要件どおり**大引の下に
//	    一定間隔（910mm＝半間）で決め打ち配置**する。
//	  * **継手の統合**: ホームズ君 IFC の大引は継手（支持材の上での継目）で分断されて
//	    いるが、継手は実際の大引の端ではない。同一直線上で継手（すき間 ≤ 半モジュール）に
//	    分断された大引は 1 連に統合してから間隔を割り付ける（mergeCollinearOhbiki）。
//	    同一直線上でも 1 モジュール以上離れた別々の大引は統合しない。
//	  * **端部は実部材端ではなく「支持材芯」**。ホームズ君 IFC の大引は端部が支持材芯より
//	    半支持材厚だけ内側に納まって描かれており（単モジュール＝910mm 区間で実長 805mm＝
//	    910−105）、実部材端を基準にすると床束が実際より内側へ寄る。各端で大引の芯線と交わる
//	    支持材（土台・**他の大引**）の芯線の交点を端部として扱う（shinReference）。他の大引を
//	    含めるのは、二次大引（他の大引に載る大引）の端も受ける大引の芯を基準にするため。
//	    どの支持材にも受けられていない端（基礎に直接載る端等）は実部材端へフォールバックする。
//	  * **配置**: 始点側の支持材芯から 910mm・1820mm・… と並べる。最後の床束と終点側の
//	    支持材芯との間隔は 910mm 未満の半端でよい。支持材芯そのものには置かない（端部は
//	    支持材が受ける）ので、支持材芯区間が 910mm 以下の大引には床束が 0 本になる。
//	  * **立上り（基礎梁）と重なる位置には置かない**（overlapsFoundationWall）。910mm
//	    の決め打ちで割り付けるため、大引が立上りを跨ぐ位置に床束が来ることがある。
//	    床束は底盤の上に立つ部材なので、立上りのコンクリートと重なる位置には**そもそも立てられ
//	    ない**（実機で立上りの上に床束が描かれていた）。しかもその位置の大引は立上り（とその上
//	    の土台）が受けているので床束は要らない——**間隔を詰め替えるのではなく、その
//	    1 本を落とす**（両隣は立上りから 910mm 以内に収まる）。この規則の出どころは IFC
//	    ではなく**実機の見た目**（基礎伏図で立上りの帯の上に床束の記号が乗っていた）で、
//	    判定には立上りの命令（parse/Context の walls）を使う。
//	  * **高さは命令に持たせない**——基準は基礎底盤上端（底盤天端）で、配置先レイヤ
//	    "F-床束" のストーリレベルが担う。
//	  * **基礎が無いモデルでは空**（parse/Footing の hasFoundation）。配置先レイヤも
//	    高さ基準も定まらないため。
//
//	【配置先レイヤ】"F-床束" は**基礎ストーリの床束レベル**（底盤天端に揃えた高さ）に紐づく
//	デザインレイヤで、これを作るのは parse/Footing の buildFoundationStoryCommand（M9 の
//	基礎ストーリに M11 で床束レベルを足した）。レイヤ名の定数は parse/Footing.h に 1 つだけ
//	置き、配置先を名乗る側（ここ）とレベルを作る側の両方がそれを通る。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <optional>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 置換するハイブリッドシンボル名。
	inline constexpr const char* kSymbolFloorPost = "床束";

	// 床束の配置間隔（mm）。IFC に床束が無いための決め打ち値（半間＝910mm）。
	inline constexpr double kFloorPostInterval = 910.0;

	// 支持材芯の探索許容値（mm）。
	inline constexpr double kFloorPostParallelTol = 1e-9; // 芯線がほぼ平行な支持材は対象外
	inline constexpr double kFloorPostSegTol = 1.0; // 交点が支持材区間からはみ出す余裕
	inline constexpr double kFloorPostShinMargin = 1.0; // 大引端が支持材 footprint に載る余裕

	// 床束と立上りの重なり判定に足す余裕（mm）。床束の平面の大きさは**その床束が受ける
	// 大引の断面幅**で代表させ（床束は大引の直下に立ち、慣習的に大引と同寸）、立上りの
	// footprint を半床束幅ぶん広げてから点で判定する。この定数はそこへさらに足す、座標の
	// 丸め誤差ぶんの余裕（立上りの外面ちょうどに来た床束を「重なっている」と読むための下駄）。
	inline constexpr double kFloorPostWallMargin = 1.0;

	// 同一直線上の大引の継手（継目）判定の許容値。
	inline constexpr double kCollinearAngleTol = 1e-6; // 方向の外積（sin 角）がこれ以下なら平行
	inline constexpr double kCollinearPerpTol = 1.0; // 相手端の芯線からの直交距離
	// 継手のすき間（支持材幅 ≈ 105mm）は半モジュール（455mm）を大きく下回り、別々の大引の
	// 間隔（≥ 1 モジュール ≈ 1000mm）を大きく下回るので、この値で継手と別材を切り分けられる。
	inline constexpr double kJointGapTol = kFloorPostInterval / 2.0;

	// 支持材 1 本の平面芯線。
	//   origin    … 芯線の始点
	//   direction … 単位方向
	//   length    … 芯線長
	//   width     … 断面幅（大引端がこの半分＋余裕以内なら「載っている」とみなす）
	struct SupportLine
	{
		core::Vec2 origin;
		core::Vec2 direction;
		double length = 0.0;
		double width = 0.0;
	};

	// 大引 1 本（または継手で統合した 1 連）の平面芯線。
	//   start / end … 芯線の両端
	//   width       … 断面幅。**この大引の下に立つ床束の平面の大きさ**として使い、立上りと
	//                 重なる床束を落とすのに要る（overlapsFoundationWall）。継手で統合した
	//                 連は成分の最大値を採る（安全側）。
	struct OhbikiRun
	{
		core::Vec2 start;
		core::Vec2 end;
		double width = 0.0;
	};

	// 支持材芯区間 1 つに沿った床束の配置位置（始点側の支持材芯からの距離）を返す。910mm・
	// 1820mm・… と並べ、終点ちょうど以遠には置かない。
	std::vector<double> floorPostOffsets(double length);

	// 大引を受ける支持材（土台・大引）の平面芯線を集める。座標はグリッド中心オフセット**前**
	// の生値。土台だけでなく他の大引も含めることで、二次大引の端も支持材芯を基準にできる（自
	// 身の芯線・同一直線上の大引は shinReference の平行判定で除外される）。
	std::vector<SupportLine> collectSupportLines(const Model& model);

	// 大引（CLASS_OOBIKI）の平面芯線を集める。座標はグリッド中心オフセット**前**の生値。
	std::vector<OhbikiRun> collectOhbikiLines(const Model& model);

	// 大引端 point を受けている支持材（土台・大引）の芯（交点）を返す。大引の芯線（点 point・
	// 方向 direction）と各支持材の芯線の交点のうち、交点が支持材の区間内にあり、
	// かつ大引端から半支持材厚（＋ kFloorPostShinMargin）以内にあるものの中で最も近い交点を返
	// す。平行な支持材（自身の芯線・同一直線上の大引を含む）は交点が定まらないため除外する。
	// 受けている支持材が無ければ nullopt。
	std::optional<core::Vec2> shinReference(const core::Vec2& point, const core::Vec2& direction,
											const std::vector<SupportLine>& supports);

	// 大引 a・b が同一直線上にあるとき、区間のすき間（重なり／接触は 0）を返す。
	// 平行でない／別の直線上にある（直交距離が大きい）ときは nullopt。
	std::optional<double> collinearGap(const OhbikiRun& first, const OhbikiRun& second);

	// 同一直線上で継手（すき間 ≤ kJointGapTol）の大引を 1 連に統合する。Union-Find
	// で連結成分にまとめ、各成分を先頭の芯線方向へ全端点を射影した最小〜最大区間の 1 本にする。
	// 統合は入力順に依存しない（代表は最小インデックス、出力は代表インデックス昇順）。
	std::vector<OhbikiRun> mergeCollinearOhbiki(const std::vector<OhbikiRun>& lines);

	// 床束（position を中心・平面の大きさ postWidth）が立上り（基礎梁）の平面 footprint と
	// 重なるかを返す。position と walls は**どちらもセンタリング済み**の座標で渡す
	// （walls は parse/Footing の buildWallCommands が出したもの＝人通口の分割・切り下げまで
	// 反映済み。開口で立上りが消える区間には壁が無いので、そこの床束は落ちない）。
	//
	// 判定は壁芯を軸にした 2 方向の距離で行う。直交方向は 半壁厚 + 半床束幅 +
	// kFloorPostWallMargin まで、沿軸方向は区間 [0, 壁長] の外側へ 半床束幅 +
	// kFloorPostWallMargin まで（半壁厚は直交方向の寸法なので沿軸には効かない）。これで
	// 立上りの端に寄りかかる床束も重なりとみなす。角の付近だけ丸い床束を仮定した近似に
	// なるが、落としすぎても両隣の床束が 910mm 以内で受けるので実害が無い側へ倒れる。
	// 長さ 0 の立上り（縮退）は向きが定まらないので飛ばす。
	bool overlapsFoundationWall(const core::Vec2& position, double postWidth,
								const std::vector<core::WallCommand>& walls);

	// STEP Model から床束のシンボル配置命令を組み立てる。基礎が無いモデルでは空を返す。
	// 並びは大引の連ごと（collectOhbikiLines の #id 昇順に由来）→ 連内は始点からの距離順で決
	// 定的。
	std::vector<core::SymbolCommand> buildFloorPostCommands(const Model& model);

	// 同上。共有コンテキストのセンタリング中心を使う（parse/Context.h）。
	std::vector<core::SymbolCommand> buildFloorPostCommands(Context& context);
} // namespace HomeskzIfcImport::parse
