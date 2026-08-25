//
//	draw/Roof.h
//
//	Phase 2（VW 描画）の野地板モジュール。命令セット（core::RoofCommand）を**屋根面
//	オブジェクト（Roof Face）**として配置する（docs/DEV-NOTES.md M6）。屋根版 1 面＝野地板
//	1 枚。ISDK には屋根作成の一連の呼び出し（VectorScript の BeginRoof 相当）が無いため、VWFC
//	の VWRoofFaceObj を外形・オブジェクト変数から組み立てる（理由は draw/Roof.cpp 冒頭）。
//
//	【SDK 依存】実装（draw/Roof.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは core/Document.h までしか参照しないので、SDK を持たない翻訳単位からも
//	安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document の roof 命令を描く。配置した枚数を返す。
	//
	// 配置先レイヤ（"n-野地板"）が無い命令はスキップする（レイヤは story 命令が作るので、
	// 無い＝そのストーリの生成がスキップされたということ。野地板のために勝手にレイヤを作らな
	// い）。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描けたところまでは図面に残る。
	std::size_t drawRoofs(const core::Document& document, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::draw
