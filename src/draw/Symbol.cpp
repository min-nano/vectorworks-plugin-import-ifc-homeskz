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
		// シンボル定義が図面に在るか。**配置を止める門にはしない**（下記 drawSymbols）——
		// 配置に失敗した理由を診断に書き分けるためだけに使う。同じ名前を何百回も問い合わせ
		// ないよう 1 回の描画の中で覚えておく（1 要素のシンボル名は多くて 2 種類）。
		bool HasSymbolDefinition(std::map<std::string, bool>& cache, const std::string& name)
		{
			const auto found = cache.find(name);
			if (found != cache.end())
				return found->second;
			bool exists = false;
			try
			{
				exists = VWSymbolDefObj::IsSymbolDefObject(TXString(name.c_str()));
			}
			catch (...)
			{
				exists = false;
			}
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
		std::map<std::string, bool> symbolExists;
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

			// **必ず配置を試みる。** かつてはここで IsSymbolDefObject による事前ガードを
			// 掛けていたが、その判定が期待どおりでないと**1 つも置けないのに「シンボルが
			// 見つかりません」と報告する**（原因を誤って指す）形になる。配置そのものを
			// 唯一の判定にし、失敗したときだけ理由を調べて書き分ける。
			if (PlaceOne(command))
			{
				++drawn;
				continue;
			}

			++failed;
			std::vector<std::string>& bucket = HasSymbolDefinition(symbolExists, command.symbol)
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
