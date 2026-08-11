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
//	  * gSDK->AddObjectToContainer(handle, layer)         … 配置先レイヤへ入れ直す
//	VWSymbolObj のこの構築子が VS の vs.Symbol(name, point, angle) にあたる。
//
//	【生成しただけでは図面に現れない】ISDK にシンボルを配置する呼び出しは無く（在るのは
//	CreateSymbolDefinition だけ）、この構築子はレガシーの PlaceSymbol を包む。VWFC の他の
//	ラッパー（ポリライン等）と違って**できたインスタンスはアクティブレイヤに入らない**——
//	ローカル実測で、シンボル定義もレイヤも名前も揃っているのに 4 種 472 件すべてで
//	GetParentLayer が配置先と一致せず、図面には 1 つも現れなかった。そこで生成後に
//	AddObjectToContainer（「h を container の末尾へ**移動**する」）で配置先レイヤへ
//	入れ直す。
//
//	【成功判定に handle の非 nil を使わない】同じ理由で、構築子は**何も置けていなくても
//	非 nil のハンドルを返し得る**（PlaceSymbol は「定義が nil なら何もしない」仕様）。
//	実際に「アンカーボルト 97 本…を描きました」と全数成功で報告しながら図面には 1 つも
//	出ていない、という報告を受けた。生成したハンドルが**その名前のシンボルインスタンスで
//	あること**まで確かめて初めて 1 件と数える。
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

		// 1 件の配置の結末。**非 nil のハンドルは成功の証拠にならない**ので（ファイル冒頭
		// 「成功判定に handle の非 nil を使わない」）、どこまで進んだかを 3 つに分けて返す。
		enum class PlaceResult
		{
			NotPlaced,	// そもそもインスタンスができていない
			StrayLayer, // できたが、入れ直しても意図したレイヤに載らない
			Placed,		// できて、意図したレイヤに載った
		};

		// シンボル 1 つを配置する（Python 版 draw_anchor_bolt ほかに対応）。センタリング
		// 済みの絶対座標と回転角（度）をそのまま渡す。高さはアクティブレイヤ（＝配置先の
		// ストーリレベル）の平面で決まるので、命令は高さを持たない。
		//
		// layer は ActivateExistingLayer が返したアクティブレイヤ。**生成しただけでは
		// ここへ入らない**ので、入っていなければ入れ直す（下記）。
		PlaceResult PlaceOne(const core::SymbolCommand& command, MCObjectHandle layer)
		{
			// VWFC の構築子は失敗を例外で伝えるので、1 件の異常で残りの配置を止めない
			// （CLAUDE.md「エラーハンドリング」: SDK コールバックへ例外を漏らさない）。
			try
			{
				const TXString name(command.symbol.c_str());
				const VWSymbolObj symbol(name, VWPoint2D(command.position.x, command.position.y),
										 command.angle);
				const MCObjectHandle handle = symbol.GetThisObject();
				if (handle == nil || !VWSymbolObj::IsSymbolObject(handle, name))
					return PlaceResult::NotPlaced;

				// **配置先レイヤへ入れ直す。** VWFC の他のラッパー（ポリライン等）と違い、
				// VWSymbolObj はレガシーの PlaceSymbol を包んでいて、できたインスタンスは
				// アクティブレイヤに入らない（実測: 4 種 472 件すべてで GetParentLayer が
				// 配置先と不一致。図面には 1 つも現れなかった）。AddObjectToContainer は
				// 「h を container の末尾へ**移動**する」呼び出しなので、これで載せ替える。
				if (symbol.GetParentLayer() != layer)
					gSDK->AddObjectToContainer(handle, layer);
				return (symbol.GetParentLayer() == layer) ? PlaceResult::Placed
														  : PlaceResult::StrayLayer;
			}
			catch (...)
			{
				return PlaceResult::NotPlaced;
			}
		}

		// 診断へ残す名前は 1 度だけ（同じ名前が何百件も並ばないように）。**参照を三項演算子で
		// 束ねてから push_back しない**——clang-tidy の misc-const-correctness がその形の変更を
		// 見落とし、束ねた先の vector に const を要求してくる（CI の tidy-mac / tidy-windows）。
		void RememberOnce(std::vector<std::string>& names, const std::string& name)
		{
			if (std::ranges::find(names, name) == names.end())
				names.push_back(name);
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
		std::size_t wrongLayer = 0; // できたが意図したレイヤに載らなかった
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
			const MCObjectHandle layer = ActivateExistingLayer(command.layer);
			if (layer == nil)
			{
				++missingLayers;
				continue;
			}

			// **必ず配置を試みる。** 事前ガードで弾かず、置けたかどうかだけで数える
			// （PlaceOne が「その名前のシンボルインスタンスができ、そのレイヤに載ったか」
			// まで確かめ、載っていなければ載せ直す）。載せ直しても駄目だったものは
			// 「置けてはいる」ので drawn に数え、件数だけ診断へ出す（図面に見えない
			// 原因になり得るため）。
			const PlaceResult result = PlaceOne(command, layer);
			if (result != PlaceResult::NotPlaced)
			{
				++drawn;
				if (result == PlaceResult::StrayLayer)
					++wrongLayer;
				continue;
			}

			++failed;
			const auto defined = definedBefore.find(command.symbol);
			if (defined != definedBefore.end() && defined->second)
				RememberOnce(failedSymbols, command.symbol);
			else
				RememberOnce(undefinedSymbols, command.symbol);
		}

		// 診断行（何も無ければ空のまま）。「命令はあるのに 0 件」のときに、原因が配置先レイヤ
		// （＝ストーリ側）か、図面にシンボル定義が無いのか（＝リソース側）か、定義はあるのに
		// 配置に失敗したのか（＝描画側）を切り分けられる。
		if (note != nullptr && (missingLayers > 0 || failed > 0 || wrongLayer > 0))
		{
			std::string text = std::string(elementLabel) + "の診断: ";
			if (missingLayers > 0)
				text += "配置先レイヤが無い命令 " + std::to_string(missingLayers) + " 件。";
			if (wrongLayer > 0)
				text +=
					"意図したレイヤに載らなかったシンボル " + std::to_string(wrongLayer) + " 件。";
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
