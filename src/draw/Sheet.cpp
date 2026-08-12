//
//	draw/Sheet.cpp
//
//	シート（伏図）描画の実装。Python 版 vw/sheet.py の execute_sheets / draw_sheet /
//	configure_viewport_layers / configure_viewport_scale に対応する。【SDK 依存】
//	PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位はプラグイン
//	ビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API は ISDK（gSDK）／VWFC の実在シグネチャに合わせている
//	（Vectorworks 2026 SDK。ci-debug の sdk-grep / shell で確認済み）:
//	  * gSDK->GetNamedLayer(name) / gSDK->CreateLayer(name, layerType)
//	                                                  … シートレイヤの取得・生成
//	  * VWLayerObj::IsLayerObject(handle)             … 走査中のオブジェクトがレイヤか
//	  * VWLayerObj(handle).SetDescription(title)      … シートレイヤのタイトル
//	                                                    （オブジェクト変数 159 と同じもの）
//	  * VWLayerObj(handle).GetScale()                 … デザインレイヤの縮尺
//	  * VWDocument::GetDrawingHeaderFristMember()     … 図面のオブジェクト列の先頭（＝最初の
//	                                                    レイヤ。SDK の綴りママ）
//	  * gSDK->NextObject(handle)                      … 次のレイヤへ
//	  * gSDK->CreateViewport(sheetLayer)              … ビューポート生成
//	  * gSDK->SetViewportLayerVisibility(vp, layer, v)… 表示レイヤの絞り込み（0=表示/1=非表示）
//	  * gSDK->FirstMemberObj(h) / gSDK->GetObjectClass(h)
//	                                                  … 図形が使うクラスの数え上げ（下記）
//	  * gSDK->ClassNameToID(name)                     … クラス名 → InternalIndex
//	  * gSDK->SetViewportClassVisibility(vp, idx, 0)  … クラス表示（既定は非表示。下記）
//	  * gSDK->GetObjectInternalIndex(layer)           … 重ね順上書きが要る InternalIndex
//	  * gSDK->SetViewportLayerStackingOverride(vp, idx, pos) /
//	    gSDK->GetNumViewportLayerStackingOverrides(vp)
//	                                                  … per-viewport のレイヤ重ね順上書きと確認
//	  * VWViewportObj(vp).SetScale(scale)             … ビューポート縮尺（オブジェクト変数 1003）
//	  * VWViewportObj(vp).SetDescription(title)       … 図面タイトル（同 1032・Dwg Title）
//	  * VWViewportObj(vp).SetLocator(number)          … 図番（同 1033・Item）
//	  * VWViewportObj(vp).Update()                    … 描画更新（gSDK->UpdateViewport と同じ）
//
//	【表示レイヤの絞り込みは「全部隠してから挙げたものだけ出す」】ビューポートは既定で
//	ドキュメントの表示状態を引き継ぐので、命令に挙げていないレイヤが映り込む。Python 版と
//	同じく、まず全レイヤを**非表示**にしてから命令の layers を名前で引いて表示へ戻す
//	（グレー表示 2 を使うと薄く残ってしまうので、必ず 1＝非表示）。
//
//	【クラスは 1 つずつ表示に戻す】レイヤと違い、**ビューポートのクラスは明示しないと非表示の
//	まま**（ローカル確認で判明）。しかも ISDK にドキュメントの全クラスを列挙する呼び出しが
//	無いので、**図形が身に付けているクラスを全レイヤ走査で数え上げて**（CollectUsedClasses。
//	PIO / シンボルの中まで辿る）表示に戻す。クラスで絞る伏図はまだ無い（命令に
//	hidden_classes を持たせていない。core/Document.h の ViewportCommand 参照）。
//
//	【重ね順は per-viewport 上書き】希望順（core::desiredStoryLayerOrder）を前面→背面の
//	並びとして各ビューポートへ与える（draw/Sheet.h の設計メモ参照）。デザインレイヤ自体の
//	並びには触らない。
//

#include "PluginPrefix.h"
#include "draw/Sheet.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWDocument.h"
#include "VWFC/VWObjects/VWLayerObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// CreateLayer の layerType。SDK の ELayerType（Kernel/API/MiniCadCallBacks.h）が
		// kLayerDesign=1 / kLayerSheet=2 と定めており、draw/DrawUtil の kDesignLayerType=1 と
		// 対になる（Python 版 vw/sheet.py も 2 を渡す）。列挙名を直に使わないのは
		// kDesignLayerType と同じ流儀に揃えるため。
		constexpr short kSheetLayerType = 2;

		// SetViewportLayerVisibility の表示種別。**2（グレー）は使わない**——対象外のレイヤを
		// グレーにすると伏図に薄く残る（Python 版 vw/sheet.py の注記と同じ）。
		constexpr short kLayerVisible = 0;
		constexpr short kLayerHidden = 1;

		// SetViewportClassVisibility の表示種別。SDK の EClassVisibility（VWFC/VWObjects/
		// VWClass.h）が Normal=0 / Invisible=-1 / Grayed=2 と定めており、**VS の 0/1/2 とは
		// 値が違う**（1 は「非表示」ではない）。表示に戻すのが目的なので Normal だけを使う。
		constexpr short kClassVisible = 0;

		// 図面のレイヤを先頭から順に辿る。VWDocument::GetDrawingHeaderFristMember（SDK の
		// 綴りママ）が図面のオブジェクト列の先頭＝最初のレイヤで、以降は NextObject でたどれる。
		// レイヤ以外が混ざっても IsLayerObject で弾く（ISDK に「レイヤだけを列挙する」呼び出しは
		// 無いため、この走査が唯一の手立て）。
		std::vector<MCObjectHandle> AllLayers()
		{
			std::vector<MCObjectHandle> layers;
			try
			{
				for (MCObjectHandle h = VWDocument::GetDrawingHeaderFristMember(); h != nil;
					 h = gSDK->NextObject(h))
				{
					if (VWLayerObj::IsLayerObject(h))
						layers.push_back(h);
				}
			}
			catch (...)
			{
				// 走査中の異常で伏図全体を落とさない（CLAUDE.md「エラーハンドリング」）。
				// そこまでに拾えたレイヤだけを返す（絞り込みの取りこぼしは、そのレイヤが
				// 伏図に映り込むだけで済む）。
				return layers;
			}
			return layers;
		}

		// シートレイヤを用意する（同じ番号のものがあれば再利用）。**シートレイヤ番号は
		// レイヤ名が担う**（Python 版と同じ）。タイトルはレイヤの説明＝オブジェクト変数 159
		// （ovLayerDescription。"only used for sheet layers"）へ入れる。用意できなければ nil。
		MCObjectHandle PrepareSheetLayer(const core::SheetCommand& command)
		{
			const TXString name(command.number.c_str());
			MCObjectHandle layer = gSDK->GetNamedLayer(name);
			if (layer == nil)
				layer = gSDK->CreateLayer(name, kSheetLayerType);
			if (layer == nil)
				return nil;
			try
			{
				VWLayerObj sheet(layer);
				sheet.SetDescription(TXString(command.title.c_str()));
			}
			catch (...)
			{
				// タイトルが付かなくても図は描ける（1 つの失敗で全体を止めない）ので、
				// レイヤはそのまま返す。
				return layer;
			}
			return layer;
		}

		// ビューポートの表示レイヤを命令どおりに絞る。まず全レイヤを非表示にし（親シート
		// レイヤ自身は対象外）、命令の layers を名前で引いて表示へ戻す。**存在しないレイヤ名は
		// 黙って読み飛ばす**（要素の描画がスキップされてレイヤが無い場合など。伏図自体は残す）。
		void ConfigureViewportLayers(MCObjectHandle viewport, MCObjectHandle sheetLayer,
									 const std::vector<MCObjectHandle>& allLayers,
									 const core::ViewportCommand& command)
		{
			for (const MCObjectHandle layer : allLayers)
			{
				if (layer == sheetLayer)
					continue;
				gSDK->SetViewportLayerVisibility(viewport, layer, kLayerHidden);
			}
			for (const std::string& name : command.layers)
			{
				const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
				if (layer != nil)
					gSDK->SetViewportLayerVisibility(viewport, layer, kLayerVisible);
			}
		}

		// クラス表示に渡す「図面で実際に使われているクラス」の索引を集める。
		//
		// 【なぜ数え上げるのか】ISDK には**ドキュメントの全クラスを列挙する呼び出しが無い**
		// （VWClass にあるのは名前↔索引の変換と妥当性判定だけ。BuildResourceList にクラス用の
		// 型も無い。ci-debug で確認）。そこで**図形が身に付けているクラス**を全レイヤ走査で
		// 数え上げる。**コンテナは中まで辿る**のが要点で、通り芯（GridAxis PIO）のラベルの
		// ように「PIO / シンボルの中の図形が、スタイルの決めたクラスを持つ」ものは、外側の
		// オブジェクトのクラスだけ見ても拾えない（ラベルだけ消える。ローカル確認で判明）。
		//
		// 走査は 1 回だけ行い、全ビューポートで同じ集合を使う（伏図ごとに全図形を辿り直すと
		// 図面の規模なりに重くなる）。
		std::set<InternalIndex> CollectUsedClasses(const std::vector<MCObjectHandle>& layers)
		{
			std::set<InternalIndex> classes;
			// 入れ子（グループ・シンボル・PIO）は深さ上限つきで辿る。上限は「PIO の中の
			// グループの中の図形」に十分で、壊れたデータで無限に潜らない値。
			constexpr int kMaxDepth = 6;
			std::vector<std::pair<MCObjectHandle, int>> pending;
			pending.reserve(layers.size());
			for (const MCObjectHandle layer : layers)
			{
				const MCObjectHandle first = gSDK->FirstMemberObj(layer);
				if (first != nil)
					pending.emplace_back(first, 0);
			}
			while (!pending.empty())
			{
				const auto [head, depth] = pending.back();
				pending.pop_back();
				for (MCObjectHandle h = head; h != nil; h = gSDK->NextObject(h))
				{
					const InternalIndex index = gSDK->GetObjectClass(h);
					if (index != 0)
						classes.insert(index);
					if (depth >= kMaxDepth)
						continue;
					const MCObjectHandle child = gSDK->FirstMemberObj(h);
					if (child != nil)
						pending.emplace_back(child, depth + 1);
				}
			}
			return classes;
		}

		// ビューポートのクラス表示を「全部表示」にする。**設定した数**を返す。
		//
		// 【なぜ要るか】**ビューポートはクラスの表示を明示しないと非表示のまま**（ローカル
		// 確認で判明: レイヤは命令どおりなのにクラスが全て非表示で、明示したクラスだけが
		// 表示になった）。Python 版 vw/sheet.py の configure_viewport_classes が全クラスを
		// 表示へ戻していたのと同じ手当てが要る。
		//
		// ［やってはいけないこと］`SetUseDocumentClassVis(true)`（オブジェクト変数 1031）で
		// ドキュメントのクラス表示に従わせる手は**シートレイヤのビューポートには効かない**
		// ——ObjectVariables.h の但し書きが "for dlvps"（＝デザインレイヤビューポート専用）で、
		// 実際 1 回目の修正はこれを呼んでも全クラス非表示のままだった。クラスは 1 つずつ
		// 表示にするしかない。
		std::size_t ConfigureViewportClasses(MCObjectHandle viewport,
											 const std::set<InternalIndex>& classes)
		{
			std::size_t applied = 0;
			for (const InternalIndex index : classes)
			{
				if (gSDK->SetViewportClassVisibility(viewport, index, kClassVisible))
					++applied;
			}
			return applied;
		}

		// レイヤの重ね順をこのビューポートだけで上書きする（M3 の【決定】の実装箇所。
		// draw/Sheet.h 参照）。**設定できた数**を返す（0 なら上書きが効いていない＝診断へ）。
		//
		// order は前面→背面の希望順で、**そのビューポートが表示するレイヤだけ**へ 0 から
		// 詰めた位置を与える。非表示のレイヤまで並べても意味が無いうえ、VW 側が受け付けない
		// 可能性がある（重ね順はビューポートに映るレイヤの間の話）。希望順に無いレイヤ
		// （ユーザーが別途作ったもの）は触らず既定の重ね順のまま残す。
		//
		// ［1 回目の修正で外したこと］以前は先に `CreateViewportLayerOverride` を呼んでいたが、
		// これは**別系統**（SViewportLayerOverride＝レイヤの描画属性の上書き）で、重ね順とは
		// 関係が無い（重ね順は Get/Set/RemoveViewportLayerStackingOverride の 3 つだけで、
		// Create に当たるものは無い）。
		std::size_t ApplyLayerStacking(MCObjectHandle viewport,
									   const std::vector<std::string>& order,
									   const std::vector<std::string>& shown)
		{
			std::size_t applied = 0;
			Sint32 position = 0;
			for (const std::string& name : order)
			{
				if (std::ranges::find(shown, name) == shown.end())
					continue;
				const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
				if (layer == nil)
					continue;
				const InternalIndex index = gSDK->GetObjectInternalIndex(layer);
				if (gSDK->SetViewportLayerStackingOverride(viewport, index, position))
					++applied;
				++position;
			}
			return applied;
		}

		// 表示するデザインレイヤの縮尺を返す（Python 版 configure_viewport_scale）。伏図が映す
		// レイヤの縮尺は揃っているので、最初に取れたものを採る。取れなければ 0（＝ビューポートの
		// 既定縮尺のままにする）。
		double LayerScaleFor(const core::ViewportCommand& command)
		{
			for (const std::string& name : command.layers)
			{
				const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
				if (layer == nil)
					continue;
				try
				{
					const VWLayerObj design(layer);
					const double scale = design.GetScale();
					if (scale > 0.0)
						return scale;
				}
				catch (...)
				{
					// このレイヤからは縮尺を取れなかった。次の候補を見る。
					continue;
				}
			}
			return 0.0;
		}

		// 図面タイトル・図番・縮尺を設定して描画を更新する。VWViewportObj のこれらは
		// オブジェクト変数 1032（Dwg Title）/ 1033（Item）/ 1003（縮尺）に対応する。
		// 設定に失敗しても図そのものは残す（1 つの失敗で全体を止めない）。
		void FinishViewport(MCObjectHandle viewport, const core::ViewportCommand& command,
							double scale)
		{
			try
			{
				VWViewportObj vp(viewport);
				if (scale > 0.0)
					vp.SetScale(scale);
				vp.SetDescription(TXString(command.drawingTitle.c_str()));
				vp.SetLocator(TXString(command.drawingNumber.c_str()));
				vp.Update();
			}
			catch (...)
			{
				// ラベル・縮尺が付かなくてもビューポートは図面に残るので、ここで戻る。
				return;
			}
		}
	} // namespace

	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note)
	{
		const std::vector<core::SheetCommand>& commands = document.sheets;
		if (commands.empty())
			return 0;

		// レイヤの走査と希望スタック順は全シートで共通なので 1 回だけ求める。希望順の計算は
		// SDK 非依存（core::desiredStoryLayerOrder。無 SDK テスト済み）で、**伏図記号レイヤ
		// 等のストーリ非依存レイヤ（topLayers）は M12 が着地したら渡す**。
		const std::vector<MCObjectHandle> allLayers = AllLayers();
		const std::vector<std::string> stacking = core::desiredStoryLayerOrder(document.stories);
		// 表示に戻すクラス（全ビューポートで同じ）。**図形が使っているクラス**（走査で数え
		// 上げ。通り芯ラベルのように PIO の中でスタイルが決めるクラスもここで拾う）に、
		// **命令セットが名乗るクラス**（core::documentClassNames。まだ図形が無いクラスや、
		// 走査で辿れなかったものの取りこぼしを防ぐ保険）を足す。
		std::set<InternalIndex> classes = CollectUsedClasses(allLayers);
		for (const std::string& name : core::documentClassNames(document))
		{
			const InternalIndex index = gSDK->ClassNameToID(TXString(name.c_str()));
			if (index != 0)
				classes.insert(index);
		}

		// 描画の前後でカレントレイヤが変わると以降のフェーズ（M14 以降）に響くので、
		// 元のレイヤへ戻せるよう控えておく。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;
		// 重ね順の上書きは**効いたかどうかを図面から読み戻せる**ので、設定できた数と VW が
		// 実際に持っている数の両方を数えて診断に出す（ローカル確認で「順序を上書き: いいえ」
		// のままだったため、次の確認で切り分けられるようにする）。
		std::size_t stackingApplied = 0;
		std::size_t stackingRecorded = 0;
		// クラス表示も同じく「設定できた数」を数える（0 なら図形が 1 つも映らない）。
		std::size_t classesApplied = 0;

		for (const core::SheetCommand& command : commands)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は枚数で報告し、
			// 描画の前に 1 件進める（＝「いま何枚目を作っているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			const MCObjectHandle sheetLayer = PrepareSheetLayer(command);
			if (sheetLayer == nil)
			{
				++missingSheetLayers;
				continue;
			}

			const MCObjectHandle viewport = gSDK->CreateViewport(sheetLayer);
			if (viewport == nil)
			{
				// シートレイヤは残る（＝図面に空のシートができる）。件数を診断へ残して
				// 「シートはあるのに図が無い」原因が描画側だと分かるようにする。
				++missingViewports;
				continue;
			}

			ConfigureViewportLayers(viewport, sheetLayer, allLayers, command.viewport);
			classesApplied += ConfigureViewportClasses(viewport, classes);
			stackingApplied += ApplyLayerStacking(viewport, stacking, command.viewport.layers);
			stackingRecorded += gSDK->GetNumViewportLayerStackingOverrides(viewport);
			// **縮尺は表示レイヤを絞った後に読む**（映すレイヤの縮尺に合わせるため）。
			FinishViewport(viewport, command.viewport, LayerScaleFor(command.viewport));
			++drawn;
		}

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		// 診断行（何も無ければ空のまま）。「命令はあるのに 0 枚」のときに、シートレイヤを
		// 作れないのか、ビューポートを作れないのかを切り分けられる。**重ね順は図面から
		// 読み戻せる**ので、1 件も記録されていなければそれも出す（床・野地板が柱・梁を覆う
		// 見え方になる原因が、命令ではなく VW 側の受け付けだと分かる）。
		const bool stackingBroken = drawn > 0 && stackingRecorded == 0;
		const bool classesBroken = drawn > 0 && classesApplied == 0;
		if (note != nullptr &&
			(missingSheetLayers > 0 || missingViewports > 0 || stackingBroken || classesBroken))
		{
			std::string text = "伏図の診断: ";
			if (missingSheetLayers > 0)
				text += "シートレイヤを作れなかった命令 " + std::to_string(missingSheetLayers) +
						" 件。";
			if (missingViewports > 0)
				text +=
					"ビューポートを作れなかった命令 " + std::to_string(missingViewports) + " 件。";
			if (classesBroken)
				text += "クラスを表示に戻せませんでした（対象 " + std::to_string(classes.size()) +
						" クラス）。図形が映りません。";
			if (stackingBroken)
				text += "レイヤの重ね順を上書きできませんでした（設定 " +
						std::to_string(stackingApplied) + " 件・図面に記録 0 件）。";
			*note = std::move(text);
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
