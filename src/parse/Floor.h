//
//	parse/Floor.h
//
//	Phase 1（IFC 解析）の床板モジュール。Python 版 ifc/floor.py に対応する。
//	ホームズ君 IFC の床板（Name が "床版" の IfcSlab。鉛直押し出しで、押し出し
//	プロファイルがそのまま床の平面外形になる）を各階の FL レイヤ（"n-FL"）へ描くための
//	命令（core::FloorCommand）に変換する（描画オブジェクトはスラブ。draw/Floor.h 参照）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）だけで
//	完結し、通常の C++ ツールチェインでコンパイル・単体テストできる。
//
//	要件（Python 版 CLAUDE.md「床板」節。ROADMAP.md M5）:
//	  * 床のある場所は IFC から抽出する（床版の平面外形をそのまま床の外形にする）。
//	  * 厚みは 24mm 固定。IFC の押し出し厚（実際には 28mm 等が出力される）は使わない。
//	  * 高さは **IFC の床位置を尊重する**（段差＝スキップフロアを表現する）。床下端の
//	    絶対 Z は床版ソリッドの最下端（ストーリ高さ ＋ ローカル最下端 Z）そのまま。
//	    高さ基準は標準の床高＝「横架材天端」レベル（ストーリ高さ ＋ 横架材天端オフセット）
//	    にバインドし、床下端との差分（ホームズ君で入力した基準高さからの高低差）を
//	    bound.offset に入れる。段差の無い床は offset 0、段差床はここがずれる。
//	  * 最上階（屋根）は FL レイヤを持たない（軒高のみ）ので対象外にする（床版は屋根に無い）。
//
//	床は「建物形状の一次情報」で、以降の横架材・柱はこの位置に合わせて載せていく
//	（ROADMAP.md「実装順序の方針（形状先行）」）。
//

#pragma once

#include "core/Document.h"
#include "parse/Step.h"

#include <vector>

namespace HomeskzIfcImport::parse
{
	// 床板を識別する IfcSlab の Name（Python 版 FLOOR_SLAB_NAME）。
	inline constexpr const char* kFloorSlabName = "床版";

	// 床厚（mm）。要件により 24mm 固定（Python 版 FLOOR_THICKNESS）。IFC の押し出し厚は
	// 使わない（ホームズ君は 28mm 等を出力するが、作図上は 24mm で統一する）。
	inline constexpr double kFloorThickness = 24.0;

	// STEP Model から床板の描画命令を組み立てる（Python 版 build_floor_commands 相当）。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、最上階を除く
	// 各階について、その階に属する床版（Name=="床版" の IfcSlab）を FloorCommand にする。
	// 平面外形は通り芯と同じグリッド中心オフセット（parse/Grid の resolveGridCenter）で
	// 補正する。押し出しソリッドを解決できない床版はスキップする（1 枚の欠損で全体を
	// 止めない。CLAUDE.md「エラーハンドリング」）。
	//
	// 並びは階（Elevation 昇順）→ 階内は要素の出現順（IfcRelContainedInSpatialStructure の
	// #id 昇順・記述順）で、エンティティ列挙順に依存しない決定的な結果になる。
	std::vector<core::FloorCommand> buildFloorCommands(const Model& model);
} // namespace HomeskzIfcImport::parse
