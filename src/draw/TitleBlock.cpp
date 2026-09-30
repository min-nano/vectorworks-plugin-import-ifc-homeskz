//
//	draw/TitleBlock.cpp
//
//	図面枠（タイトルブロック）の設置の実装。意図・規約は draw/TitleBlock.h と
//	core/Document.h の titleBlockStyle を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API:
//	  * ResolvePluginStyle(styleName)            … スタイル名 → RefNumber（draw/DrawUtil）
//	  * PrepareCustomObjectDefinition(name)      … 設定ダイアログ抑止（draw/DrawUtil）
//	  * gSDK->SetCurrentLayer(sheetLayer)        … 置き場所（用紙）の指定
//	  * gSDK->CreateCustomObject(name, 位置, 0, true) … 図面枠 PIO の生成
//	  * gSDK->SetPluginObjectStyle(object, style)     … スタイルの関連付け
//	  * gSDK->UpdateStyledObjects(style)              … スタイルの中身を流し込む（1 回）
//	  * gSDK->FirstMemberObj / InsertObjectBefore     … 最背面へ回す
//	  * gSDK->GetObjectBounds / MoveObject            … 置いた後に測って動かす
//	  * gSDK->DeleteObject                            … 割り付けのために仮に置いた枠を消す
//

#include "PluginPrefix.h"
#include "draw/TitleBlock.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 【図面枠 PIO の登録名】**実機で確定した**（VW 2026 / macOS。PR #129 の実機
		// フィードバック round 1 の診断ログ）。SDK リファレンスの `Findings/` にはまだ
		// 無いので、**知見としてあちらへ送ること**（CLAUDE.md「ドキュメントの分担」）。
		//
		// 当初は候補を 3 つ並べて順に試していた（登録名が `Findings/` に無く、型番号
		// ＝`GetSymbolDefSubType` が返す 552 から名前を引く呼び出しも知られていないため）。
		// 1 周で `"Title Block Border"` が通ったので、**候補は畳んだ**——通らない名前を
		// 抱えたままにすると、いつ何が効いているのか分からなくなる。
		//
		// **universal 名はローカライズもプラットフォーム依存もしない**ので Windows でも
		// 同じはずだが、確かめたのは macOS の 1 周だけである。**置けなかった件数は必ず
		// 診断へ出す**ので、違っていれば次の周で分かる（titleBlockDiagnostics）。
		constexpr const char* kTitleBlockPlugin = "Title Block Border";

		// 用紙の中心。**原点である**（draw/TitleBlock.h「置き場所は測って決める」。
		// draw/DrawUtil.h の SheetPaperArea が同じ前提で印刷可能領域を組み立てている）。
		constexpr core::Vec2 kPaperCenter{0.0, 0.0};

		// そのシートレイヤはもう控えたか。軸組図は同じシートレイヤへ複数の命令が載るので、
		// これが無いと 1 枚の用紙に枠が何重にも積まれる（draw/Section）。
		bool AlreadyPlaced(const TitleBlockCounts& counts, MCObjectHandle sheetLayer)
		{
			return std::ranges::find(counts.sheets, sheetLayer) != counts.sheets.end();
		}

		// 図面枠 PIO を 1 つ作る（作れなければ nil）。**定義の用意
		// （PrepareCustomObjectDefinition）は生成の直前に行う**——最初の 1 個で
		// 「オブジェクトの設定」ダイアログが出ると、無人で回る往復の周がそこで止まる
		// （draw/DrawUtil.h の PrepareCustomObjectDefinition）。
		MCObjectHandle CreateTitleBlock()
		{
			PrepareCustomObjectDefinition(kTitleBlockPlugin);
			// 生成位置は仮（用紙の中心へ寄せるのは、スタイルを流し込んで大きさが定まって
			// から＝finishTitleBlocks）。
			return gSDK->CreateCustomObject(TXString(kTitleBlockPlugin),
											WorldPt(kPaperCenter.x, kPaperCenter.y), 0.0, true);
		}
	} // namespace

	TitleBlockCounts prepareTitleBlocks(const core::Document& document)
	{
		TitleBlockCounts counts;
		counts.style = document.titleBlockStyle;
		if (counts.style.empty())
			return counts; // 置かない（設定の既定）

		// **スタイルが図面に無ければ 1 つも置かない**（draw/TitleBlock.h の ★）。
		// スタイル無しの図面枠は空の枠にしかならず、図面を汚すだけになる。
		counts.styleRef = ResolvePluginStyle(TXString(counts.style.c_str()));
		return counts;
	}

	std::optional<core::PaperArea> measureTitleBlockFrame(const TitleBlockCounts& counts,
														  MCObjectHandle sheetLayer)
	{
		if (sheetLayer == nil || counts.styleRef == 0)
			return std::nullopt;

		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();
		gSDK->SetCurrentLayer(sheetLayer);
		std::optional<core::PaperArea> frame;
		if (const MCObjectHandle probe = CreateTitleBlock(); probe != nil)
		{
			// 外形はスタイルの中身が流れてから定まる（finishTitleBlocks と同じ手順）。
			gSDK->SetPluginObjectStyle(probe, counts.styleRef);
			gSDK->UpdateStyledObjects(counts.styleRef);
			WorldRect bounds;
			if (gSDK->GetObjectBounds(probe, bounds))
			{
				// 本物は用紙の中心＝原点へ寄せて置く（finishTitleBlocks）ので、大きさだけを
				// 採って原点の周りの矩形にする。
				const double halfWidth = std::abs(bounds.right - bounds.left) / 2.0;
				const double halfHeight = std::abs(bounds.top - bounds.bottom) / 2.0;
				if (halfWidth > 0.0 && halfHeight > 0.0)
					frame = core::PaperArea{
						core::Vec2{kPaperCenter.x - halfWidth, kPaperCenter.y - halfHeight},
						core::Vec2{kPaperCenter.x + halfWidth, kPaperCenter.y + halfHeight}};
			}
			gSDK->DeleteObject(probe, true);
		}
		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);
		return frame;
	}

	void addTitleBlockSheet(MCObjectHandle sheetLayer, TitleBlockCounts& counts)
	{
		if (sheetLayer == nil || counts.styleRef == 0)
			return;
		if (AlreadyPlaced(counts, sheetLayer))
			return;
		counts.sheets.push_back(sheetLayer);
	}

	void finishTitleBlocks(TitleBlockCounts& counts)
	{
		if (counts.sheets.empty())
			return;

		// --- 置く ------------------------------------------------------------------
		//
		// ★**ここはビューポートの縮尺を確定させた後**（draw/TitleBlock.h）。図面枠の縮尺欄は
		// 作ったときに用紙に載っているビューポートの縮尺を拾い、その後は勝手に取り直さない
		// ので、先に作ると 1:1 のまま残る（`UpdateStyledObjects` も `ResetObject` も効かな
		// かった。docs/DEV-NOTES.md M28）。
		std::vector<std::pair<MCObjectHandle, MCObjectHandle>> placed; // (シートレイヤ, 図面枠)
		placed.reserve(counts.sheets.size());
		for (const MCObjectHandle sheetLayer : counts.sheets)
		{
			// 図面枠は**シートレイヤの上**に置く（用紙に載る）。PIO は bInsert=true で
			// カレントレイヤへ入るので、先にそのシートレイヤをアクティブにする。
			gSDK->SetCurrentLayer(sheetLayer);
			const MCObjectHandle object = CreateTitleBlock();
			if (object == nil)
			{
				++counts.failed;
				continue;
			}
			counts.plugin = kTitleBlockPlugin;
			// スタイルは関連付けるだけでは中身が流れない（[Findings「Parametric Objects」]
			// (https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Parametric%20Objects.md)
			// の「プラグインスタイル」）。流し込みは全部置き終えてから 1 回（下）。
			gSDK->SetPluginObjectStyle(object, counts.styleRef);
			placed.emplace_back(sheetLayer, object);
			++counts.drawn;
		}
		if (placed.empty())
			return;

		// スタイルの中身を流し込む（**ジオメトリの作り直しまで行う**ので、1 つずつの
		// ResetObject は要らない。上記 Findings）。これを通さないと枠の外形が定まらず、
		// 下の位置合わせが測るものを持たない。
		gSDK->UpdateStyledObjects(counts.styleRef);

		for (const auto& [sheetLayer, object] : placed)
		{
			// --- 最背面へ回す ------------------------------------------------------
			//
			// 後から作ったので、このままでは**ビューポート・凡例の手前**にあって図を覆う。
			// オブジェクト列は背面→前面の順なので、シートレイヤの先頭の前へ差し込めば
			// 最背面になる（draw/TitleBlock.h の ★）。
			const MCObjectHandle first = gSDK->FirstMemberObj(sheetLayer);
			if (first != object && (first == nil || !gSDK->InsertObjectBefore(object, first)))
				++counts.frontLeft;

			// --- 測って用紙の中心へ寄せる（draw/TitleBlock.h「置き場所は測って決める」）---
			WorldRect bounds;
			if (!gSDK->GetObjectBounds(object, bounds))
			{
				++counts.placeLeft;
				continue;
			}
			const double centerX = (bounds.left + bounds.right) / 2.0;
			const double centerY = (bounds.bottom + bounds.top) / 2.0;
			gSDK->MoveObject(object, kPaperCenter.x - centerX, kPaperCenter.y - centerY);
		}
	}

	std::string titleBlockDiagnostics(const TitleBlockCounts& counts)
	{
		// **「置かない」は異常ではない**（設定の既定）ので何も言わない。
		if (counts.style.empty())
			return {};

		const bool styleMissing = counts.styleRef == 0;
		if (!styleMissing && counts.failed == 0 && counts.placeLeft == 0 && counts.frontLeft == 0)
			return {};

		std::string text = "図面枠の診断: ";
		if (styleMissing)
			text += "図面枠スタイル「" + counts.style +
					"」がこの図面に無いので、図面枠を置いていません。";
		// **登録名を文面へ入れる**——ここが効かないときの原因はほぼそれなので、次の周で
		// 名前を疑えるようにしておく。**綴りは kTitleBlockPlugin から引く**（書き下すと
		// 登録名を変えたときにこの診断だけ古いまま残る。CLAUDE.md「重複を作らない置き場所」）。
		const std::string missing =
			"図面枠のプラグイン \"" + std::string(kTitleBlockPlugin) + "\" を呼び出せませんでした";
		AppendCount(text, "図面枠を作れなかったシートレイヤ", counts.failed, "枚", missing.c_str());
		AppendCount(text, "用紙の中心へ寄せられなかった図面枠", counts.placeLeft, "枚",
					"外形を測れませんでした");
		AppendCount(text, "最背面へ回せなかった図面枠", counts.frontLeft, "枚",
					"図を覆っているかもしれません");
		return text;
	}

	std::string titleBlockInfo(const char* what, const TitleBlockCounts& counts)
	{
		if (counts.style.empty() || counts.drawn == 0)
			return {};
		// **使った登録名を必ず出す**（別の環境で違っていたときに、ここが唯一の手掛かりに
		// なる。draw/TitleBlock.h の ★）。
		return std::string("図面枠（") + what + "）: スタイル「" + counts.style + "」を " +
			   std::to_string(counts.drawn) + " 枚に置きました（登録名 \"" + counts.plugin +
			   "\"）。";
	}
} // namespace HomeskzIfcImport::draw
