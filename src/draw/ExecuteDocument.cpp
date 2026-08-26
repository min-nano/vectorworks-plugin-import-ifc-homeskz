//
//	draw/ExecuteDocument.cpp
//
//	executeDocument の実装。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include する。
//	したがってこの翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の
//	core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	現状は Document を検証したうえで draw/Story → draw/Grid → draw/Footing（立上り・底盤）→
//	draw/Floor → draw/Member → draw/Column → draw/Rafter → draw/Roof → draw/Symbol
//	（アンカーボルト・床束・火打・仕口）→ draw/ColumnMark（記号）→ draw/Sheet（伏図）→
//	draw/Section（軸組図）へディスパッチする。伏図・軸組図のビューポート注釈に載る断面寸法
//	データタグ（draw/Tag）は、それぞれのフェーズの中で置かれる。
//	実描画（高さ・傾き・スタイル・PIO の挙動）はローカルの VectorWorks で目視確認する。
//

#include "PluginPrefix.h"
#include "draw/ExecuteDocument.h"
#include "draw/DrawUtil.h"
#include "draw/Column.h"
#include "draw/ColumnMark.h"
#include "draw/Floor.h"
#include "draw/Footing.h"
#include "draw/Grid.h"
#include "draw/Member.h"
#include "draw/Rafter.h"
#include "draw/Roof.h"
#include "draw/Section.h"
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

	ViewportRefresh markImportedViewportsOutOfDate(const ObjectHandles& viewports)
	{
		ViewportRefresh result;
		result.total = viewports.table().handles.size();
		result.marked = MarkViewportsOutOfDate(viewports);
		return result;
	}

	DrawCounts executeDocument(const core::Document& document, core::ProgressReporter& progress,
							   ObjectHandles* outViewports)
	{
		DrawCounts counts;

		// 検証を通らない Document は描画しない。
		if (!core::validateDocument(document))
			return counts;
		counts.valid = true;

		// 作った伏図・軸組図のビューポートを預ける先。**描き直しはしない**——「更新が要る」印を
		// 立てるのは取り込みが終わり切ってから呼び出し側が行う
		// （draw/ExecuteDocument.h の markImportedViewportsOutOfDate）。
		ObjectHandles ownViewports;
		ObjectHandles& viewports = outViewports != nullptr ? *outViewports : ownViewports;

		// **取り込みの図面変更をまるごと 1 つの undo イベントで包む**（docs/DEV-NOTES.md M15）。
		// VW は取り込みの開始時にイベントを開かないので、ここで自分から開く。構築で開き、
		// 破棄で閉じる RAII なので、途中で例外が出ても・中止されても閉じる。
		// 何が登録され何が戻らないかは draw/DrawUtil.h「取り込み全体の Undo」を参照。
		const ImportUndoScope undoScope;

		// 進捗バーの配分は**実測した 1 件あたりの重さ×件数**で按分する（重さの表と計算は
		// core/Progress。件数比では 1 件 0.1ms のシンボル 472 件がバーを 4 割進め、1 枚 0.5 秒の
		// 軸組図 33 枚が 3% しか進まない、という嘘の進捗になっていた）。
		const double weightedTotal = core::drawWeightedTotal(document);

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
		const auto beginPhase = [&](const char* label, std::size_t count, core::DrawPhase phase)
		{
			if (progress.cancelled())
				return false;
			progress.beginPhase(
				label, core::drawPhaseShare(count, phase, weightedTotal, core::kDrawShare), count);
			return true;
		};

		// **レイヤは「前面に来るものから」作る。** 重ね順は作る順で決まるものとして扱う
		// （draw/Story.cpp の kCreateFrontLayerFirst。並べ替えは取り込み中の描画へ届かない）。
		// 希望順の最上段は通り芯の "共通"、その下が伏図記号レイヤなので、**ストーリのレイヤより
		// 先に**この 2 つを用意する（どちらも描くものが確定している要素のレイヤなので、
		// 「空のレイヤを先に作らない」には反しない）。
		if (!progress.cancelled())
		{
			prepareGridLayer(document);
			preparePlanMarkLayers(document);
		}

		// M3 ストーリを先に描く。以降の要素はここで生成したストーリレベル・デザインレイヤに配
		// 置されるため、通り芯や他要素より前に用意する。
		if (beginPhase("ストーリとレイヤを作成しています…", document.stories.size(),
					   core::DrawPhase::Stories))
			counts.stories = drawStories(document, progress);

		// M1 通り芯を描く。
		if (beginPhase("通り芯を描画しています…", document.grids.size(), core::DrawPhase::Grids))
			counts.grids = drawGrids(document, progress);

		// M3 の【決定】の実装箇所: **デザインレイヤのスタック順を希望順へ並べ替える**（床・
		// 野地板が伏図で柱・梁を覆わないようにする。per-viewport の重ね順上書きは実機で
		// 効かなかった。draw/Story.h の reorderStoryLayers）。
		//
		// **いまは並べ替えそのものが最後の砦**——レイヤは希望順に沿って**作って**あるので
		// （draw/Story の drawStories と kCreateFrontLayerFirst）、ここは普通なら 1 つも
		// 動かさない。動いたら「作る順序の向きが違う」ということなので、診断行に出す。
		//
		// **要素を 1 つも描く前のここで行う。** M13 では「伏図の直前（＝ビューポート生成より
		// 前）なら足りる」と考えて全要素の描画後に置いていたが、実機では取り込み直後の伏図
		// だけが並べ替え前の重ね順で描かれ（図面の並び自体は並べ替え後で、ユーザーが「更新」を
		// 1 回押すと正しくなる）、out-of-date を立てて更新し直しても変わらなかった。並べ替えの
		// 結果が同じ取り込みの中で作るビューポートへ届いていない、ということなので、**届く
		// までの時間を作る**——ここで並べ、以後の全要素の描画を挟んでから伏図を作る。
		//
		// 並べ替えの対象になるにはレイヤが実在していないといけないので、ストーリに属さない
		// 伏図記号レイヤ（"{to}-柱伏図記号"。M12）だけはここで先に用意する。
		if (!progress.cancelled())
		{
			const LayerOrderResult order = reorderStoryLayers(document);
			if (!order.ordered && !document.stories.empty())
				addDiagnostics("レイヤの重ね順を並べ替えられませんでした"
							   "（伏図で床・野地板が柱・梁を覆います）。");
			else if (!order.wasOrdered && !document.stories.empty())
				// **作った順だけでは希望どおりにならなかった**——並べ替えで直してはあるが、
				// その並べ替えは取り込み中の描画へ届かない（draw/Story.h）。作る順序の向き
				// （draw/Story.cpp の kCreateFrontLayerFirst）を見直す材料として出す。
				addDiagnostics(std::string("レイヤの重ね順: 作ったままでは希望どおりに"
										   "なりませんでした（") +
							   (order.wasReversed ? "ちょうど逆順" : "逆順ではない並び") + "・" +
							   std::to_string(order.moved) + " 件を並べ替え）。");
		}

		// M9/M10 基礎を描く。立上り（壁）→ 壁結合 → 底盤（スラブ）の順。**壁結合は立上りの
		// ハンドルを引く**ので、立上りをすべて配置した直後に置く（対応表は WallHandles
		// で受け渡す。draw/Footing.h）。配置先の "F-立上り" / "F-底盤" レイヤは基礎ストーリの
		// story 命令が作るので、必ず drawStories の後に置く（レイヤが無い命令はそれぞれが
		// スキップする）。
		ObjectHandles wallHandles;
		if (beginPhase("基礎の立上りを描画しています…", document.walls.size(),
					   core::DrawPhase::Walls))
			counts.walls = drawWalls(document, progress, &wallHandles);
		if (beginPhase("基礎の立上りを結合しています…", document.wallJoins.size(),
					   core::DrawPhase::WallJoins))
		{
			std::string note;
			counts.wallJoins = drawWallJoins(document, progress, wallHandles, &note);
			addDiagnostics(note);
		}
		if (beginPhase("基礎の底盤を描画しています…", document.slabs.size(),
					   core::DrawPhase::Slabs))
			counts.slabs = drawSlabs(document, progress);

		// M5 床板を描く。配置先の FL レイヤは上の drawStories が作るので、必ずその後に
		// 置く（レイヤが無い命令は drawFloors がスキップする）。
		if (beginPhase("床を描画しています…", document.floors.size(), core::DrawPhase::Floors))
			counts.floors = drawFloors(document, progress);

		// M7 横架材を描く。配置先の "n-横架材天端" / "R-軒高" / "n-母屋" / "n-登り梁" レイヤは
		// drawStories が作るので、必ずその後に置く（レイヤが無い命令はスキップされる）。
		// **横架材ハンドルを記録する**——伏図・軸組図の断面寸法データタグがこれを関連付け先
		// として引く（立上り → 壁結合と同じ受け渡し方式。draw/ObjectHandles.h）。
		ObjectHandles memberHandles;
		if (beginPhase("横架材を描画しています…", document.members.size(),
					   core::DrawPhase::Members))
		{
			std::string note;
			counts.members = drawMembers(document, progress, &note, &memberHandles);
			addDiagnostics(note);
		}

		// M8 柱を描く。配置先の span レイヤ（"1to2-柱" 等）も drawStories が作るので、必ず
		// その後に置く（レイヤが無い命令はスキップされる）。横架材の後なのは、柱が横架材と
		// 同じ構造材ツール／同じスタイル更新の作法を採るため揃えているだけで依存は無い。
		// **柱ハンドルを記録する**——伏図記号のデータタグがこれを関連付け先として引く
		// （立上り → 壁結合と同じ受け渡し方式。draw/ObjectHandles.h）。
		ObjectHandles columnHandles;
		if (beginPhase("柱を描画しています…", document.columns.size(), core::DrawPhase::Columns))
		{
			std::string note;
			counts.columns = drawColumns(document, progress, &note, &columnHandles);
			addDiagnostics(note);
		}

		// M6 屋根組を描く。垂木 → 野地板 の順。配置先の "n-垂木" / "n-野地板" レイヤも
		// drawStories が作るので、必ずその後に置く（レイヤが無い命令はそれぞれがスキップする）。
		// 以降のマイルストーンで footing … と命令ごとに draw モジュールへのディスパッチを足し
		// ていく（docs/DEV-NOTES.md）。
		if (beginPhase("垂木を描画しています…", document.rafters.size(), core::DrawPhase::Rafters))
			counts.rafters = drawRafters(document, progress);
		if (beginPhase("野地板を描画しています…", document.roofs.size(), core::DrawPhase::Roofs))
			counts.roofs = drawRoofs(document, progress);

		// M11 シンボル置換系。4 種とも同じ描画（draw/Symbol）で、違いは配置先レイヤと
		// シンボル名だけ。配置先（アンカーボルト・床束＝基礎ストーリの "F-…"、火打・仕口＝
		// 横架材と同じレイヤ）はいずれも drawStories が作るので、必ずその後に置く
		// （レイヤが無い命令はスキップされ、その件数が診断行に出る）。
		const auto drawSymbolPhase = [&](const char* label, const char* elementLabel,
										 const std::vector<core::SymbolCommand>& commands,
										 core::DrawPhase phase, std::size_t& out)
		{
			if (!beginPhase(label, commands.size(), phase))
				return;
			std::string note;
			out = drawSymbols(commands, progress, elementLabel, &note);
			addDiagnostics(note);
		};
		drawSymbolPhase("アンカーボルトを配置しています…", "アンカーボルト", document.anchorBolts,
						core::DrawPhase::AnchorBolts, counts.anchorBolts);
		drawSymbolPhase("床束を配置しています…", "床束", document.floorPosts,
						core::DrawPhase::FloorPosts, counts.floorPosts);
		drawSymbolPhase("火打を配置しています…", "火打", document.fireBraces,
						core::DrawPhase::FireBraces, counts.fireBraces);
		drawSymbolPhase("仕口を配置しています…", "仕口", document.joints, core::DrawPhase::Joints,
						counts.joints);

		// M12 断面記号・伏図記号。**柱の後**に置く: 記号 PIO はリセット時に対象レイヤの
		// 構造材を検索するので、柱が置かれていないと記号 0 個で確定してしまう。かつ
		// **レイヤの並べ替えより前**に置く: 伏図記号レイヤ（"{to}-柱伏図記号"）はここで
		// 作られるので、希望順へ並べるときに実在していないといけない。
		if (beginPhase("柱記号を配置しています…", document.columnMarks.size(),
					   core::DrawPhase::ColumnMarks))
		{
			std::string note;
			counts.columnMarks = drawColumnMarks(document, progress, &note);
			addDiagnostics(note);
		}

		// 重ね順の**確かめ直し**。並べ替え自体は通り芯の直後で済ませてあるので、ここは
		// 「描画の途中で並びが崩れていないか」を見るだけ——崩れていなければ 1 つも動かさない
		// （reorderStoryLayers は既に希望どおりなら図面を触らない）。記号レイヤはここまでに
		// 記号が置かれて実在するので、前倒しで作れていなかった場合もここで拾える。
		if (!progress.cancelled())
		{
			const LayerOrderResult order = reorderStoryLayers(document);
			if (!order.ordered && !document.stories.empty())
				addDiagnostics("レイヤの重ね順を並べ替えられませんでした"
							   "（伏図で床・野地板が柱・梁を覆います）。");
			else if (order.moved > 0)
				addDiagnostics("レイヤの重ね順が描画中に崩れたので並べ直しました（" +
							   std::to_string(order.moved) + " 件）。");
		}

		// M13 シート（伏図）。**必ず最後**に置く: ビューポートはデザインレイヤ（＝ここまでに
		// 描いたモデル）を映すので、全要素の描画が済んでいないと空の図になる。表示レイヤの
		// 絞り込みも、対象のレイヤが揃っていて初めて効く（draw/Sheet.h）。
		if (beginPhase("伏図を作成しています…", document.sheets.size(), core::DrawPhase::Sheets))
		{
			std::string note;
			counts.sheets = drawSheets(document, progress, &note, &memberHandles, &viewports);
			addDiagnostics(note);
		}

		// M14 軸組図（断面ビューポート）。**伏図の後**に置く: どちらもモデルを映すので全要素
		// の描画が済んでいる必要があり、シートレイヤの番号も伏図（"1" / "2" …）の後に "A"
		// が続く並びになる。
		if (beginPhase("軸組図を作成しています…", document.sections.size(),
					   core::DrawPhase::Sections))
		{
			std::string note;
			// 軸組図のハンドルも同じ表へ預ける。**鍵は伏図の枚数ぶんずらす**（どちらも命令
			// インデックスを鍵にするので、そのままだとぶつかる）。
			ObjectHandles sectionViewports;
			counts.sections =
				drawSections(document, progress, &note, &memberHandles, &sectionViewports);
			for (const auto& [index, viewport] : sectionViewports.table().handles)
				viewports.table().handles.emplace(document.sheets.size() + index, viewport);
			addDiagnostics(note);
		}

		// 途中で中止されたか（件数が命令数に届かないのが正常になる）。
		counts.cancelled = progress.cancelled();

		// 「取り消し」で取り込みを戻せる状態にできたか（完了ダイアログがこれを伝える）。
		// armed=false は「登録できるレイヤが 1 つも無かった」＝取り込み前から在るレイヤへ
		// だけ描いた場合で、そのときイベントはスコープの破棄で捨てられる。
		counts.undoArmed = undoScope.armed();
		counts.undoPartial = undoScope.partial();

		// **図は描かない。** ビューポートは描画キャッシュを持たないまま残し、「更新が要る」印を
		// 立てるのは取り込みが終わり切ってから（undo イベントも進捗ダイアログも閉じた後）
		// 呼び出し側が行う（markImportedViewportsOutOfDate）。実際に描くのは VW。
		return counts;
	}
} // namespace HomeskzIfcImport::draw
