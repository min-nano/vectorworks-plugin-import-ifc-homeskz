//
//	draw/Floor.h
//
//	Phase 2（VW 描画）の床板モジュール。命令セット（core::FloorCommand の列）を**
//	スラブオブジェクト**として VectorWorks に配置する。ExecuteDocument からディスパッチされる
//	（docs/DEV-NOTES.md M5）。
//
//	床ツール（Floor オブジェクト）は実体が押し出しの派生で、オブジェクト構造が押し出しとほぼ変
//	わらない。スラブは BIM オブジェクトとして機能が強化されており発展性が高いので、
//	床はスラブで描く（詳細は .cpp 冒頭）。
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
	// Document 内の全床板を描く。配置先の FL デザインレイヤ（"n-FL"）が既に存在する命令だけを
	// 処理する（レイヤは story 命令が作る。存在しない＝ストーリ生成がスキップされた階なので、
	// 床のために勝手にレイヤを作らない）。実際に配置できた枚数を返す。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描けたところまでは図面に残る。
	std::size_t drawFloors(const core::Document& document, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::draw
