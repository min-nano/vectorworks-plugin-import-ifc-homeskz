//
//	draw/ExecuteDocument.cpp
//
//	executeDocument の骨組み実装。Python 版 vw/__init__.py execute_document に対応。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include する。したがって
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の
//	core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	現状は Document を検証するだけの器。以降のマイルストーンで、命令ごとに
//	draw/Grid … draw/Section へディスパッチし、SDK API で実描画する（ROADMAP.md）。
//	実描画（高さ・傾き・スタイル・PIO の挙動）はローカルの VectorWorks で目視確認する。
//

#include "PluginPrefix.h"
#include "draw/ExecuteDocument.h"
#include "core/Document.h"

namespace HomeskzIfcImport::draw
{
	bool executeDocument(const core::Document& document)
	{
		// 検証を通らない Document は描画しない（Python 版 validateDocument と同じ関門）。
		if (!core::validateDocument(document))
			return false;

		// TODO: grid → story → member … と命令ごとに draw モジュールへディスパッチする。
		return true;
	}
} // namespace HomeskzIfcImport::draw
