//
//	draw/Symbol.cpp
//
//	シンボル配置の実装。Python 版 vw/{anchor_bolt,floor_post,fire_brace,joint}.py の
//	draw_* / execute_* に対応する（4 本の逐語的な複製を 1 本にまとめてある。draw/Symbol.h 参照）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API は ISDK（gSDK）／VWFC の実在シグネチャに合わせている
//	（Vectorworks 2026 SDK。ci-debug の sdk-grep / sdk-ls で確認済み）:
//	  * draw/DrawUtil の ActivateExistingLayer            … 配置先レイヤ（既存のみ）
//	  * VWSymbolDefObj::IsSymbolDefObject(name)           … シンボル定義が図面に在るか
//	  * VWSymbolObj(name, VWPoint2D(x, y), angleDeg)      … シンボルインスタンスの配置
//	  * VWSymbolObj::IsSymbolObject(handle, name)         … 置けたものが本当にそれか
//	VWSymbolObj のこの構築子が VS の vs.Symbol(name, point, angle) にあたる。
//
//	【成功判定に handle の非 nil を使わない】ISDK にシンボルを配置する呼び出しは無く
//	（在るのは CreateSymbolDefinition だけ）、この構築子はレガシーの PlaceSymbol を包む。
//	PlaceSymbol は「定義が nil なら何もしない」仕様なので、**定義が無いときでも構築子は
//	非 nil のハンドル（直前のオブジェクト等）を返し得る**。実際に「アンカーボルト 97 本…
//	を描きました」と全数成功で報告しながら図面には 1 つも出ていない、という報告を受けた。
//	そこで生成したハンドルが**その名前のシンボルインスタンスであること**まで確かめて
//	初めて 1 件と数える。
//
//	【設計上の要点】**シンボル定義はプラグインが作らない。** ハイブリッドシンボル
//	（"アンカーボルト_M12" / "アンカーボルト_M16" / "床束" / "鋼製火打" / "仕口"）は
//	テンプレートやリソースライブラリから供給される前提で、Python 版も同じく既存の
//	シンボルを名前で置くだけ。定義が無ければ**黙って何も置かない**のではなく、件数を
//	診断行に出して「命令はあるのに見えない」原因が分かるようにする。
//
//	配置角度・基準姿勢（火打の 45 度補正が正しいか等）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M11「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Symbol.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWSymbolDefObj.h"
#include "VWFC/VWObjects/VWSymbolObj.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// シンボル定義が図面に在るか。**配置を止める門にはしない**（下記 drawSymbols）——
		// 配置に失敗した理由を診断に書き分けるためだけに使う。
		bool HasSymbolDefinition(const std::string& name)
		{
			try
			{
				return VWSymbolDefObj::IsSymbolDefObject(TXString(name.c_str()));
			}
			catch (...)
			{
				return false;
			}
		}

		// シンボル 1 つを配置する（Python 版 draw_anchor_bolt ほかに対応）。センタリング
		// 済みの絶対座標と回転角（度）をそのまま渡す。高さはアクティブレイヤ（＝配置先の
		// ストーリレベル）の平面で決まるので、命令は高さを持たない。
		//
		// **置けたと数えるのは、その名前のシンボルインスタンスが実際にできたときだけ。**
		// 非 nil のハンドルは成功の証拠にならない（ファイル冒頭「成功判定に handle の
		// 非 nil を使わない」）。
		bool PlaceOne(const core::SymbolCommand& command)
		{
			// VWFC の構築子は失敗を例外で伝えるので、1 件の異常で残りの配置を止めない
			// （CLAUDE.md「エラーハンドリング」: SDK コールバックへ例外を漏らさない）。
			try
			{
				const TXString name(command.symbol.c_str());
				const VWSymbolObj symbol(name, VWPoint2D(command.position.x, command.position.y),
										 command.angle);
				MCObjectHandle handle = symbol.GetThisObject();
				if (handle == nil)
					return false;
				return VWSymbolObj::IsSymbolObject(handle, name);
			}
			catch (...)
			{
				return false;
			}
		}

		// 診断行へ名前の一覧を「・」区切りで足す。
		void AppendNames(std::string& text, const std::vector<std::string>& names)
		{
			for (std::size_t i = 0; i < names.size(); ++i)
			{
				if (i > 0)
					text += "・";
				text += names[i];
			}
		}
	} // namespace

	std::size_t drawSymbols(const std::vector<core::SymbolCommand>& commands,
							core::ProgressReporter& progress, const char* elementLabel,
							std::string* note)
	{
		// **配置を始める前に**、命令に出てくる名前の定義の有無を控えておく。配置の途中で
		// 調べると、VWFC が見つからない名前の（空の）定義を作ってしまった場合に「定義は
		// あった」と誤って報告しかねない。名前は 1 要素あたり多くて 2 種類なので安い。
		std::map<std::string, bool> definedBefore;
		for (const core::SymbolCommand& command : commands)
			if (!definedBefore.contains(command.symbol))
				definedBefore.emplace(command.symbol, HasSymbolDefinition(command.symbol));

		std::size_t drawn = 0;
		std::size_t missingLayers = 0;
		std::size_t failed = 0;
		std::vector<std::string> undefinedSymbols; // 図面にシンボル定義が無かった名前
		std::vector<std::string> failedSymbols; // 定義はあるのに配置できなかった名前

		for (const core::SymbolCommand& command : commands)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は件数で報告し、
			// 描画の前に 1 件進める（＝「いま何件目を置いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤが無い命令はスキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(command.layer) == nil)
			{
				++missingLayers;
				continue;
			}

			// **必ず配置を試みる。** 事前ガードで弾かず、置けたかどうかだけで数える
			// （PlaceOne が「その名前のシンボルインスタンスができたか」まで確かめる）。
			if (PlaceOne(command))
			{
				++drawn;
				continue;
			}

			++failed;
			const auto defined = definedBefore.find(command.symbol);
			std::vector<std::string>& bucket = (defined != definedBefore.end() && defined->second)
												   ? failedSymbols
												   : undefinedSymbols;
			if (std::ranges::find(bucket, command.symbol) == bucket.end())
				bucket.push_back(command.symbol);
		}

		// 診断行（何も無ければ空のまま）。「命令はあるのに 0 件」のときに、原因が配置先レイヤ
		// （＝ストーリ側）か、図面にシンボル定義が無いのか（＝リソース側）か、定義はあるのに
		// 配置に失敗したのか（＝描画側）を切り分けられる。
		if (note != nullptr && (missingLayers > 0 || failed > 0))
		{
			std::string text = std::string(elementLabel) + "の診断: ";
			if (missingLayers > 0)
				text += "配置先レイヤが無い命令 " + std::to_string(missingLayers) + " 件。";
			if (failed > 0)
			{
				text += "配置できなかった命令 " + std::to_string(failed) + " 件。";
				if (!undefinedSymbols.empty())
				{
					text += "図面にシンボル定義がありません: ";
					AppendNames(text, undefinedSymbols);
					text += "。";
				}
				if (!failedSymbols.empty())
				{
					text += "シンボル定義はあるのに配置できません: ";
					AppendNames(text, failedSymbols);
					text += "。";
				}
			}
			*note = std::move(text);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
