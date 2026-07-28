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
#include "draw/Grid.h"
#include "draw/Story.h"
#include "core/Document.h"

namespace HomeskzIfcImport::draw
{
	bool executeDocument(const core::Document& document)
	{
		// 検証を通らない Document は描画しない（Python 版 validateDocument と同じ関門）。
		if (!core::validateDocument(document))
			return false;

		// M3 ストーリを先に描く。以降の要素はここで生成したストーリレベル・デザイン
		// レイヤに配置されるため、通り芯や他要素より前に用意する（Python 版 execute_document
		// が execute_stories を先頭で呼ぶのと同じ）。
		drawStories(document);

		// M1 通り芯を描く。以降のマイルストーンで member … と命令ごとに
		// draw モジュールへのディスパッチを足していく（ROADMAP.md）。
		drawGrids(document);

		// レイヤのスタック順の並べ替えはここでは行わない（draw/Story.h 参照: VW 2026 ISDK に
		// デザインレイヤの重ね順変更呼び出しが無く、目的の伏図ビューポート重ね順制御は
		// per-viewport の SetViewportLayerStackingOverride を使う M13 へ委ねる。希望順の計算は
		// core::desiredStoryLayerOrder に用意済み）。

		return true;
	}
} // namespace HomeskzIfcImport::draw
