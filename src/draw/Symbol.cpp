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
//	VWSymbolObj のこの構築子が VS の vs.Symbol(name, point, angle) にあたる。
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
		// シンボル定義が図面に在るか。**同じ名前を何百回も問い合わせない**よう 1 回の
		// 描画の中で覚えておく（1 要素のシンボル名は多くて 2 種類なので、地図は極小）。
		bool HasSymbolDefinition(std::map<std::string, bool>& cache, const std::string& name)
		{
			const auto found = cache.find(name);
			if (found != cache.end())
				return found->second;
			const bool exists = VWSymbolDefObj::IsSymbolDefObject(TXString(name.c_str()));
			cache.emplace(name, exists);
			return exists;
		}

		// シンボル 1 つを配置する（Python 版 draw_anchor_bolt ほかに対応）。センタリング
		// 済みの絶対座標と回転角（度）をそのまま渡す。高さはアクティブレイヤ（＝配置先の
		// ストーリレベル）の平面で決まるので、命令は高さを持たない。
		bool PlaceOne(const core::SymbolCommand& command)
		{
			// VWFC の構築子は失敗を例外で伝えるので、1 件の異常で残りの配置を止めない
			// （CLAUDE.md「エラーハンドリング」: SDK コールバックへ例外を漏らさない）。
			try
			{
				const VWSymbolObj symbol(TXString(command.symbol.c_str()),
										 VWPoint2D(command.position.x, command.position.y),
										 command.angle);
				return symbol.GetThisObject() != nil;
			}
			catch (...)
			{
				return false;
			}
		}
	} // namespace

	std::size_t drawSymbols(const std::vector<core::SymbolCommand>& commands,
							core::ProgressReporter& progress, const char* elementLabel,
							std::string* note)
	{
		std::map<std::string, bool> symbolExists;
		std::size_t drawn = 0;
		std::size_t missingLayers = 0;
		std::vector<std::string> missingSymbols;

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

			// シンボル定義が無ければ置けない。名前を 1 度だけ診断へ残す。
			if (!HasSymbolDefinition(symbolExists, command.symbol))
			{
				if (std::find(missingSymbols.begin(), missingSymbols.end(), command.symbol) ==
					missingSymbols.end())
					missingSymbols.push_back(command.symbol);
				continue;
			}

			if (PlaceOne(command))
				++drawn;
		}

		// 診断行（何も無ければ空のまま）。「命令はあるのに 0 件」のときに、原因が
		// 配置先レイヤ（＝ストーリ側）かシンボル定義（＝リソース側）かを切り分けられる。
		if (note != nullptr && (missingLayers > 0 || !missingSymbols.empty()))
		{
			std::string text = std::string(elementLabel) + "の診断: ";
			if (missingLayers > 0)
				text += "配置先レイヤが無い命令 " + std::to_string(missingLayers) + " 件。";
			if (!missingSymbols.empty())
			{
				text += "シンボルが見つかりません: ";
				for (std::size_t i = 0; i < missingSymbols.size(); ++i)
				{
					if (i > 0)
						text += "・";
					text += missingSymbols[i];
				}
				text += "。";
			}
			*note = std::move(text);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
