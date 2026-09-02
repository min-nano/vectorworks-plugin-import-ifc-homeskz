//
//	draw/Symbol.cpp
//
//	シンボル配置の実装。アンカーボルト・床束・火打・仕口の 4 要素が 1 本の実装を共有する
//	（draw/Symbol.h 参照）。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API は ISDK（gSDK）／VWFC の実在シグネチャに合わせている
//	（Vectorworks 2026 SDK。ci-debug の sdk-grep / sdk-ls で確認済み）:
//	  * draw/DrawUtil の ActivateExistingLayer            … 配置先レイヤ（既存のみ）
//	  * draw/DrawUtil の HasSymbolDefinition               … シンボル定義が図面に在るか
//	                                                        （耐力壁の伏図記号と共有）
//	  * VWSymbolObj(name, VWPoint2D(x, y), angleDeg)      … シンボルインスタンスの配置
//	  * VWSymbolObj::IsSymbolObject(handle, name)         … 置けたものが本当にそれか
//	  * gSDK->AddObjectToContainer(handle, layer)         … 配置先レイヤへ入れ直す
//	  * gSDK->MoveObject3D(handle, 0, 0, dz)              … レイヤ平面からの高さ調整（下記）
//	VWSymbolObj のこの構築子が VS の vs.Symbol(name, point, angle) にあたる。
//
//	【VW 2026 SDK の落とし穴】ISDK にシンボルを配置する呼び出しは無く（在るのは
//	CreateSymbolDefinition だけ）、`VWSymbolObj` の構築子はレガシーの PlaceSymbol を包む。
//	ここから 2 つの作法が要る。どちらも実機で「シンボルがひとつも配置できない」ところから
//	切り分けて分かったもので、**外すと静かに壊れる**（docs/DEV-NOTES.md M11）。
//
//	  1. **生成しただけでは図面に現れない。** VWFC の他のラッパー（ポリライン等）と違い、
//	     できたインスタンスはアクティブレイヤに入らない（定義・レイヤ・名前・座標が
//	     すべて揃っていても `GetParentLayer()` が配置先と一致しない）。生成後に
//	     `AddObjectToContainer`（「h を container の末尾へ**移動**する」）で入れ直す。
//	  2. **非 nil のハンドルを成功判定に使わない。** PlaceSymbol は「定義が nil なら何も
//	     しない」仕様で、構築子は何も置けていなくても非 nil のハンドルを返し得る。
//	     `IsSymbolObject(handle, name)` で「その名前のシンボルインスタンスができた」ことを
//	     確かめて初めて 1 件と数える（これが無いと完了ダイアログが全数成功で嘘をつく）。
//
//	【設計上の要点】**シンボル定義はプラグインが作らない。** ハイブリッドシンボル（"
//	アンカーボルト_M12" / "アンカーボルト_M16" / "床束" / "鋼製火打" / "仕口"）はテンプレート
//	やリソースライブラリから供給される前提で、ここは既存のシンボルを名前で置くだけ。
//	定義が無ければ**黙って何も置かない**のではなく、件数を診断行に出して「命令はあるのに見えな
//	い」原因が分かるようにする。
//
//	【高さはレイヤ平面からの相対移動で合わせる】シンボルは構造材・スラブと違い
//	**ストーリバウンド（SetObjectStoryBound）を持てない**ので、命令の高さ（zOffset）は
//	「置いてから 3D で動かす」形でしか反映できない。zOffset は配置先レイヤ平面からの相対 Z
//	（mm）なので、配置直後に `MoveObject3D(handle, 0, 0, zOffset)` を一度だけ呼ぶ
//	（レイヤ平面の絶対 Z を描画側で引き直す必要が無い）。zOffset = 0 の要素（アンカーボルト・
//	床束・火打）では呼ばないので、それらの挙動は従来のまま変わらない。
//
//	**高さは相対移動だけで扱い、配置行列（SetEntityMatrix）には書かない。** 行列は
//	「与えるときは Z が絶対・読み戻すときは Z がレイヤ相対」という混在があり
//	（draw/Rafter.cpp「高さは配置行列の絶対 Z で与える」で切り分け済み）、命令が持つのは
//	レイヤ平面からの相対値なので、行列へ書くと絶対／相対を取り違えてシンボルを地面へ
//	落としかねない。相対移動にはその曖昧さが無い（地中梁の可視ソリッドの位置合わせでも
//	同じ MoveObject3D を使っている。draw/Footing.cpp）。
//
//	配置角度・基準姿勢（火打の 45 度補正が正しいか等）と**仕口の高さ**（傾斜した登り梁の
//	両端・母屋・段差梁）はローカルの VectorWorks で目視確認する（docs/DEV-NOTES.md M11
//	「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Symbol.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWSymbolObj.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 高さ調整をするかどうかの閾値（mm）。丸め誤差で 3D 移動を呼ばない程度に小さく、
		// 図面で見える差より十分小さい値。**平らな梁（offset ≈ 0）は動かさない**。
		constexpr double kZOffsetTol = 0.001;

		// 置いたシンボルをレイヤ平面から zOffset だけ持ち上げる（ファイル冒頭「高さは
		// レイヤ平面からの相対移動で合わせる」）。**相対移動 1 回だけ**で、平面位置
		// （dx / dy）には触らない。
		void RaiseToOffset(MCObjectHandle handle, double zOffset)
		{
			if (std::abs(zOffset) <= kZOffsetTol)
				return;
			gSDK->MoveObject3D(handle, 0.0, 0.0, zOffset);
		}

		// シンボル 1 つを配置する。センタリング済みの絶対座標と回転角（度）をそのまま渡す。
		// 高さの基準はアクティブレイヤ（＝配置先のストーリレベル）の平面で、命令の zOffset
		// がそこからの差（非 0 なのは仕口だけ）。layer は ActivateExistingLayer
		// が返したアクティブレイヤ。
		//
		// **「その名前のシンボルインスタンスができ、配置先レイヤに載った」ときだけ true。**
		// 2 つの作法（生成後に入れ直す／非 nil を成功と見なさない）はファイル冒頭を参照。
		bool PlaceOne(const core::SymbolCommand& command, MCObjectHandle layer)
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
					return false;
				if (symbol.GetParentLayer() != layer)
					gSDK->AddObjectToContainer(handle, layer);
				if (symbol.GetParentLayer() != layer)
					return false;
				// **配置先レイヤへ入れてから**動かす（レイヤ平面が高さの基準になるため）。
				RaiseToOffset(handle, command.zOffset);
				return true;
			}
			catch (...)
			{
				return false;
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

			// **必ず配置を試みる。** 事前ガードで弾かず、置けたかどうかだけで数える。
			if (PlaceOne(command, layer))
			{
				++drawn;
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
