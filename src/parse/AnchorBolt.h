//
//	parse/AnchorBolt.h
//
//	Phase 1（IFC 解析）のアンカーボルトモジュール（docs/DEV-NOTES.md M11「シンボル置換系」）。
//	IfcMechanicalFastener のうちボルト本体だけを拾い、ハイブリッドシンボルへ置換する
//	core::SymbolCommand を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・柱（parse/Column の配置・型名ヘルパー）・通り芯（parse/Grid の
//	センタリング中心）だけで完結する（CLAUDE.md「Phase 1」）。
//
//	解析の要点:
//	  * ホームズ君 IFC のアンカーボルトは **IfcMechanicalFastener 2 要素**（ボルト本体と
//	    角座金）で表され、型（IfcMechanicalFastenerType）の名前で見分ける:
//	      - ボルト本体 … "アンカーボルト:{種別}:定着長さ:{長さ}mm"
//	      - 座金       … "アンカーボルト座金:{種別}"
//	    **本体だけ**を対象にする（座金は同じ軸芯にあるので両方採ると二重になる。座金の
//	    無い「座金なし」ボルトも本体は必ずあるので、本体基準なら取りこぼしも無い）。
//	  * 置換シンボルは種別コードから決める。ホームズ君のサンプルに M12 / M16 の直接表記が
//	    無いため、要件どおり **座金付き（Z1/Z2 等）→ アンカーボルト_M12 / 座金なし →
//	    アンカーボルト_M16** に振り分ける。
//	  * 基準点はボルト軸芯（ローカル配置 Location の XY）を通り芯と同じグリッド中心
//	    オフセットで補正した平面座標。**高さは命令に持たせない**——基準は基礎天端で、
//	    配置先レイヤ "F-アンカーボルト" のストーリレベルが担う。
//
//	【配置先レイヤ】"F-アンカーボルト" は**基礎ストーリの基礎天端レベル**に紐づくデザイン
//	レイヤで、これを作るのは parse/Footing の buildFoundationStoryCommand（M9 の基礎ストーリに
//	M11 で基礎天端レベルを足した）。レイヤ名の定数は parse/Footing.h に 1 つだけ置き、
//	配置先を名乗る側（ここ）とレベルを作る側の両方がそれを通る。解析自体は基礎の有無に
//	依存しないので、ここでは常に命令を組み立てる（基礎の無いモデルではレイヤが無く、
//	描画側がスキップして診断行に出す）。
//

#pragma once

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "parse/Step.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// ボルト本体の型名接頭辞。座金の型名 "アンカーボルト座金:…" はこれに一致しない（"
	// アンカーボルト" の直後がコロンかどうかで分かれる）。
	inline constexpr const char* kAnchorBoltTypePrefix = "アンカーボルト:";

	// 座金なしを表す型名中のトークン。
	inline constexpr const char* kWasherlessToken = "座金なし";

	// 置換するハイブリッドシンボル名は**取り込み設定が持つ**（core::SymbolRole の
	// AnchorBoltM12 / AnchorBoltM16。既定は "アンカーボルト_M12" / "アンカーボルト_M16"）。
	// 設定ダイアログで図面の別のシンボルへ差し替えられる（core/ImportOptions.h）。

	// 型名がアンカーボルト**本体**（座金でない）を表すか。
	bool isAnchorBoltType(const std::string& typeName);

	// ボルト本体の型名から**役割**を決める。型名が座金なしなら座金なしの役割、そうでなければ
	// （Z1/Z2 等の角座金付き）座金付きの役割。名前も「取り込むか」も、この役割を鍵に
	// 取り込み設定から引く。
	core::SymbolRole anchorBoltRole(const std::string& typeName);

	// ボルト本体の型名から置換するシンボル名を返す（＝上の役割に割り当てられた名前）。
	std::string resolveAnchorBoltSymbol(const std::string& typeName,
										const core::ImportOptions& options);

	// STEP Model からアンカーボルトのシンボル配置命令を組み立てる。
	//
	// IfcMechanicalFastener を #id 昇順に走査し、型名がボルト本体のものだけを採る。
	// 配置先は "F-アンカーボルト"（parse/Footing の kLayerFoundationAnchor）。配置座標を
	// 解決できないボルトはスキップする（1 本の欠損で全体を止めない。CLAUDE.md
	// 「エラーハンドリング」）。並びは #id 昇順で、エンティティ列挙順に依存しない。
	std::vector<core::SymbolCommand> buildAnchorBoltCommands(const Model& model);

	// 同上。共有コンテキストのセンタリング中心を使う（parse/Context.h）。
	std::vector<core::SymbolCommand> buildAnchorBoltCommands(Context& context);
} // namespace HomeskzIfcImport::parse
