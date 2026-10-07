//
//	draw/Rafter.h
//
//	Phase 2（VW 描画）の垂木モジュール。命令セット（core::RafterCommand）を**構造材ツール
//	（StructuralMember、構造用途＝垂木）**のオブジェクトとして配置する（docs/DEV-NOTES.md
//	M6・M16）。横架材・柱と同じ PIO・同じ手順（draw/StructuralMember）で、垂木固有なのは
//	軒先まで伸ばしたパス・中下の断面基準点・構造用途・スタイルを当てないことだけ。
//
//	【SDK 依存】実装（draw/Rafter.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
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
	// Document の rafter 命令を描画する。配置した本数を返す。
	//
	// 配置先レイヤ（"n-垂木"）が無い命令はスキップする（レイヤは story 命令が作るので、
	// 無い＝そのストーリの生成がスキップされたということ。垂木のために独自にレイヤを作らない）。
	//
	// progress には 1 件描画するごとに 1 ステップ報告し、**ループの先頭で中止要求を確認して
	// 抜ける**（進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描画済みの分は図面に残る。
	//
	// outDiagnostics が非 nullptr なら、「PIO は生成できたが断面・パスが期待どおりに
	// 設定されなかった」件数を 1 行にまとめて入れる（横架材・柱と同じ扱い。
	// 異常が無ければ変更しない）。実描画はローカルの VectorWorks でしか確認できないので、
	// 原因の切り分けに使う。
	std::size_t drawRafters(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics = nullptr);
} // namespace HomeskzIfcImport::draw
