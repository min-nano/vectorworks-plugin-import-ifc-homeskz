//
//	draw/Rafter.h
//
//	Phase 2（VW 描画）の垂木モジュール。Python 版 vw/rafter.py に対応する。
//	命令セット（core::RafterCommand）を**軸組ツール（FramingMember、部材種別 rafter）**の
//	オブジェクトとして配置する（ROADMAP.md M6）。
//
//	【SDK 依存】実装（draw/Rafter.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは core/Document.h までしか参照しないので、SDK を持たない翻訳単位からも
//	安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document の rafter 命令を描く。配置した本数を返す。
	//
	// 配置先レイヤ（"n-垂木"）が無い命令はスキップする（レイヤは story 命令が作るので、
	// 無い＝そのストーリの生成がスキップされたということ。垂木のために勝手にレイヤを
	// 作らない。Python 版 execute_rafters と同じ規約）。
	std::size_t drawRafters(const core::Document& document);
} // namespace HomeskzIfcImport::draw
