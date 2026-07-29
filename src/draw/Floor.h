//
//	draw/Floor.h
//
//	Phase 2（VW 描画）の床板モジュール。Python 版 vw/floor.py に対応する。
//	命令セット（core::FloorCommand の列）を床ツール（Floor オブジェクト）として
//	VectorWorks に配置する。ExecuteDocument からディスパッチされる（ROADMAP.md M5）。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document
//	しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document 内の全床板を描く。配置先の FL デザインレイヤ（"n-FL"）が既に存在する
	// 命令だけを処理する（レイヤは story 命令が作る。存在しない＝ストーリ生成が
	// スキップされた階なので、床のために勝手にレイヤを作らない。Python 版
	// execute_floors と同じ規約）。実際に配置できた枚数を返す。
	std::size_t drawFloors(const core::Document& document);
} // namespace HomeskzIfcImport::draw
