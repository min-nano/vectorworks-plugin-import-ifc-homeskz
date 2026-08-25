//
//	draw/Grid.h
//
//	Phase 2（VW 描画）の通り芯モジュール。Python 版 vw/grid.py に対応する。
//	命令セット（core::GridCommand の列）を GridAxis オブジェクトとして VectorWorks に
//	配置する。ExecuteDocument からディスパッチされる（docs/DEV-NOTES.md M1）。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document
//	しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document 内の全通り芯を描く。配置先の「共通」デザインレイヤを（無ければ）用意し、
	// 各 GridCommand を GridAxis のカスタムオブジェクト（PIO）として生成する。PIO の
	// 生成に失敗した場合は通常の直線へフォールバックする（1 本の失敗で全体を止めない。
	// Python 版 vw/grid.py の寛容さ）。実際に配置できた本数を返す。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描けたところまでは図面に残る。
	std::size_t drawGrids(const core::Document& document, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::draw
