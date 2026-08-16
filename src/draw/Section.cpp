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
//	**SDK には高さ・長さを「無限」に切り替える呼び出しもオブジェクト変数も無い**ことを
//	ci-debug で SDK 全体を検索して確認済み（断面まわりの API は CreateSectionViewport /
//	CreateSectionLineInstance / IsSectionLineLinkedToViewport / UpdateSectionLineInstances の
//	4 つだけ、ovSectionViewport* にも範囲の項目は無い）。
//	いずれも「どこを切るか」ではなく「どう描くか」なので、命令セット（core::SectionCommand）
//	には載せずここが持つ。
//

#include "PluginPrefix.h"
#include "draw/Section.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
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
		// 消えかねないことがローカル確認で分かった。**SDK には高さ・長さを「無限」にする
		// 呼び出しもオブジェクト変数も無い**（断面まわりは CreateSectionViewport /
		// CreateSectionLineInstance / IsSectionLineLinkedToViewport /
		// UpdateSectionLineInstances だけ。ci-debug で SDK 全体を確認）ので、実寸＋余白の
		// 有限範囲で建物全体を収める（ROADMAP.md M14）。
		//
		// 長さの範囲も同じ理由で〈断面線の長さ〉のままになる。こちらは**指示線を通り芯の
		// bbox より十分外まで延ばす**ことで実質無制限にしている（parse/Section の
		// kSectionLineMargin）。

		// 断面ビューポートのオブジェクト変数（Kernel/API/ObjectVariables.h。ci-debug で確認）。
		//   1064 … 切断面より**奥**の図形を表示するか（要件: 表示しない）
		//   1035 … プレイナー（レイヤ平面）／2D 図形を表示するか（要件: 表示しない）
		//   1059 … ハイブリッドシンボル等の 2D コンポーネントを表示するか（要件: 表示する。
		//          既定でも表示だが、既定値に頼らず明示する）
		// **どれもビューポートの更新より前に設定する**（更新時の描画へ効かせるため。
		// CreateSectionViewport のヘッダコメントも「表示設定は呼び出し後、更新はその後」）。
		constexpr short kOVDisplayObjectsBeyondCutPlane = 1064;
		constexpr short kOVDisplayPlanarObjects = 1035;
		constexpr short kOVDisplay2DComponents = 1059;
		constexpr Boolean kShowObjectsBeyondCutPlane = false;
		constexpr Boolean kShowPlanarObjects = false;
		constexpr Boolean kShow2DComponents = true;

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

		// 軸組図としての見え方を整える（要件。ConfigureViewport＝更新より**前**に呼ぶ）。
		void ApplySectionDisplayOptions(MCObjectHandle viewport)
		{
			SetBooleanVariable(viewport, kOVDisplayObjectsBeyondCutPlane,
							   kShowObjectsBeyondCutPlane);
			SetBooleanVariable(viewport, kOVDisplayPlanarObjects, kShowPlanarObjects);
			SetBooleanVariable(viewport, kOVDisplay2DComponents, kShow2DComponents);
		}

#ifdef VW_DEV_BUILD
		// --- 【一時的な診断】断面の「範囲」を保持するオブジェクト変数を突き止める ---------
		//
		// 【なぜ要るか】VW の「断面の詳細設定」にある〈長さの範囲〉〈高さの範囲〉を**無限へ
		// 切り替える手段が SDK に無い**（範囲を決めるのは CreateSectionViewport の引数だけで、
		// 高さに 0 を渡すと〈有限 0〜0〉になってしまう。ROADMAP.md M14）。一方でオブジェクト
		// 変数には**公開ヘッダに載っていない欠番**（1067〜1076）があり、そこに範囲の設定が
		// 載っている可能性がある。読み取りだけなので図面は変えない。
		//
		// 【使い方（ローカル）】
		//   1. dev ビルドでインポートする（完了ダイアログの末尾にこの診断が出る）。
		//   2. VW で断面ビューポートを**1 枚だけ**選び、「断面の詳細設定」で
		//      〈長さの範囲: 無限〉〈高さの範囲: 無限〉へ手で変更する。
		//   3. 同じ図面でもう一度インポートする。**手で変えた 1 枚だけ値が違う**ので、
		//      「差のある変数」としてその番号と値が出る。
		//
		// 突き止めたらこのブロックごと削除し、ApplySectionDisplayOptions で該当変数を設定する。
		constexpr short kOVIsSectionViewport = 1054; // 断面ビューポートか（読み取り専用）
		constexpr short kProbeFirstSelector = 1060;
		constexpr short kProbeLastSelector = 1090;
		constexpr std::size_t kProbeMaxGroups = 4;	   // 報告する組み合わせの上限
		constexpr std::size_t kProbeMaxDiffLines = 12; // 報告する差の上限

		// 変数の値を人が読める文字列にする。型が分からないものは型番号だけを出す
		// （欠番の変数が何型かも手掛かりになる）。
		std::string DescribeVariable(const TVariableBlock& value)
		{
			bool flag = false;
			if (value.GetBoolean(flag))
				return flag ? "true" : "false";
			Sint32 int32 = 0;
			if (value.GetSint32(int32))
				return std::to_string(int32);
			Sint16 int16 = 0;
			if (value.GetSint16(int16))
				return std::to_string(int16);
			Uint8 uint8 = 0;
			if (value.GetUint8(uint8))
				return std::to_string(static_cast<int>(uint8));
			Real64 real = 0.0;
			if (value.GetReal64(real))
			{
				std::array<char, 32> buffer{};
				std::snprintf(buffer.data(), buffer.size(), "%g", real);
				return {buffer.data()};
			}
			return "型" + std::to_string(static_cast<int>(value.GetType()));
		}

		// 1 枚のビューポートについて、読めた変数を "セレクタ=値" の並びで返す。
		std::vector<std::pair<short, std::string>> ReadProbeValues(MCObjectHandle viewport)
		{
			std::vector<std::pair<short, std::string>> values;
			for (short selector = kProbeFirstSelector; selector <= kProbeLastSelector; ++selector)
			{
				TVariableBlock value;
				if (!gSDK->GetObjectVariable(viewport, selector, value))
					continue;
				values.emplace_back(selector, DescribeVariable(value));
			}
			return values;
		}

		// そのオブジェクトが断面ビューポートか。
		bool IsSectionViewport(MCObjectHandle object)
		{
			TVariableBlock value;
			if (!gSDK->GetObjectVariable(object, kOVIsSectionViewport, value))
				return false;
			bool flag = false;
			return value.GetBoolean(flag) && flag;
		}

		// 図面内の全断面ビューポートを走査し、**値の組み合わせが違うもの**を報告する。
		// 組み合わせが 1 通りしか無ければ「まだ差が無い」ことだけを出す（手順 2 が済んで
		// いない、あるいはその変数が範囲を持っていない、のどちらか）。
		std::string ProbeSectionExtentVariables(const std::vector<MCObjectHandle>& layers)
		{
			// 値の組み合わせ（文字列化したもの）→ その枚数と代表の値。
			std::map<std::string,
					 std::pair<std::size_t, std::vector<std::pair<short, std::string>>>>
				groups;
			std::size_t viewports = 0;
			for (const MCObjectHandle layer : layers)
			{
				for (MCObjectHandle h = gSDK->FirstMemberObj(layer); h != nil;
					 h = gSDK->NextObject(h))
				{
					if (!IsSectionViewport(h))
						continue;
					++viewports;
					std::vector<std::pair<short, std::string>> values = ReadProbeValues(h);
					std::string signature;
					for (const auto& [selector, text] : values)
						signature += std::to_string(selector) + "=" + text + ";";
					auto& group = groups[signature];
					++group.first;
					if (group.second.empty())
						group.second = std::move(values);
				}
			}

			if (viewports == 0)
				return {};
			std::string text = "範囲の探索: 断面ビューポート " + std::to_string(viewports) +
							   " 枚・値の組み合わせ " + std::to_string(groups.size()) + " 通り。";
			if (groups.size() < 2)
			{
				text += "（まだ差がありません。1 枚だけ手動で〈長さ・高さの範囲: 無限〉に"
						"してから、もう一度インポートしてください。）";
				return text;
			}

			// 組み合わせ間で値の違う変数だけを並べる（同じ値の変数は手掛かりにならない）。
			const auto& first = groups.begin()->second.second;
			std::size_t lines = 0;
			for (const auto& [selector, value] : first)
			{
				bool differs = false;
				std::string others;
				for (const auto& entry : groups)
				{
					const auto& group = entry.second;
					const auto& values = group.second;
					const auto match = std::ranges::find_if(values, [selector](const auto& entry)
															{ return entry.first == selector; });
					const std::string text2 = match != values.end() ? match->second : "-";
					if (text2 != value)
						differs = true;
					others += " " + text2 + "(" + std::to_string(group.first) + "枚)";
				}
				if (!differs)
					continue;
				if (++lines > kProbeMaxDiffLines)
				{
					text += "\n  …（差のある変数が多すぎます）";
					break;
				}
				text += "\n  " + std::to_string(selector) + ":" + others;
			}
			if (lines == 0)
				text += "（読み取れた変数には差がありませんでした。範囲は別の場所に"
						"保持されています。）";
			if (groups.size() > kProbeMaxGroups)
				text += "\n  （組み合わせが多いので先頭 " + std::to_string(kProbeMaxGroups) +
						" 通りだけを見ています）";
			return text;
		}
#endif // VW_DEV_BUILD

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
							 std::string* note)
	{
		const std::vector<core::SectionCommand>& commands = document.sections;
		if (commands.empty())
			return 0;

		// レイヤの走査とクラスの数え上げは全命令で共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup(document);

		// 断面の高さ範囲も全命令で共通（建物を包む実寸＋余白。core::sectionHeightRange）。
		// 求まらない＝高さの分かる要素が 1 つも無い文書では 0〜0 になるが、そのときは
		// そもそも切断位置が出ない（parse/Section）のでここへは来ない。
		double startHeight = 0.0;
		double endHeight = 0.0;
		core::sectionHeightRange(document, startHeight, endHeight);

		// 描画の前後でカレントレイヤが変わらないようにする（伏図と同じ作法）。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;
		std::size_t classesApplied = 0;
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
			ApplySectionDisplayOptions(viewport);
			classesApplied += ConfigureViewport(viewport, sheetLayer, setup, command.viewport);
			viewports.push_back(viewport);
			++drawn;
		}

		// 作ったぶんをシート上に並べる（中止で途中まででも、描けたものは重ならないようにする）。
		ArrangeViewports(viewports);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		const bool classesBroken = drawn > 0 && classesApplied == 0;
#ifdef VW_DEV_BUILD
		// 【一時的】範囲の設定がどのオブジェクト変数に載っているかの探索（上記の手順）。
		if (note != nullptr)
		{
			const std::string probe = ProbeSectionExtentVariables(setup.layers);
			if (!probe.empty())
			{
				if (!note->empty())
					*note += "\n";
				*note += probe;
			}
		}
#endif
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
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
