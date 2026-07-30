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
//	現状は story（M3）→ grid（M1）→ floor（M5）→ rafter → roof（M6）へディスパッチする。
//	残りの要素は対応マイルストーンで足していく。
//

#pragma once

#include "core/Document.h"

namespace HomeskzIfcImport::draw
{
	// 命令セットを描画する。validateDocument を通してから、命令ごとに要素の draw モジュール
	// （story → grid → floor の順）へディスパッチする。戻り値は成功可否で、検証を通らな
	// かったときだけ false（何も描かない）。命令が空でも検証は通るので true を返す。
	//
	// TODO(M6〜): rafter / member … と、要素ごとの draw モジュールへのディスパッチを足す。
	bool executeDocument(const core::Document& document);
} // namespace HomeskzIfcImport::draw
