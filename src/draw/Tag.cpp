//
//	draw/Tag.cpp
//
//	断面寸法データタグ描画の実装。意図・規約は draw/Tag.h と parse/Tag.h を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API:
//	  * gSDK->CreateCustomObject("Data Tag", 挿入点, 角度, bInsert) … データタグ PIO の生成
//	  * gSDK->AddViewportAnnotationObject(viewport, object)         … ビューポート注釈へ移す
//	  * gSDK->ResetObject / DeleteObject                            … 反映・後始末
//	  * VectorWorks::Extension::IDataTagSupport（VCOM）
//	      SetDataTagStyle    … データタグスタイル（"断面寸法"）の関連付け
//	      AssociateWithObject … 対象の横架材へ関連付け（Python 版 DT_AssociateWithObj）
//	      UpdateDataTag       … 関連付け後の再計算（Python 版 DT_UpdateTaggedTags）
//
//	【注釈に入らなかったタグは消す】AddViewportAnnotationObject に失敗すると、タグは
//	**生成したときのカレントレイヤ（シートレイヤ）に residue として残る**——図面の上に
//	寸法だけが浮くので、失敗したら必ず削除する。
//

#include "PluginPrefix.h"
#include "draw/Tag.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "Interfaces/VectorWorks/Extension/IDataTagSupport.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <cstddef>
#include <map>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// データタグの内部プラグイン名（Python 版 vw/sheet.py _DATA_TAG_PLUGIN）。VW 標準の
		// データタグツールの universal 名で、表示名（"データタグ"）とは別物。
		constexpr const char* kDataTagPlugin = "Data Tag";

		// 「引出線を表示」パラメータ（既定 ON）。部材の面ちょうどに置いても ON のままだと
		// 引出線が描かれるので OFF にする（Python 版 _LEADER_FIELD / _LEADER_OFF）。
		// universal 名で見つからなければ OIP の日本語名で引き直す（draw/DrawUtil の
		// ResolveParamName。名前が 1 つ違うだけで setter は黙って無視される）。
		constexpr const char* kFieldUseLeader = "Use Leader";
		constexpr const char* kLocalizedUseLeader = "引出線を表示";

		// タグ 1 つを注釈として置く。置けたら true。support は呼び出し側が 1 回だけ作った
		// VCOM のデータタグ支援インターフェース（タグごとに QueryInterface しない）。
		bool PlaceOne(MCObjectHandle viewport, const core::TagCommand& tag, RefNumber style,
					  MCObjectHandle member,
					  const VectorWorks::Extension::IDataTagSupportPtr& support, TagCounts& counts)
		{
			// 第 4 引数 bInsert=true でカレントレイヤへ入る。この後 AddViewportAnnotationObject で
			// 注釈へ移すので、レイヤ上に残るのは失敗したときだけ（下記で消す）。
			const MCObjectHandle object = gSDK->CreateCustomObject(
				TXString(kDataTagPlugin), WorldPt(tag.position.x, tag.position.y), tag.angle, true);
			if (object == nil)
			{
				++counts.failed;
				return false;
			}

			// スタイル（"断面寸法"）。文書に無ければ**スタイル無しで置く**——タグを失うより、
			// 位置だけでも正しいタグを残した方が原因を追いやすい（構造材のプラグイン
			// スタイルと同じ方針。draw/DrawUtil の ResolvePluginStyle）。
			if (style != 0 && support)
				support->SetDataTagStyle(object, style);

			// 引出線を OFF にする。
			try
			{
				VWParametricObj pio(object);
				const TXString param = ResolveParamName(pio, kFieldUseLeader, kLocalizedUseLeader);
				pio.SetParamBool(param, false);
			}
			catch (...)
			{
				// 引出線が残るだけでタグ自体は使えるので、失敗しても続ける。
			}

			gSDK->ResetObject(object);

			// 関連付け先（構造材ツールで描けた横架材）。フォールバックの直線になった横架材は
			// ハンドルが無いので関連付けを省く（Python 版と同じ。タグは置く）。
			if (member != nil && support)
				support->AssociateWithObject(object, member);
			else
				++counts.unassociated;

			// ビューポートの注釈へ移す。入らなければタグを消す（冒頭「注釈に入らなかった
			// タグは消す」）。
			if (!gSDK->AddViewportAnnotationObject(viewport, object))
			{
				gSDK->DeleteObject(object, true);
				++counts.failed;
				return false;
			}

			// 関連付け後の再計算（Python 版 DT_UpdateTaggedTags）。これをしないと、関連付けた
			// 横架材の断面寸法が本文へ流し込まれない。
			if (support)
				support->UpdateDataTag(object);

			++counts.drawn;
			return true;
		}
	} // namespace

	void prepareDataTagPlugin()
	{
		gSDK->DefineCustomObject(TXString(kDataTagPlugin), kCustomObjectPrefNever);
	}

	std::size_t drawViewportTags(MCObjectHandle viewport, const core::ViewportCommand& command,
								 const ObjectHandleTable& memberHandles, TagCounts& counts)
	{
		if (viewport == nil || command.tags.empty())
			return 0;

		// スタイル名 → RefNumber。**タグ 1 つごとに文書のリソースを引き直すと図面の規模なりに
		// 効いてくる**（1 枚の伏図に横架材の本数だけタグが載る）ので、このビューポートの中では
		// 名前ごとに 1 回だけ引く。引けなければ 0＝スタイル無しで置く。
		std::map<std::string, RefNumber> styles;

		// VCOM のデータタグ支援インターフェース（関連付け・スタイル・更新）。ビューポート
		// 1 枚につき 1 回だけ取る。取れなければ**タグは置くが関連付けとスタイルは省く**
		// （位置だけでも正しいタグが残る方が原因を追いやすい）。
		const VectorWorks::Extension::IDataTagSupportPtr support(
			VectorWorks::Extension::IID_DataTagSupport);

		std::size_t drawn = 0;
		for (const core::TagCommand& tag : command.tags)
		{
			auto style = styles.find(tag.style);
			if (style == styles.end())
			{
				const RefNumber resolved = ResolvePluginStyle(TXString(tag.style.c_str()));
				style = styles.emplace(tag.style, resolved).first;
				// 「1 度でも引けなかったか」だけを持ち帰る（診断行が長くならないように）。
				if (resolved == 0)
					counts.styleMissing = true;
			}

			const auto found = memberHandles.handles.find(tag.memberIndex);
			const MCObjectHandle member =
				found == memberHandles.handles.end() ? nil : found->second;

			if (PlaceOne(viewport, tag, style->second, member, support, counts))
				++drawn;
		}
		return drawn;
	}

	std::string tagDiagnostics(const std::string& label, const TagCounts& counts)
	{
		if (counts.failed == 0 && counts.unassociated == 0 && !counts.styleMissing)
			return {};

		std::string text = label + "の断面寸法タグの診断: ";
		if (counts.styleMissing)
			text +=
				"データタグスタイル「断面寸法」が文書にありません（スタイル無しで置きました）。";
		if (counts.failed > 0)
			text += "タグを置けなかった命令 " + std::to_string(counts.failed) + " 件。";
		if (counts.unassociated > 0)
			text += "関連付け先の横架材が無いタグ " + std::to_string(counts.unassociated) +
					" 件（断面寸法が空になります）。";
		return text;
	}
} // namespace HomeskzIfcImport::draw
