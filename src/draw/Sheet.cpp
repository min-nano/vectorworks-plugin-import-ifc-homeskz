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
//	  * gSDK->GetObjectInternalIndex(layer)           … 重ね順上書きが要る InternalIndex
//	  * gSDK->CreateViewportLayerOverride(vp, idx) /
//	    gSDK->SetViewportLayerStackingOverride(vp, idx, pos)
//	                                                  … per-viewport のレイヤ重ね順上書き
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

#include <cstddef>
#include <string>
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

		// レイヤの重ね順をこのビューポートだけで上書きする（M3 の【決定】の実装箇所。
		// draw/Sheet.h 参照）。order は前面→背面の希望順で、**そのビューポートに実在する
		// レイヤだけ**へ 0 から詰めた位置を与える（希望順に載っていないレイヤ＝ユーザーが
		// 別途作ったレイヤには触れない＝既定の重ね順のまま残す）。
		//
		// SetViewportLayerStackingOverride は上書きレコードが要るので、先に
		// CreateViewportLayerOverride を呼ぶ（既にあれば失敗するだけで害は無い）。
		void ApplyLayerStacking(MCObjectHandle viewport, const std::vector<std::string>& order)
		{
			Sint32 position = 0;
			for (const std::string& name : order)
			{
				const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
				if (layer == nil)
					continue;
				const InternalIndex index = gSDK->GetObjectInternalIndex(layer);
				if (index == 0)
					continue;
				gSDK->CreateViewportLayerOverride(viewport, index);
				gSDK->SetViewportLayerStackingOverride(viewport, index, position);
				++position;
			}
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

		// 描画の前後でカレントレイヤが変わると以降のフェーズ（M14 以降）に響くので、
		// 元のレイヤへ戻せるよう控えておく。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;

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
			ApplyLayerStacking(viewport, stacking);
			// **縮尺は表示レイヤを絞った後に読む**（映すレイヤの縮尺に合わせるため）。
			FinishViewport(viewport, command.viewport, LayerScaleFor(command.viewport));
			++drawn;
		}

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		// 診断行（何も無ければ空のまま）。「命令はあるのに 0 枚」のときに、シートレイヤを
		// 作れないのか、ビューポートを作れないのかを切り分けられる。
		if (note != nullptr && (missingSheetLayers > 0 || missingViewports > 0))
		{
			std::string text = "伏図の診断: ";
			if (missingSheetLayers > 0)
				text += "シートレイヤを作れなかった命令 " + std::to_string(missingSheetLayers) +
						" 件。";
			if (missingViewports > 0)
				text +=
					"ビューポートを作れなかった命令 " + std::to_string(missingViewports) + " 件。";
			*note = std::move(text);
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
