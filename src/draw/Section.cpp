//
//	draw/Section.cpp
//
//	軸組図（断面ビューポート）描画の実装。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を
//	include するため、この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK
//	の core/parse ライブラリには入れない。
//
//	使用する SDK API（Vectorworks 2026 SDK。ci-debug の sdk-grep / shell で確認済み）:
//	  * gSDK->CreateSectionViewport(pt1, pt2, pt3, depth, startHeight, endHeight, layer)
//	                                      … 断面ビューポートの新規作成。ヘッダのコメントに
//	                                        「クラス・レイヤの表示はこの呼び出しでは扱わない。
//	                                        呼び出し後に設定し、そのあとで更新すること」と
//	                                        あるので、後段は伏図と同じ手順（draw/DrawUtil の
//	                                        ConfigureViewport）で仕上げる。
//	  * gSDK->SetObjectVariable(h, 1064/1035/1059, …) … 断面の見え方（下記）
//	  * VWViewportObj::SetRenderType(renderFinalHiddenLine) … レンダリング（下記）
//	  * gSDK->GetObjectBounds(h, WorldRect&) … できたビューポートの実寸（並べるのに使う）
//	  * gSDK->MoveObject(h, dx, dy)          … シートレイヤ上での移動
//
//	【切断面の与え方（ローカル確認で実証済み）】ISDK の引数名は 3 点とも "sectionLinePt" だが、
//	VW の UI は「切断線の点を 2 つ以上クリック → **切断の向き**をクリック → 奥行きを指定」
//	という順で、API の引数もこの順（点 2 つ＋向きの点＋奥行き）に対応する:
//	  * pt1 / pt2 … 切断線（通りの上を端から端まで。命令の lineStart / lineEnd）
//	  * pt3       … **見る側**を示す点（命令の viewPoint。指示線の中点から視線方向へ離した点）
//	**実機で断面指示線が直線になり、視線の向きも意図どおりであることを確認済み**（pt3 が
//	「切断線の 3 点目」であれば指示線は L 字に折れるはずで、そうならなかった）。視線の向きの
//	決め方（X通り＝−X 方向・Y通り＝+Y 方向。図面の右へ座標が増える＝通り名が左から右へ並ぶ）は
//	parse/Section が持つ。
//
//	【軸組図としての見え方（要件）】**切断面より奥は表示せず**、**プレイナー（アクティブ
//	レイヤ平面）図形は表示せず**、**2D コンポーネントは表示する**（下記のオブジェクト変数）。
//	断面の範囲は、**奥行きは無制限**（0 を渡す）・**高さは建物を包む実寸＋余白**・**長さは
//	断面線の長さ**（指示線を十分外まで延ばして実質無制限にする）。
//
//	【レンダリングは〈隠線消去〉にする（オブジェクト情報パレットの項目が減る原因）】
//	インポートした軸組図を選ぶと、**手で作った断面ビューポートには在る項目がパレットから
//	丸ごと消えている**という実機の症状があった（〈2D コンポーネントを表示〉と〈2D の面を
//	表示〉が無い・〈切断面より手前を表示〉が灰色で押せない）。原因は**ビューポートの
//	レンダリング（バックグラウンド）**の違いで、手作りは〈隠線消去〉、`CreateSectionViewport`
//	が作ったものは〈シェイド〉になっていた（1 枚目・2 枚目のパレットを見比べると、
//	消えている項目は**すべて隠線消去でしか意味を持たない項目**で、それ以外の項目は同じ）。
//	VW は**その描き方で効かない設定をパレットから隠す**（SDK にも読み取り専用の
//	`ovViewportHiddenLineDisplay2DFillsAllowed`＝「2D の面を出せる状態か」という変数がある）
//	ので、シェイドのままでは項目が出ない。**2D コンポーネント（1059）へ true を書いても
//	入らなかった**のも同じ理由と考えられる（ハイブリッドシンボルの 2D 表現は隠線消去・
//	ワイヤーフレームでしか描かれない）。
//	そこで**作った直後・更新より前にレンダリングを `renderFinalHiddenLine`（隠線消去）へ
//	揃える**。これで手作りの断面と同じ項目が並び、同じように設定を変えられる。
//	なお〈切断面より奥の範囲〉の中の項目が灰色なのは**〈切断面より奥を表示〉が off だから**で
//	（要件どおり。手作りでも off にすれば同じく灰色になる）、こちらは不具合ではない。
//
//	【範囲を「無限」にはできない（調べ尽くした結論）】UI で断面ビューポートを手で作ると
//	〈長さ・高さの範囲〉は既定で〈無限〉になるが、**SDK からその状態にする手段は無い**。
//	  * 断面まわりの API は CreateSectionViewport / CreateSectionLineInstance /
//	    IsSectionLineLinkedToViewport / UpdateSectionLineInstances の 4 つだけで、範囲を
//	    切り替える呼び出しは無い（ci-debug で SDK 全体を検索）。
//	  * ovSectionViewport* のオブジェクト変数にも範囲の項目は無い。
//	  * 公開ヘッダの**欠番**（1060〜1090）に隠れている可能性を dev ビルドの一時診断で
//	    調べたが、**手で〈無限〉にした 1 枚とプラグインが作った 65 枚とで値の違う変数は
//	    1 つも無かった**（断面ビューポート 66 枚・値の組み合わせ 1 通り）。範囲はオブジェクト
//	    変数の外に保持されている。
//	そこで**建物の大きさから有限の範囲を決める**（高さ＝core::sectionHeightRange、長さ＝
//	指示線を通り芯 bbox より十分外へ延ばす）。実用上は無限と同じ見え方になる。
//	いずれも「どこを切るか」ではなく「どう描くか」なので、命令セット（core::SectionCommand）
//	には載せずここが持つ。
//

#include "PluginPrefix.h"
#include "draw/Section.h"
#include "draw/DrawUtil.h"
#include "draw/Tag.h"
#include "core/Document.h"
#include "core/Progress.h"

// レンダリングの設定に使う VWFC ラッパー（draw/DrawUtil が縮尺・図番で使うのと同じもの）。
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// シートレイヤ上でビューポートを並べるレイアウト（用紙上・ mm）。左上を基準に 1 行
		// kArrangeColumns 枚ずつ、各ビューポートの実寸に余白を足して詰める。
		constexpr double kArrangeOriginX = 0.0;
		constexpr double kArrangeOriginY = 0.0;
		constexpr int kArrangeColumns = 5;
		constexpr double kArrangeGap = 300.0;

		// 切断面より奥の範囲に渡す値。**0 が「無限」**（ローカル確認で実測: 0 を渡した
		// ビューポートの「断面の詳細設定」で〈切断面より奥の範囲: 無限〉になっていた）。
		// 奥は表示しない設定（下記 1064）なので実際には効かないが、範囲の指定としては
		// 無制限にしておく。
		constexpr double kInfiniteDepth = 0.0;

		// **高さの範囲だけは実寸を渡す**（core::sectionHeightRange。建物の上下＋余白）。
		// 同じ 0 を高さへ渡すと〈高さの範囲: 有限・始点 0・終点 0〉になり、断面から建物が
		// 消えてしまうことがローカル確認で分かった。**高さ・長さを「無限」にする手段は
		// SDK に無い**（ファイル冒頭「範囲を『無限』にはできない」）ので、実寸＋余白の
		// 有限範囲で建物全体を収める（docs/DEV-NOTES.md M14）。
		//
		// 長さの範囲も同じ理由で〈断面線の長さ〉のままになる。こちらは**指示線を通り芯の
		// bbox より十分外まで延ばす**ことで実質無制限にしている（parse/Section の
		// kSectionLineMargin）。

		// 断面ビューポートのオブジェクト変数（Kernel/API/ObjectVariables.h。ci-debug で確認）。
		//   1064 … 切断面より**奥**の図形を表示するか（要件: 表示しない）
		//   1035 … プレイナー（レイヤ平面）／2D 図形を表示するか（要件: 表示しない）
		//   1059 … ハイブリッドシンボル等の 2D コンポーネントを表示するか（要件: 表示する。
		//          **CreateSectionViewport の直後は非表示**で、しかも**レンダリングが
		//          シェイドのままでは書いても入らない**——先に隠線消去へ揃えること
		//          （ファイル冒頭「レンダリングは〈隠線消去〉にする」）。入ったかどうかは
		//          読み戻して診断へ残す）
		// **どれもビューポートの更新より前に設定する**（更新時の描画へ効かせるため。
		// CreateSectionViewport のヘッダコメントも「表示設定は呼び出し後、更新はその後」）。
		constexpr short kOVDisplayObjectsBeyondCutPlane = 1064;
		constexpr short kOVDisplayPlanarObjects = 1035;
		constexpr short kOVDisplay2DComponents = 1059;
		constexpr Boolean kShowObjectsBeyondCutPlane = false;
		constexpr Boolean kShowPlanarObjects = false;
		constexpr Boolean kShow2DComponents = true;

		// ビューポートのレンダリング（バックグラウンド）。**手で作った断面ビューポートと
		// 同じ〈隠線消去〉**に揃える（ファイル冒頭「レンダリングは〈隠線消去〉にする」）。
		// TRenderMode は Kernel/API/MiniCadCallBacks.h（renderFinalHiddenLine = 6 が
		// VW の UI の〈隠線消去〉、renderOpenGL = 11 が〈シェイド〉）。
		// **レンダリング（輪郭）は触らない**——手作りも既定の〈なし〉で、こちらは差が無い。
		constexpr TRenderMode kSectionRenderMode = renderFinalHiddenLine;

		// オブジェクト変数へ真偽値を書き込む（draw/Footing の SetBooleanVariable と同じ流儀）。
		void SetBooleanVariable(MCObjectHandle object, short variable, Boolean value)
		{
			gSDK->SetObjectVariable(object, variable, TVariableBlock(value));
		}

		// 断面ビューポートを 1 枚作る。作れなければ nil。奥行きは無制限、高さは建物を包む
		// 実寸（上記）。
		MCObjectHandle CreateSectionViewport(const core::SectionCommand& command,
											 MCObjectHandle sheetLayer, double startHeight,
											 double endHeight)
		{
			const WorldPt start(command.lineStart.x, command.lineStart.y);
			const WorldPt end(command.lineEnd.x, command.lineEnd.y);
			const WorldPt view(command.viewPoint.x, command.viewPoint.y);
			return gSDK->CreateSectionViewport(start, end, view, kInfiniteDepth, startHeight,
											   endHeight, sheetLayer);
		}

		// レンダリング（バックグラウンド）を〈隠線消去〉にする。できたら true
		// （ファイル冒頭「レンダリングは〈隠線消去〉にする」）。
		bool ApplySectionRenderMode(MCObjectHandle viewport)
		{
			try
			{
				VWViewportObj(viewport).SetRenderType(kSectionRenderMode);
			}
			catch (...)
			{
				// レンダリングを変えられなくても断面そのものは図面に残る（見え方だけの
				// 設定）。1 枚の失敗で残りを止めず、件数だけ診断へ残す。
				return false;
			}
			return true;
		}

		// 軸組図としての見え方を整える（要件。ConfigureViewport＝更新より**前**に呼ぶ）。
		// レンダリングを揃えられたら true。
		//
		// **レンダリングを先に決める**——2D コンポーネント（1059）のような「その描き方でしか
		// 意味を持たない設定」は、レンダリングがシェイドのままだと書いても入らない
		// （ファイル冒頭「レンダリングは〈隠線消去〉にする」）。順番を入れ替えないこと。
		bool ApplySectionDisplayOptions(MCObjectHandle viewport)
		{
			const bool rendered = ApplySectionRenderMode(viewport);
			SetBooleanVariable(viewport, kOVDisplayObjectsBeyondCutPlane,
							   kShowObjectsBeyondCutPlane);
			SetBooleanVariable(viewport, kOVDisplayPlanarObjects, kShowPlanarObjects);
			SetBooleanVariable(viewport, kOVDisplay2DComponents, kShow2DComponents);
			// 【読み戻して確かめない】以前は 2D コンポーネント（1059）を読み戻して
			// 「入らなかった」を診断へ出していたが、**実機ではハイブリッドシンボルの 2D
			// 表現がちゃんと出ているのに読み戻しは false を返す**（ローカル確認）。
			// 読める値が実際の表示と一致しない以上、この読み戻しは誤警報しか生まないので
			// 外した。書き込みはそのまま残す。
			return rendered;
		}

		// できたビューポートをシートレイヤ上で重ならないように格子状へ並べる。**実寸は描いて
		// みるまで分からない**ので、GetObjectBounds で測ってから左上を合わせる。
		// 測れないものはその場に残す（並びが崩れるだけで図は残る）。
		void ArrangeViewports(const std::vector<MCObjectHandle>& viewports)
		{
			double cursorX = kArrangeOriginX;
			double cursorY = kArrangeOriginY;
			double rowHeight = 0.0;
			int column = 0;
			for (const MCObjectHandle viewport : viewports)
			{
				WorldRect bounds;
				if (!gSDK->GetObjectBounds(viewport, bounds))
					continue;
				const double width = bounds.right - bounds.left;
				// WorldRect は top > bottom（Y 上向き）。高さは絶対値で見る。
				const double height = std::abs(bounds.top - bounds.bottom);
				gSDK->MoveObject(viewport, cursorX - bounds.left, cursorY - bounds.top);
				cursorX += width + kArrangeGap;
				rowHeight = std::max(rowHeight, height);
				++column;
				if (column >= kArrangeColumns)
				{
					column = 0;
					cursorX = kArrangeOriginX;
					cursorY -= rowHeight + kArrangeGap;
					rowHeight = 0.0;
				}
			}
		}
	} // namespace

	std::size_t drawSections(const core::Document& document, core::ProgressReporter& progress,
							 std::string* note, const ObjectHandles* memberHandles)
	{
		const std::vector<core::SectionCommand>& commands = document.sections;
		if (commands.empty())
			return 0;

		// レイヤとクラスの列挙は全命令で共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup();

		// 断面の高さ範囲も全命令で共通（建物を包む実寸＋余白。core::sectionHeightRange）。
		// **求まらないときは描かない**——高さの分かる要素が 1 つも無い文書では out が
		// 触られず 0〜0 のままで、その範囲で作ると「軸組図はあるのに空」になる（高さに 0 を
		// 渡すと〈高さの範囲: 有限・0〜0〉になる。ファイル冒頭）。parse/Section を通った
		// 文書ならここへは来ないが、drawSections は**任意の Document を取れる公開関数**
		// なので、暗黙の不変条件に寄りかからず理由を残して抜ける。
		double startHeight = 0.0;
		double endHeight = 0.0;
		if (!core::sectionHeightRange(document, startHeight, endHeight))
		{
			if (note != nullptr)
			{
				if (!note->empty())
					*note += "\n";
				*note += "軸組図の診断: 建物の高さが求まらないため作成しませんでした。";
			}
			return 0;
		}

		// 描画の前後でカレントレイヤが変わらないようにする（伏図と同じ作法）。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;
		std::size_t missingRenderMode = 0;
		std::size_t classesApplied = 0;
		// 仕上げの更新が走らなかった枚数（伏図と同じ。draw/DrawUtil の ViewportFinish）。
		std::size_t notUpdated = 0;
		// 断面寸法データタグ（M13）。伏図と同じ受け渡し・同じ実装（draw/Tag）。
		const ObjectHandles emptyHandles;
		const ObjectHandleTable& members =
			memberHandles != nullptr ? memberHandles->table() : emptyHandles.table();
		TagCounts tags;
		// タグ PIO の定義を先に用意する（伏図と同じ。draw/Tag.h）。タグが 1 つも無い文書では
		// 定義そのものを作らない。
		if (std::ranges::any_of(commands, [](const core::SectionCommand& section)
								{ return !section.viewport.tags.empty(); }))
			prepareDataTagPlugin();
		std::vector<MCObjectHandle> viewports;
		viewports.reserve(commands.size());

		for (const core::SectionCommand& command : commands)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。
			if (progress.cancelled())
				break;
			progress.step();

			const MCObjectHandle sheetLayer = PrepareSheetLayer(command.number, command.title);
			if (sheetLayer == nil)
			{
				++missingSheetLayers;
				continue;
			}

			const MCObjectHandle viewport =
				CreateSectionViewport(command, sheetLayer, startHeight, endHeight);
			if (viewport == nil)
			{
				// 断面ビューポートを作れなかった。件数を診断へ残して「命令はあるのに軸組図が
				// 無い」原因を切り分けられるようにする（1 枚の失敗で残りを止めない）。
				++missingViewports;
				continue;
			}

			// 表示の作法（奥を出さない・プレイナー図形を出さない・2D コンポーネントは出す）は
			// **更新より前**に設定する（ConfigureViewport の最後が更新）。
			if (!ApplySectionDisplayOptions(viewport))
				++missingRenderMode;
			// **投影は触らない**（ViewportProjection::Keep）——断面の向きで作られているので、
			// 伏図がやる 2D/平面への作り直し（draw/DrawUtil.h の ViewportProjection）は
			// ここでは意味を成さない。
			const ViewportFinish finish = ConfigureViewport(
				viewport, sheetLayer, setup, command.viewport, ViewportProjection::Keep);
			classesApplied += finish.classesApplied;
			if (!finish.updated)
				++notUpdated;
			// 断面寸法データタグ。**並べ替え（ArrangeViewports）より前**に置く——注釈は
			// ビューポートと一緒に動くので、先に置いておけば移動しても図の上に留まる。
			drawViewportTags(viewport, command.viewport, members, tags);
			viewports.push_back(viewport);
			++drawn;
		}

		// 作ったぶんをシート上に並べる（中止で途中まででも、描けたものは重ならないようにする）。
		ArrangeViewports(viewports);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		const bool classesBroken = drawn > 0 && classesApplied == 0;
		if (note != nullptr && (missingSheetLayers > 0 || missingViewports > 0 || classesBroken ||
								missingRenderMode > 0 || notUpdated > 0))
		{
			std::string text = "軸組図の診断: ";
			if (missingSheetLayers > 0)
				text += "シートレイヤを作れなかった命令 " + std::to_string(missingSheetLayers) +
						" 件。";
			if (missingViewports > 0)
				text += "断面ビューポートを作れなかった命令 " + std::to_string(missingViewports) +
						" 件。";
			if (classesBroken)
				text += "クラスを表示に戻せませんでした（対象 " +
						std::to_string(setup.classes.size()) + " クラス）。図形が映りません。";
			if (missingRenderMode > 0)
				text += "レンダリングを〈隠線消去〉にできなかった軸組図 " +
						std::to_string(missingRenderMode) + " 枚。";
			if (notUpdated > 0)
				text +=
					"更新が走らなかった軸組図 " + std::to_string(notUpdated) +
					" 枚（オブジェクト情報パレットの「更新」を押すまで生成時のまま描かれます）。";
			if (!note->empty())
				*note += "\n";
			*note += text;
		}

		// タグの診断は軸組図の診断とは別行にする（原因が別物なので混ぜない）。
		const std::string tagNote = tagDiagnostics("軸組図", tags);
		if (note != nullptr && !tagNote.empty())
		{
			if (!note->empty())
				*note += "\n";
			*note += tagNote;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
