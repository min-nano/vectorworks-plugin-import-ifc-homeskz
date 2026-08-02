//
//	draw/Column.h
//
//	Phase 2（VW 描画）の柱モジュール。Python 版 vw/column.py に対応する。
//	命令セット（core::ColumnCommand）を**構造材ツール（StructuralMember）**の鉛直材として
//	配置する（ROADMAP.md M8）。管柱・通し柱・小屋束を同じ経路で描き、違いは構造用途
//	（StructuralUse）とクラス・配置先の span レイヤだけ。
//
//	【SDK 依存】実装（draw/Column.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは core/Document.h までしか参照しないので、SDK を持たない翻訳単位からも
//	安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// Document の column 命令を描く。配置した本数を返す。
	//
	// 配置先の span レイヤ（"1to2-柱" 等）が無い命令はスキップする（レイヤは story 命令が
	// 作るので、無い＝そのストーリの生成がスキップされたということ。柱のために勝手にレイヤを
	// 作らない。Python 版 execute_columns と同じ規約）。
	//
	// outDiagnostics に非 nullptr を渡すと、「配置はできたが断面を設定できなかった本数」
	// 「鉛直パスを作れなかった本数」「パスから長さを取れなかった本数」「プラグインスタイルが
	// 見つからない」といった**描画側の異常**を人が読める 1 行として返す（異常が無ければ
	// 触らない）。実描画はローカルの VectorWorks でしか確認できないため、柱が見えないときに
	// 原因を解析側と描画側で切り分けるための唯一の手掛かりになる（横架材と同じ枠組み）。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描けたところまでは図面に残る。
	std::size_t drawColumns(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics = nullptr);
} // namespace HomeskzIfcImport::draw
