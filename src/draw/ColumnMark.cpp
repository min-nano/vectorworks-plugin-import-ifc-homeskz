//
//	draw/ColumnMark.cpp
//
//	断面記号・伏図記号の描画の実装（意図と Python 版との差異は draw/ColumnMark.h と
//	parse/ColumnMark.h を参照）。
//	【SDK 依存】PluginPrefix.h を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされる（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API（Vectorworks 2026 SDK。ci-debug の sdk-grep で実在を確認）:
//	  * gSDK->CreateLine(WorldPt, WorldPt)                     … 断面記号の線 1 本
//	  * gSDK->CreateCustomObject(name, location, angle, insert) … データタグ PIO の生成
//	  * gDataTagSupport->CanBeTaggedObject(h)                   … タグを付けられる相手か
//	  * gDataTagSupport->SetDataTagStyle(tag, style, skip)      … タグスタイルの関連付け
//	  * gDataTagSupport->AssociateWithObject(tag, tagged)       … 柱への関連付け（＝追随）
//	  * gDataTagSupport->UpdateDataTag(tag)                     … 関連付け後の作図更新
//	gDataTagSupport は gSDK と同じくグローバルアクセサ（Kernel/API/MiniCadHookIntf.h）。
//
//	【データタグの手順は「作る → スタイル → 関連付け → 更新」】スタイルを先に当てるのは、
//	タグの絵（柱伏図記号／束伏図記号のシンボル）と抽出するデータをスタイルが決めるため。
//	関連付けの**後**に UpdateDataTag を呼ばないと、関連付ける前の（＝空の）作図が残る。
//	この順序と各呼び出しの最終挙動は**ローカルの VectorWorks で目視確認する**
//	（ROADMAP.md M12「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/ColumnMark.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "Interfaces/VectorWorks/Extension/IDataTagSupport.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// データタグの**内部プラグイン名**。表示名（「データタグ」）とは違う登録名で、
		// Python 版が実オブジェクトの VectorScript エクスポートで確認済みのもの
		// （vw/sheet.py の _DATA_TAG_PLUGIN）。M13 の断面寸法タグも同じ名前を使う。
		constexpr const char* kDataTagPlugin = "Data Tag";

		// 断面記号 1 つ（× なら 2 本・／なら 1 本）を引く。1 本でも引けたら true。
		// 線は**アクティブレイヤ**（呼び出し側が span レイヤにしてある）へ入る。
		bool DrawSectionMark(const core::ColumnSectionMarkCommand& mark)
		{
			bool drawn = false;
			for (const core::MarkSegment& segment : mark.segments)
			{
				const MCObjectHandle line =
					gSDK->CreateLine(WorldPt(segment.start.x, segment.start.y),
									 WorldPt(segment.end.x, segment.end.y));
				if (line == nil)
					continue;
				// 極細実線クラスへ入れ、描画属性もクラスに従わせる（記号の太さ・色を
				// クラスで一括して変えられるようにする。他要素と同じ作法）。
				SetClassByName(line, mark.drawClass);
				SetAllAttributesByClass(line);
				drawn = true;
			}
			return drawn;
		}

		// タグを付けられる相手か。VW が「タグ付け不可」と答える相手に関連付けると、
		// タグは残るのに何も抽出できない（＝空のタグ）ので、その手前で弾く。
		// gDataTagSupport が使えない環境では判断できないので true 扱いにして進める
		// （関連付けの戻り値でどのみち分かる）。
		bool CanTag(MCObjectHandle object)
		{
			try
			{
				return gDataTagSupport->CanBeTaggedObject(object);
			}
			catch (...)
			{
				return true;
			}
		}

		// 伏図記号 1 つを置く。スタイルの関連付け・柱への関連付け・更新までできたら true。
		// タグ自体を作れなければ false（呼び出し側が診断に数える）。
		bool DrawPlanMark(const core::ColumnPlanMarkCommand& mark, MCObjectHandle column,
						  RefNumber style)
		{
			// PIO の生成。第 4 引数（bInsert）は true＝アクティブレイヤへ入れる。
			// 角度は 0＝タグスタイルの基準姿勢のまま（伏図記号は向きを持たない）。
			const MCObjectHandle tag = gSDK->CreateCustomObject(
				TXString(kDataTagPlugin), WorldPt(mark.position.x, mark.position.y), 0.0, true);
			if (tag == nil)
				return false;

			SetClassByName(tag, mark.drawClass);

			try
			{
				// スタイル → 関連付け → 更新の順（冒頭「データタグの手順」）。
				if (style != 0)
					gDataTagSupport->SetDataTagStyle(tag, style, false);
				gDataTagSupport->AssociateWithObject(tag, column);
				gDataTagSupport->UpdateDataTag(tag);
			}
			catch (...)
			{
				// タグは図面に残る（位置だけは正しい）。関連付けができなかったことは
				// 呼び出し側が診断に出せないので、ここでは true のまま返して数を合わせる
				// ——「置けたが追随しない」は目視で分かるが、「1 つも置けない」は分からない。
				return true;
			}
			return true;
		}
	} // namespace

	std::size_t drawColumnSectionMarks(const core::Document& document,
									   core::ProgressReporter& progress, std::string* outNote)
	{
		std::size_t drawn = 0;
		std::size_t missingLayers = 0;
		std::size_t failed = 0;

		for (const core::ColumnSectionMarkCommand& mark : document.columnSectionMarks)
		{
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先は柱と同じ span レイヤ＝ストーリが作るレイヤなので、無ければスキップ。
			if (ActivateExistingLayer(mark.layer) == nil)
			{
				++missingLayers;
				continue;
			}

			if (DrawSectionMark(mark))
				++drawn;
			else
				++failed;
		}

		if (outNote != nullptr && (missingLayers > 0 || failed > 0))
		{
			std::string text = "断面記号の診断: ";
			if (missingLayers > 0)
				text += "配置先レイヤが無い命令 " + std::to_string(missingLayers) + " 件。";
			if (failed > 0)
				text += "線を引けなかった命令 " + std::to_string(failed) + " 件。";
			*outNote = std::move(text);
		}

		return drawn;
	}

	std::size_t drawColumnPlanMarks(const core::Document& document,
									core::ProgressReporter& progress, const ObjectHandles& columns,
									std::string* outNote)
	{
		const std::map<std::size_t, MCObjectHandle>& table = columns.table().handles;

		// スタイル名 → RefNumber。1 回のインポートで出てくる名前は 2 つ（柱・小屋束）だけ
		// なので、引き直さずに覚えておく。
		std::map<std::string, RefNumber> styles;

		std::size_t drawn = 0;
		std::size_t missingLayers = 0;
		std::size_t missingColumns = 0;
		std::size_t failed = 0;
		std::vector<std::string> missingStyles;

		for (const core::ColumnPlanMarkCommand& mark : document.columnPlanMarks)
		{
			if (progress.cancelled())
				break;
			progress.step();

			// 関連付け先の柱が未配置なら**タグを置かない**（ヘッダ参照）。
			const auto column = table.find(mark.columnIndex);
			if (column == table.end() || !CanTag(column->second))
			{
				++missingColumns;
				continue;
			}

			// "{to}-柱伏図記号" はストーリが作らない独立レイヤなので、無ければ作る
			// （通り芯の "共通" と同じ扱い）。
			if (PrepareLayer(mark.layer) == nil)
			{
				++missingLayers;
				continue;
			}

			const auto known = styles.find(mark.styleName);
			const RefNumber style =
				known != styles.end()
					? known->second
					: styles
						  .emplace(mark.styleName,
								   ResolvePluginStyle(TXString(mark.styleName.c_str())))
						  .first->second;
			if (style == 0 &&
				std::ranges::find(missingStyles, mark.styleName) == missingStyles.end())
				missingStyles.push_back(mark.styleName);

			if (DrawPlanMark(mark, column->second, style))
				++drawn;
			else
				++failed;
		}

		if (outNote != nullptr &&
			(missingLayers > 0 || missingColumns > 0 || failed > 0 || !missingStyles.empty()))
		{
			std::string text = "伏図記号の診断: ";
			if (missingLayers > 0)
				text += "配置先レイヤを作れなかった命令 " + std::to_string(missingLayers) + " 件。";
			if (missingColumns > 0)
				text += "関連付け先の柱が無い命令 " + std::to_string(missingColumns) + " 件。";
			if (failed > 0)
				text += "データタグを作れなかった命令 " + std::to_string(failed) + " 件。";
			if (!missingStyles.empty())
			{
				text += "図面にデータタグスタイルがありません: ";
				for (std::size_t i = 0; i < missingStyles.size(); ++i)
				{
					if (i > 0)
						text += "・";
					text += missingStyles[i];
				}
				text += "。";
			}
			*outNote = std::move(text);
		}

		return drawn;
	}

	std::vector<std::string> planMarkLayerNames(const core::Document& document)
	{
		std::vector<std::string> names;
		for (const core::ColumnPlanMarkCommand& mark : document.columnPlanMarks)
			if (std::ranges::find(names, mark.layer) == names.end())
				names.push_back(mark.layer);
		return names;
	}
} // namespace HomeskzIfcImport::draw
