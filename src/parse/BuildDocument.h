//
//	parse/BuildDocument.h
//
//	Phase 1（IFC 解析）のオーケストレーション。Python 版 ifc/__init__.py の
//	build_document に対応する。IFC ファイルを読み、各要素の parse モジュール
//	（Grid / Story / Member …）を呼んで命令セット（core::Document）を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。通常の C++
//	ツールチェインだけでコンパイル・単体実行・テストできる（CLAUDE.md「Phase 1」）。
//	この宣言も core/Document.h しか依存しない。
//
//	現状は parse/Loader で IFC を読み（テキスト→STEP グラフ）、parse/Story（M3）・
//	parse/Grid（M1）・parse/Floor（M5）・parse/Rafter / parse/Roof（M6）を呼んで Document を
//	組み立てる。残りの要素は対応マイルストーンで足していく。
//

#pragma once

#include "core/Document.h"

#include <string>

namespace HomeskzIfcImport::parse
{
	// IFC ファイルを解析して命令セットを返す。フェーズ境界は値で返す（例外を
	// フェーズ外へ漏らさない）。1 要素の欠損で全体を止めず、Python 版の寛容さ
	// （スキップ・フォールバック）を踏襲する。
	//
	// TODO(M7〜): 横架材（M7）以降の要素ごとの parse モジュールを呼び、Document を
	// さらに肉付けする（ROADMAP.md）。
	core::Document buildDocument(const std::string& ifcPath);
} // namespace HomeskzIfcImport::parse
