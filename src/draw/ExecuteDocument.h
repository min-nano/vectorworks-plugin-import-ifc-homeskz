//
//	draw/ExecuteDocument.h
//
//	Phase 2（VW 描画）のディスパッチ。Python 版 vw/__init__.py の execute_document
//	に対応する。命令セット（core::Document）を検証してから、命令ごとに要素の
//	draw モジュール（Grid / Story / Member …）へ振り分けて SDK API で描画する。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため、SDK ビルドでのみコンパイル
//	される（無 SDK の core/parse ライブラリには含めない）。この宣言ヘッダ自体は
//	core::Document しか参照せず、SDK ヘッダを引き込まない。
//
//	いまはフォルダ骨組みとして関数シグネチャだけを置く。実際のディスパッチと
//	要素ごとの描画は対応マイルストーンで実装する（M1 grid から）。
//

#pragma once

#include "core/Document.h"

namespace HomeskzIfcImport::draw
{
	// 命令セットを描画する。validateDocument を通してから、命令ごとに要素の
	// draw モジュールへディスパッチする。戻り値は成功可否（骨組みでは常に成功）。
	//
	// TODO(M1〜): validateDocument → grid/story/member … の順にディスパッチを実装する。
	bool executeDocument(const core::Document& document);
} // namespace HomeskzIfcImport::draw
