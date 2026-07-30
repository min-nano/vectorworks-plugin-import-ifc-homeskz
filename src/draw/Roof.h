//
//	draw/Roof.h
//
//	Phase 2（VW 描画）の野地板モジュール。Python 版 vw/roof.py に対応する。
//	命令セット（core::RoofCommand）を**屋根オブジェクト（屋根ツール）**として配置する
//	（ROADMAP.md M6）。屋根版 1 面＝野地板 1 枚。
//
//	【SDK 依存】実装（draw/Roof.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは core/Document.h までしか参照しないので、SDK を持たない翻訳単位からも
//	安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document の roof 命令を描く。配置した枚数を返す。
	//
	// 配置先レイヤ（"n-野地板"）が無い命令はスキップする（レイヤは story 命令が作るので、
	// 無い＝そのストーリの生成がスキップされたということ。Python 版 execute_roofs と同じ規約）。
	std::size_t drawRoofs(const core::Document& document);
} // namespace HomeskzIfcImport::draw
