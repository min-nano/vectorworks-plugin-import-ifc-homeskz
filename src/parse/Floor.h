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
//	要件（Python 版 CLAUDE.md「床板」節＋本移植でのスラブ化。ROADMAP.md M5）:
//	  * 床のある場所は IFC から抽出する（床版の平面外形をそのまま床の外形にする）。
//	  * 高さは **IFC の床位置を尊重する**（段差＝スキップフロアを表現する）。命令が持つ
//	    高さは**床仕上げ上端**の絶対 Z で、一般部は FL と同じ、部分的に床レベルを指定して
//	    いる場合は FL ± 差分になる。高さ基準は配置先ストーリの「FL」レベルにバインドし、
//	    その差分を bound.offset に入れる（一般部は offset 0、段差床はここがずれる）。
//	  * スラブ構成（上から）は 床仕上げ＝FL 高さ − 横架材天端高さ − 24、床下地＝24mm 固定。
//	    合計＝FL 高さ − 横架材天端高さなので、スラブ下端は一般部で横架材天端に一致する。
//	    IFC の押し出し厚（実際には 28mm 等が出力される）は使わない。
//	  * 最上階（屋根）は FL レイヤを持たない（軒高のみ）ので対象外にする（床版は屋根に無い）。
//
//	床は「建物形状の一次情報」で、以降の横架材・柱はこの位置に合わせて載せていく
//	（ROADMAP.md「実装順序の方針（形状先行）」）。
//

#pragma once

#include "core/Document.h"
#include "parse/Step.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// 床板を識別する IfcSlab の Name（Python 版 FLOOR_SLAB_NAME）。
	inline constexpr const char* kFloorSlabName = "床版";

	// 床下地の厚み（mm）。要件により 24mm 固定（Python 版 FLOOR_THICKNESS と同値）。IFC の
	// 押し出し厚は使わない（ホームズ君は 28mm 等を出力するが、作図上は 24mm で統一する）。
	inline constexpr double kSubfloorThickness = 24.0;

	// スラブ構成層の名前。上から 床仕上げ → 床下地。
	inline constexpr const char* kFloorFinishName = "床仕上げ";
	inline constexpr const char* kSubfloorName = "床下地";

	// 床のスラブスタイル名を返す。**階により構成（床仕上げ厚）が異なることが多いため、
	// スタイルは階ごとに 1 つ**作る。一般階は "{階}F-床スタイル"（"1F-床スタイル" …）、
	// 最上階は "屋根-床スタイル"（屋根の床＝小屋裏収納・ロフトの床）。index は 0 始まりの
	// 階インデックス（collectStories の並び）で、isTop は最上階か。
	std::string floorSlabStyleName(std::size_t index, bool isTop);

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
