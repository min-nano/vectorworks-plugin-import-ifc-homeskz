//
//	draw/Section.cpp
//
//	軸組図（断面ビューポート）描画の実装。Python 版 vw/section.py の execute_sections /
//	_place_section_line / _arrange_viewports に対応する。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド（SDK あり）でのみ
//	コンパイルされ、無 SDK の core/parse ライブラリには入れない。
//
//	使用する SDK API（Vectorworks 2026 SDK。ci-debug の sdk-grep / shell で確認済み）:
//	  * gSDK->CreateSectionViewport(pt1, pt2, pt3, depth, startHeight, endHeight, layer)
//	                                      … 断面ビューポートの新規作成。ヘッダのコメントに
//	                                        「クラス・レイヤの表示はこの呼び出しでは扱わない。
//	                                        呼び出し後に設定し、そのあとで更新すること」と
//	                                        あるので、後段は伏図と同じ手順（draw/DrawUtil の
//	                                        ConfigureViewport）で仕上げる。
//	  * gSDK->SetObjectVariable(h, 1064/1035/1059, …) … 断面の見え方（下記）
//	  * gSDK->GetObjectVariable(h, …)        … 上を**読み戻して効いたか確かめる**（下記）
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
//	【軸組図としての見え方（要件）】**切断面より奥は表示せず**、**プレイナー（レイヤ平面）
//	図形は表示せず**、**2D コンポーネントは表示する**（下記のオブジェクト変数）。
//	断面の範囲は、**奥行きは無制限**（0 を渡す）・**高さは建物を包む実寸＋余白**・**長さは
//	断面線の長さ**（指示線を十分外まで延ばして実質無制限にする）。
//
//	【通り芯（グリッド線）の写り込み】VW のグリッド線は**デザインレイヤのレイヤ平面に置かれた
//	平面図形**で、ビューポートには**インスタンス**として出る。VW ヘルプによれば、断面・立面
//	ビューポートに出せるのは**視線に直交するグリッド線だけ**（平行なものはインスタンスを
//	作れない＝ダイアログ上でもグレーになる）。ところが**このプラグインが作った断面ビューポート
//	には、切断面に平行な通り芯が紙面に平行な水平線として写り込む**。ローカル確認で分かった
//	ことは次のとおり（ROADMAP.md M14 に切り分けの経過）:
//	  * **同じ書類で手で作った断面ビューポートには写らない**（＝通り芯オブジェクトやレイヤの
//	    表示ではなく、CreateSectionViewport が作るビューポート側の設定差）。
//	  * 設定の前後で読み戻した実測で、**1059（2D コンポーネント）だけ true を書いても入らない**
//	    と判明した（作成直後＝非表示、更新後も非表示。1064・1035 は同じ書き方で入っている）。
//	    手作りビューポートは**表示**なので、これが差だった。
//	そこで本実装は
//	  * 表示設定を当てた後に**読み戻して確かめ、入っていなければ更新の後にもう一度当てて更新
//	    し直す**（SetObjectVariable は書けなかったことを教えてくれないので推測しない）、
//	  * 1065（切断面より手前）も false にする（軸組図は切断面の軸組を見る図なので素直な指定。
//	    実測では作成直後から非表示だった）、
//	  * 4 つの表示設定を各段階で読み出して完了ダイアログへ出す（SnapshotDisplayOptions。
//	    **原因調査中の一時的な診断**）、
//	  * 切断面に平行な通り芯のクラスを、その軸組図でだけ非表示にする（命令の hiddenClasses。
//	    値は parse/Section の gridClassFor が決める。VW 自身の規則をクラス指定で実現するもので、
//	    表示設定の効き方に依らず効く）
//	を重ねる。当て直しで入ることが確認できたら、診断と重複した対処は整理する。
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

// ビューポートの更新（表示設定を当て直したときに描き直す）に使う VWFC ラッパー。
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// シートレイヤ上でビューポートを並べるレイアウト（用紙上・mm。Python 版 vw/section.py の
		// _ARRANGE_ORIGIN / _ARRANGE_COLUMNS / _ARRANGE_GAP と同じ値）。左上を基準に 1 行
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
		// 有限範囲で建物全体を収める（ROADMAP.md M14）。
		//
		// 長さの範囲も同じ理由で〈断面線の長さ〉のままになる。こちらは**指示線を通り芯の
		// bbox より十分外まで延ばす**ことで実質無制限にしている（parse/Section の
		// kSectionLineMargin）。

		// 断面ビューポートの表示設定（オブジェクト変数。Kernel/API/ObjectVariables.h。
		// 名前と番号は ci-debug の sdk-grep で確認済み）。
		//   1064 ovSectionViewportDisplayObjectsBeyondCutPlane … 切断面より**奥**を出すか
		//   1035 ovViewportDisplayPlanar … **プレイナー（レイヤ平面）図形と 2D 図形**を出すか
		//   1059 ovViewportDisplay2DComponents … ハイブリッドの 2D コンポーネントを出すか
		//   1065 ovSectionViewportDisplayObjectsBeforeCutPlane … 切断面より**手前**を出すか
		// **どれもビューポートの更新より前に設定する**（更新時の描画へ効かせるため。
		// CreateSectionViewport のヘッダコメントも「表示設定は呼び出し後、更新はその後」）。
		constexpr short kOVDisplayObjectsBeyondCutPlane = 1064;
		constexpr short kOVDisplayPlanarObjects = 1035;
		constexpr short kOVDisplay2DComponents = 1059;
		constexpr short kOVDisplayObjectsBeforeCutPlane = 1065;

		// 軸組図としてこうあってほしい値と、診断に出す表示名。
		//
		// **1059（2D コンポーネント）は書いても効かないことがある**——実機の読み戻しで、
		// CreateSectionViewport の直後は**非表示**で、true を書いてビューポートを更新しても
		// **非表示のまま**だった（1064・1035 は同じ書き方で入っている＝読み出しは正しい）。
		// 手で作った断面ビューポートは**表示**なので、これが「切断面に平行な通り芯が水平線と
		// して写る／写らない」の差になっている（ROADMAP.md M14）。そこで**更新のあとに当て
		// 直す**（drawSections の retryApply）。
		//
		// 1065（切断面より手前）は手作りビューポートとの差を潰すために足したもの。軸組図は
		// 切断面の軸組を見る図なので素直な指定でもある（実測では作成直後から非表示だった）。
		struct DisplayOption
		{
			short selector = 0;
			bool wanted = false;
			const char* label = "";
		};
		constexpr std::array<DisplayOption, 4> kDisplayOptions = {{
			{kOVDisplayObjectsBeyondCutPlane, false, "1064 奥"},
			{kOVDisplayPlanarObjects, false, "1035 プレイナー"},
			{kOVDisplay2DComponents, true, "1059 2D"},
			{kOVDisplayObjectsBeforeCutPlane, false, "1065 手前"},
		}};

		// オブジェクト変数へ真偽値を書き込む（draw/Footing の SetBooleanVariable と同じ流儀）。
		void SetBooleanVariable(MCObjectHandle object, short variable, Boolean value)
		{
			gSDK->SetObjectVariable(object, variable, TVariableBlock(value));
		}

		// オブジェクト変数の真偽値を読む。読めなければ false を返し out は触らない
		// （＝「読めない」と「false だった」を取り違えない）。
		bool ReadBooleanVariable(MCObjectHandle object, short variable, bool& out)
		{
			TVariableBlock block;
			if (!gSDK->GetObjectVariable(object, variable, block))
				return false;
			return block.GetBoolean(out);
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

		// 軸組図としての見え方を整える（要件。ConfigureViewport＝更新より**前**に呼び、
		// 効いていなければ更新の**後**にもう一度呼ぶ）。
		void ApplySectionDisplayOptions(MCObjectHandle viewport)
		{
			for (const DisplayOption& option : kDisplayOptions)
				SetBooleanVariable(viewport, option.selector, static_cast<Boolean>(option.wanted));
		}

		// 表示設定がすべて要件どおりに**読み戻せる**か。読めない項目があれば false
		// （＝「効いていない」側に倒す。当て直しを試す価値があるため）。
		bool MatchesDisplayOptions(MCObjectHandle viewport)
		{
			for (const DisplayOption& option : kDisplayOptions)
			{
				bool actual = false;
				if (!ReadBooleanVariable(viewport, option.selector, actual))
					return false;
				if (actual != option.wanted)
					return false;
			}
			return true;
		}

		// ビューポートを描き直す（表示設定を当て直したあとに要る）。失敗しても図は残るので
		// 黙って戻る（ConfigureViewport の Update と同じ扱い）。
		void UpdateViewport(MCObjectHandle viewport)
		{
			try
			{
				VWViewportObj vp(viewport);
				vp.Update();
			}
			catch (...)
			{
			}
		}

		// **ビューポートの表示設定をそのまま読み出す**（"1064 奥=表示 / 1035 プレイナー=非表示"
		// …の形）。読めない項目は「?」にする（＝「読めない」と「false だった」を混同しない）。
		//
		// 【なぜ読むか】SetObjectVariable は**変数が読み取り専用・型違い・そのオブジェクトに
		// 無い**とき黙って何もしない（返り値も見ていなかった）。「設定したつもりで効いていない」
		// を目視で見抜くのは難しいので、図面から読み戻して確かめる。
		//
		// 【調査中は作成直後と更新後の両方を出す】切断面に平行な通り芯が水平線として写る件
		// （ROADMAP.md M14）は、**同じ書類でも手で作ったビューポートには出ない**と分かって
		// おり、差は CreateSectionViewport が作るビューポートの設定にある。既定値が何かを
		// 実機から読むために、当面は完了ダイアログへ実測をそのまま出す（原因が確定したら、
		// 要件どおりでないときだけ出す形へ戻す）。
		std::string SnapshotDisplayOptions(MCObjectHandle viewport)
		{
			std::string text;
			for (const DisplayOption& option : kDisplayOptions)
			{
				bool actual = false;
				// 三項演算子を入れ子にしない（clang-tidy
				// readability-avoid-nested-conditional-operator）。
				const char* state = "?"; // 読めなかった
				if (ReadBooleanVariable(viewport, option.selector, actual))
					state = actual ? "表示" : "非表示";
				if (!text.empty())
					text += " / ";
				text += std::string(option.label) + "=" + state;
			}
			return text;
		}

		// できたビューポートをシートレイヤ上で重ならないように格子状へ並べる（Python 版
		// _arrange_viewports）。**実寸は描いてみるまで分からない**ので、GetObjectBounds で
		// 測ってから左上を合わせる。測れないものはその場に残す（並びが崩れるだけで図は残る）。
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

		// レイヤの走査とクラスの数え上げは全命令で共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup(document);

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
		std::size_t classesApplied = 0;
		// 表示設定（1064/1035/1059/1065）の実測（SnapshotDisplayOptions）。設定の前後で読み、
		// 完了ダイアログへ出す。**原因調査中の一時的な診断**（ROADMAP.md M14）。
		std::string displayNote;
		// 更新後の当て直しを試すか。1 枚目で効かなければ false にして以降は省く。
		bool retryApply = true;
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

			// 表示の作法（奥・手前を出さない／プレイナー図形を出さない／2D コンポーネントは
			// 出す）は**更新より前**に設定する（ConfigureViewport の最後が更新）。設定の前後で
			// 読み出しておき（1 枚目だけ。全命令で同じ設定なので診断行を命令の数だけ並べない）、
			// **CreateSectionViewport の既定値と、設定が効いたか**を診断へ出す。
			const bool inspect = drawn == 0;
			if (inspect)
				displayNote = "作成直後 " + SnapshotDisplayOptions(viewport);
			ApplySectionDisplayOptions(viewport);
			if (inspect)
				displayNote += " ｜ 設定直後 " + SnapshotDisplayOptions(viewport);
			classesApplied += ConfigureViewport(viewport, sheetLayer, setup, command.viewport);
			if (inspect)
				displayNote += " ｜ 更新後 " + SnapshotDisplayOptions(viewport);

			// **更新の後にもう一度当てる**。1059（2D コンポーネント）は作成直後に書いても
			// 入らないので、更新後に当て直して描き直す（kDisplayOptions のコメント）。
			// **1 枚目で効果が無ければ以降はやらない**（retryApply）——効かない当て直しの
			// ために全枚数を 2 回更新すると、取り込みが遅くなるだけになる。
			if (retryApply && !MatchesDisplayOptions(viewport))
			{
				ApplySectionDisplayOptions(viewport);
				UpdateViewport(viewport);
				if (inspect)
				{
					displayNote += " ｜ 再設定後 " + SnapshotDisplayOptions(viewport);
					retryApply = MatchesDisplayOptions(viewport);
				}
			}

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

		// 表示設定の実測（原因が別物なので他の診断とは別行にする）。**図は出ているのに
		// 見え方だけが違う**という不具合は目視で見落としやすいので、実測値をそのまま出す。
		if (note != nullptr && !displayNote.empty())
		{
			if (!note->empty())
				*note += "\n";
			*note += "軸組図の表示設定（実測。期待は 奥=非表示 / プレイナー=非表示 / 2D=表示 / "
					 "手前=非表示）: " +
					 displayNote;
		}

		const bool classesBroken = drawn > 0 && classesApplied == 0;
		if (note != nullptr && (missingSheetLayers > 0 || missingViewports > 0 || classesBroken))
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
