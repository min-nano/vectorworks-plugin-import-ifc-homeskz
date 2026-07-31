//
//	draw/ExecuteDocument.cpp
//
//	executeDocument の実装。Python 版 vw/__init__.py execute_document に対応。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include する。したがって
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の
//	core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	現状は Document を検証したうえで draw/Story → draw/Grid → draw/Floor →
//	draw/Rafter → draw/Roof へディスパッチする。以降のマイルストーンで draw/Member …
//	draw/Section を足していく（ROADMAP.md）。
//	実描画（高さ・傾き・スタイル・PIO の挙動）はローカルの VectorWorks で目視確認する。
//

#include "PluginPrefix.h"
#include "draw/ExecuteDocument.h"
#include "draw/Floor.h"
#include "draw/Grid.h"
#include "draw/Rafter.h"
#include "draw/Roof.h"
#include "draw/Story.h"
#include "core/Document.h"

namespace HomeskzIfcImport::draw
{
	DrawCounts executeDocument(const core::Document& document)
	{
		DrawCounts counts;

		// 検証を通らない Document は描画しない（Python 版 validateDocument と同じ関門）。
		if (!core::validateDocument(document))
			return counts;
		counts.valid = true;

		// M3 ストーリを先に描く。以降の要素はここで生成したストーリレベル・デザイン
		// レイヤに配置されるため、通り芯や他要素より前に用意する（Python 版 execute_document
		// が execute_stories を先頭で呼ぶのと同じ）。
		counts.stories = drawStories(document);

		// M1 通り芯を描く。
		counts.grids = drawGrids(document);

		// M5 床板を描く。配置先の FL レイヤは上の drawStories が作るので、必ずその後に
		// 置く（レイヤが無い命令は drawFloors がスキップする）。
		counts.floors = drawFloors(document);

		// M6 屋根組を描く。垂木 → 野地板 の順（Python 版 execute_document の実行順と同じで、
		// 野地板は垂木の上に載る）。配置先の "n-垂木" / "n-野地板" レイヤも drawStories が
		// 作るので、必ずその後に置く（レイヤが無い命令はそれぞれがスキップする）。以降の
		// マイルストーンで member / column … と命令ごとに draw モジュールへのディスパッチを
		// 足していく（ROADMAP.md）。
		counts.rafters = drawRafters(document);
		counts.roofs = drawRoofs(document);

		// レイヤのスタック順の並べ替えはここでは行わない（draw/Story.h 参照: VW 2026 ISDK に
		// デザインレイヤの重ね順変更呼び出しが無く、目的の伏図ビューポート重ね順制御は
		// per-viewport の SetViewportLayerStackingOverride を使う M13 へ委ねる。希望順の計算は
		// core::desiredStoryLayerOrder に用意済み）。

		return counts;
	}
} // namespace HomeskzIfcImport::draw
