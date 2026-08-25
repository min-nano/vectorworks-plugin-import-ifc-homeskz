//
//	parse/BuildDocument.h
//
//	Phase 1（IFC 解析）のオーケストレーションする。IFC ファイルを読み、各要素の parse
//	モジュール（Grid / Story / Member …）を呼んで命令セット（core::Document）を組み立てる。
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
#include "core/Progress.h"

#include <string>

namespace HomeskzIfcImport::parse
{
	// IFC ファイルを解析して命令セットを返す。フェーズ境界は値で返す（例外をフェーズ外へ漏ら
	// さない）。1 要素の欠損で全体を止めず、解決できないものはスキップ・フォールバックで済ま
	// せる。
	//
	// TODO(M7〜): 横架材（M7）以降の要素ごとの parse モジュールを呼び、Document を
	// さらに肉付けする（docs/DEV-NOTES.md）。
	core::Document buildDocument(const std::string& ifcPath);

	// 進捗を報告しながら解析する。読み込みと要素ごとの解析を core/Progress の
	// 2 フェーズ（kLoadShare / kParseShare）として報告する。上のオーバーロードは
	// これを NullProgressReporter で呼ぶだけ（＝振る舞いは同じ）。
	//
	// **中止（cancelled）は見ない。** 解析は大きなホームズ君 IFC でも 0.1 秒程度で
	// 終わり、途中で切り上げる意味が無い（体感時間はすべて描画側にある。
	// core/Progress.h の配分の但し書き参照）。中止は描画側で効かせる。
	core::Document buildDocument(const std::string& ifcPath, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::parse
