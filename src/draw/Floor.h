//
//	draw/Floor.h
//
//	Phase 2（VW 描画）の床板モジュール。Python 版 vw/floor.py に対応する。
//	命令セット（core::FloorCommand の列）を**スラブオブジェクト**として VectorWorks に
//	配置する。ExecuteDocument からディスパッチされる（docs/DEV-NOTES.md M5）。
//
//	Python 版は床ツール（Floor オブジェクト）で描くが、床ツールは実体が押し出しの派生で
//	オブジェクト構造が押し出しとほぼ変わらない。スラブは BIM オブジェクトとして機能が
//	強化されており今後の発展性が高いため、本移植ではスラブを使う（詳細は .cpp 冒頭）。
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
	// Document 内の全床板を描く。配置先の FL デザインレイヤ（"n-FL"）が既に存在する
	// 命令だけを処理する（レイヤは story 命令が作る。存在しない＝ストーリ生成が
	// スキップされた階なので、床のために勝手にレイヤを作らない。Python 版
	// execute_floors と同じ規約）。実際に配置できた枚数を返す。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描けたところまでは図面に残る。
	std::size_t drawFloors(const core::Document& document, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::draw
