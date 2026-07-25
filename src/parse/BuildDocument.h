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
//	いまはフォルダ骨組みとして関数シグネチャだけを置く。STEP リーダ（parse/Step）・
//	サニタイズ（parse/Loader）・要素ごとの解析は対応マイルストーンで実装する。
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
	// TODO(M0/M1〜): parse/Loader でサニタイズ込みの読み込み、parse/Step で STEP
	// グラフ構築、要素ごとの parse モジュールで Document を肉付けする（ROADMAP.md）。
	core::Document buildDocument(const std::string& ifcPath);
} // namespace HomeskzIfcImport::parse
