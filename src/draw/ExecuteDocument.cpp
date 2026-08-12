//
//	draw/ExecuteDocument.cpp
//
//	executeDocument の実装。Python 版 vw/__init__.py execute_document に対応。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include する。したがって
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の
//	core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	現状は Document を検証したうえで draw/Story → draw/Grid → draw/Footing（立上り・底盤）→
//	draw/Floor → draw/Member → draw/Column → draw/Rafter → draw/Roof → draw/Symbol
//	（アンカーボルト・床束・火打・仕口）→ draw/Sheet（伏図）へディスパッチする。以降の
//	マイルストーンで draw/ColumnMark … draw/Section を足していく（ROADMAP.md）。
//	実描画（高さ・傾き・スタイル・PIO の挙動）はローカルの VectorWorks で目視確認する。
//

#include "PluginPrefix.h"
#include "draw/ExecuteDocument.h"
#include "draw/Column.h"
#include "draw/ColumnMark.h"
#include "draw/Floor.h"
#include "draw/Footing.h"
#include "draw/Grid.h"
#include "draw/Member.h"
#include "draw/Rafter.h"
#include "draw/Roof.h"
#include "draw/Sheet.h"
#include "draw/Story.h"
#include "draw/Symbol.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	DrawCounts executeDocument(const core::Document& document)
	{
		// 進捗を表示しない呼び出し（従来の呼び出し口）。振る舞いは同じ。
		core::NullProgressReporter noProgress;
		return executeDocument(document, noProgress);
	}

	DrawCounts executeDocument(const core::Document& document, core::ProgressReporter& progress)
	{
		DrawCounts counts;

		// 検証を通らない Document は描画しない（Python 版 validateDocument と同じ関門）。
		if (!core::validateDocument(document))
			return counts;
		counts.valid = true;

		// 進捗バーの配分は**命令数の比**にする（1 件あたりの重さは要素で違うが、要素ごとの
		// 実測が無い以上、件数比が一番嘘の少ない近似。core::phaseShare）。
		const std::size_t total =
			document.stories.size() + document.grids.size() + document.floors.size() +
			document.members.size() + document.columns.size() + document.rafters.size() +
			document.roofs.size() + document.walls.size() + document.wallJoins.size() +
			document.slabs.size() + document.anchorBolts.size() + document.floorPosts.size() +
			document.fireBraces.size() + document.joints.size() + document.columnMarks.size() +
			document.sheets.size();

		// 要素ごとの診断（無ければ空）を改行で連ねる。1 つの文字列を各 draw* へ渡すと
		// 後の要素が前の要素の診断を上書きしてしまうため、ここで積む。
		const auto addDiagnostics = [&counts](const std::string& note)
		{
			if (note.empty())
				return;
			if (!counts.diagnostics.empty())
				counts.diagnostics += "\n";
			counts.diagnostics += note;
		};

		// フェーズを開く。中止済みなら false を返し、呼び出し側はそのフェーズごと飛ばす
		// （各 draw* も自分のループの先頭で中止を見て抜けるので、途中で押されても止まる）。
		const auto beginPhase = [&](const char* label, std::size_t count)
		{
			if (progress.cancelled())
				return false;
			progress.beginPhase(label, core::phaseShare(count, total, core::kDrawShare), count);
			return true;
		};

		// M3 ストーリを先に描く。以降の要素はここで生成したストーリレベル・デザイン
		// レイヤに配置されるため、通り芯や他要素より前に用意する（Python 版 execute_document
		// が execute_stories を先頭で呼ぶのと同じ）。
		if (beginPhase("ストーリとレイヤを作成しています…", document.stories.size()))
			counts.stories = drawStories(document, progress);

		// M1 通り芯を描く。
		if (beginPhase("通り芯を描画しています…", document.grids.size()))
			counts.grids = drawGrids(document, progress);

		// M9/M10 基礎を描く。立上り（壁）→ 壁結合 → 底盤（スラブ）の順（Python 版
		// execute_document と同じ）。**壁結合は立上りのハンドルを引く**ので、立上りを
		// すべて配置した直後に置く（対応表は WallHandles で受け渡す。draw/Footing.h）。
		// 配置先の "F-立上り" / "F-底盤" レイヤは基礎ストーリの story 命令が作るので、必ず
		// drawStories の後に置く（レイヤが無い命令はそれぞれがスキップする）。
		ObjectHandles wallHandles;
		if (beginPhase("基礎の立上りを描画しています…", document.walls.size()))
			counts.walls = drawWalls(document, progress, &wallHandles);
		if (beginPhase("基礎の立上りを結合しています…", document.wallJoins.size()))
		{
			std::string note;
			counts.wallJoins = drawWallJoins(document, progress, wallHandles, &note);
			addDiagnostics(note);
		}
		if (beginPhase("基礎の底盤を描画しています…", document.slabs.size()))
			counts.slabs = drawSlabs(document, progress);

		// M5 床板を描く。配置先の FL レイヤは上の drawStories が作るので、必ずその後に
		// 置く（レイヤが無い命令は drawFloors がスキップする）。
		if (beginPhase("床を描画しています…", document.floors.size()))
			counts.floors = drawFloors(document, progress);

		// M7 横架材を描く。配置先の "n-横架材天端" / "R-軒高" / "n-母屋" / "n-登り梁" レイヤは
		// drawStories が作るので、必ずその後に置く（レイヤが無い命令はスキップされる）。
		if (beginPhase("横架材を描画しています…", document.members.size()))
		{
			std::string note;
			counts.members = drawMembers(document, progress, &note);
			addDiagnostics(note);
		}

		// M8 柱を描く。配置先の span レイヤ（"1to2-柱" 等）も drawStories が作るので、必ず
		// その後に置く（レイヤが無い命令はスキップされる）。横架材の後なのは、柱が横架材と
		// 同じ構造材ツール／同じスタイル更新の作法を採るため揃えているだけで依存は無い。
		// **柱ハンドルを記録する**——伏図記号のデータタグがこれを関連付け先として引く
		// （立上り → 壁結合と同じ受け渡し方式。draw/ObjectHandles.h）。
		ObjectHandles columnHandles;
		if (beginPhase("柱を描画しています…", document.columns.size()))
		{
			std::string note;
			counts.columns = drawColumns(document, progress, &note, &columnHandles);
			addDiagnostics(note);
		}

		// M6 屋根組を描く。垂木 → 野地板 の順（Python 版 execute_document の実行順と同じで、
		// 野地板は垂木の上に載る）。配置先の "n-垂木" / "n-野地板" レイヤも drawStories が
		// 作るので、必ずその後に置く（レイヤが無い命令はそれぞれがスキップする）。以降の
		// マイルストーンで footing … と命令ごとに draw モジュールへのディスパッチを
		// 足していく（ROADMAP.md）。
		if (beginPhase("垂木を描画しています…", document.rafters.size()))
			counts.rafters = drawRafters(document, progress);
		if (beginPhase("野地板を描画しています…", document.roofs.size()))
			counts.roofs = drawRoofs(document, progress);

		// M11 シンボル置換系。4 種とも同じ描画（draw/Symbol）で、違いは配置先レイヤと
		// シンボル名だけ。配置先（アンカーボルト・床束＝基礎ストーリの "F-…"、火打・仕口＝
		// 横架材と同じレイヤ）はいずれも drawStories が作るので、必ずその後に置く
		// （レイヤが無い命令はスキップされ、その件数が診断行に出る）。
		const auto drawSymbolPhase = [&](const char* label, const char* elementLabel,
										 const std::vector<core::SymbolCommand>& commands,
										 std::size_t& out)
		{
			if (!beginPhase(label, commands.size()))
				return;
			std::string note;
			out = drawSymbols(commands, progress, elementLabel, &note);
			addDiagnostics(note);
		};
		drawSymbolPhase("アンカーボルトを配置しています…", "アンカーボルト", document.anchorBolts,
						counts.anchorBolts);
		drawSymbolPhase("床束を配置しています…", "床束", document.floorPosts, counts.floorPosts);
		drawSymbolPhase("火打を配置しています…", "火打", document.fireBraces, counts.fireBraces);
		drawSymbolPhase("仕口を配置しています…", "仕口", document.joints, counts.joints);

		// M12 断面記号・伏図記号。**柱の後**に置く: 記号 PIO はリセット時に対象レイヤの
		// 構造材を検索するので、柱が置かれていないと記号 0 個で確定してしまう。かつ
		// **レイヤの並べ替えより前**に置く: 伏図記号レイヤ（"{to}-柱伏図記号"）はここで
		// 作られるので、希望順へ並べるときに実在していないといけない。
		if (beginPhase("柱記号を配置しています…", document.columnMarks.size()))
		{
			std::string note;
			counts.columnMarks = drawColumnMarks(document, progress, &note);
			addDiagnostics(note);
		}

		// M3 の【決定】の実装箇所（M13 で確定）: **デザインレイヤのスタック順を希望順へ
		// 並べ替える**。伏図ビューポートはドキュメントの重ね順で描かれるので、床・野地板が
		// 柱・梁を覆わないようにするにはここで並べ替えるしかない（per-viewport の重ね順
		// 上書きは実機で効かなかった。draw/Story.h の reorderStoryLayers）。**必ず伏図より
		// 前**に行う——ビューポートは生成時の重ね順で描かれるため。
		if (!progress.cancelled())
		{
			// 並べ替えは図面から効いたか分かる（動かせたレイヤ数）。0 件なら伏図で床・野地板が
			// 柱・梁を覆うので、原因の切り分け材料として診断行に出す。
			const std::size_t reordered = reorderStoryLayers(document);
			if (reordered == 0 && !document.stories.empty())
				addDiagnostics("レイヤの重ね順を並べ替えられませんでした（0 件）。");
		}

		// M13 シート（伏図）。**必ず最後**に置く: ビューポートはデザインレイヤ（＝ここまでに
		// 描いたモデル）を映すので、全要素の描画が済んでいないと空の図になる。表示レイヤの
		// 絞り込みも、対象のレイヤが揃っていて初めて効く（draw/Sheet.h）。
		if (beginPhase("伏図を作成しています…", document.sheets.size()))
		{
			std::string note;
			counts.sheets = drawSheets(document, progress, &note);
			addDiagnostics(note);
		}

		// 途中で中止されたか（件数が命令数に届かないのが正常になる）。
		counts.cancelled = progress.cancelled();

		return counts;
	}
} // namespace HomeskzIfcImport::draw
