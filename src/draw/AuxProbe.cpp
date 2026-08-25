//
//	draw/AuxProbe.cpp
//
//	【一時計装 ── 役目を終えたら消す】ねらいと出力の読み方は draw/AuxProbe.h。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API:
//	  * gSDK->FirstSelectedObject()                     … 選択オブジェクト
//	  * gSDK->FirstAuxObject(h) / NextObject(h)         … 補助オブジェクトの連鎖
//	    （GS_FirstAuxObject の説明どおり「先頭から NextObject で次へ」）
//	  * gSDK->GetObjectTypeN(h)                         … 型（76 = データオブジェクト）
//	  * gSDK->GetDataTag(h)                             … データオブジェクトの 4 文字タグ
//	  * gSDK->TaggedDataGetNumElements / TaggedDataGet  … タグ付きデータの読み取り
//	  * gSDK->InternalIndexToHandle(index)              … オブジェクト参照 → 実体
//
//	【読み取りしかしない】図面は一切変更しない。書き込み側の API（TaggedDataSet /
//	NewDataObject）はここでは呼ばない。
//

#include "PluginPrefix.h"
#include "draw/AuxProbe.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

namespace HomeskzIfcImport::draw
{
#ifdef VW_DEV_BUILD
	namespace
	{
		// グラフィック凡例の内部プラグイン名（draw/Legend.cpp の kGraphicLegendPlugin と同じ値。
		// **一時計装なのであちらを include して結びつけない**——消すときにこのファイルだけで済む）。
		constexpr const char* kGraphicLegendPlugin = "GraphicLegend";

		// データオブジェクトの型（Kernel/API/Objs.TDType.h の kUserDataNode = 76）。
		constexpr short kDataObjectType = 76;

		// タグ付きデータの「オブジェクト参照の配列」型 ID
		//（Kernel/API/MiniCadCallBacks.h の kTaggedDataObjectRefArrayTypeID）。
		constexpr Sint32 kObjectRefTypeID = 15;

		// 走査の上限（暴走よけ）。タグ ID は総なめするので広めに、値は先頭だけ見れば足りる。
		constexpr int kMaxAuxObjects = 64;
		constexpr Sint32 kMaxTaggedTagID = 64;
		constexpr Sint32 kMaxValuesPerTag = 8;

		// タグ付きデータの型 ID（Kernel/API/MiniCadCallBacks.h）。**要素の大きさが分かっている
		// ものだけ**並べる——行列や点の配列まで読むとバッファの扱いが増えるうえ、
		// 「フィルタ先のビューポート」がそこに入っている見込みは薄い。
		struct TaggedType
		{
			Sint32 id = 0;
			const char* name = "";
			std::size_t size = 0;
		};
		constexpr std::array<TaggedType, 8> kTaggedTypes = {{
			{1, "byte", sizeof(Uint8)},
			{2, "flags", sizeof(Sint32)},
			{6, "double", sizeof(double)},
			{8, "uint32", sizeof(Sint32)},
			{13, "colorref", sizeof(Sint32)},
			{kObjectRefTypeID, "objectref", sizeof(Sint32)},
			{17, "planarref", sizeof(Sint32)},
			{23, "refnumber", sizeof(Sint32)},
		}};

		// OSType（4 文字タグ）を読める形にする。印字できない文字は '?' に落とす。
		std::string FourCC(OSType tag)
		{
			const auto bits = static_cast<Uint32>(tag);
			std::string text;
			for (int shift = 24; shift >= 0; shift -= 8)
			{
				const auto code = static_cast<char>((bits >> static_cast<Uint32>(shift)) & 0xFFU);
				text += (code >= 0x20 && code < 0x7F) ? code : '?';
			}
			return text;
		}

		// タグ付きデータの 1 要素を読める形にする。**オブジェクト参照は実体の型まで見せる**
		// ——そこが 122（ビューポート）なら、それがフィルタ先だという確証になる。
		std::string FormatValue(const TaggedType& type, const std::array<Uint8, 64>& buffer)
		{
			if (type.size == sizeof(double))
			{
				double value = 0.0;
				std::memcpy(&value, buffer.data(), sizeof(value));
				return std::to_string(value);
			}
			if (type.size == sizeof(Sint32))
			{
				Sint32 value = 0;
				std::memcpy(&value, buffer.data(), sizeof(value));
				std::string text = std::to_string(value);
				if (type.id == kObjectRefTypeID)
				{
					const MCObjectHandle referenced =
						gSDK->InternalIndexToHandle(static_cast<InternalIndex>(value));
					text += referenced != nil
								? " -> type=" + std::to_string(gSDK->GetObjectTypeN(referenced))
								: " -> (解決できず)";
				}
				return text;
			}
			return std::to_string(static_cast<int>(buffer[0]));
		}

		// データオブジェクトのタグを**タグ付きデータの容れ物**とみなして総なめする。
		// 要素数が返る（＝そこに何か入っている）組み合わせだけを書き出す。
		std::string ProbeTaggedContainer(MCObjectHandle owner, OSType container)
		{
			std::string text;
			for (const TaggedType& type : kTaggedTypes)
			{
				for (Sint32 tagID = 0; tagID <= kMaxTaggedTagID; ++tagID)
				{
					Sint32 count = 0;
					if (!gSDK->TaggedDataGetNumElements(owner, container, type.id, tagID, &count))
						continue;
					if (count <= 0)
						continue;

					text += "    tagged type=" + std::string(type.name) +
							" tag=" + std::to_string(tagID) + " count=" + std::to_string(count) +
							" values:";
					for (Sint32 index = 0; index < count && index < kMaxValuesPerTag; ++index)
					{
						std::array<Uint8, 64> buffer{};
						if (!gSDK->TaggedDataGet(owner, container, type.id, tagID, index,
												 buffer.data()))
						{
							text += " (読めず)";
							break;
						}
						text += " " + FormatValue(type, buffer);
					}
					text += "\n";
				}
			}
			return text;
		}
	} // namespace
#endif

	std::string probeSelectedLegendAuxData()
	{
#ifndef VW_DEV_BUILD
		// stable では計装を動かさない（ユーザーの手元で余計なことをしない）。
		return {};
#else
		const MCObjectHandle selected = gSDK->FirstSelectedObject();
		if (selected == nil)
			return {};

		// **グラフィック凡例を選んだときだけ**働く。他のものが選ばれていても、
		// 呼び出し側が普通のインポートを続けられるように空文字を返す。
		TXString pluginName;
		try
		{
			const VWParametricObj legend(selected);
			pluginName = legend.GetParametricName();
		}
		catch (...)
		{
			return {};
		}
		if (pluginName != TXString(kGraphicLegendPlugin))
			return {};

		std::string text =
			"選択: グラフィック凡例 (type=" + std::to_string(gSDK->GetObjectTypeN(selected)) +
			" internalIndex=" + std::to_string(gSDK->GetObjectInternalIndex(selected)) + ")\n";

		int index = 0;
		for (MCObjectHandle aux = gSDK->FirstAuxObject(selected);
			 aux != nil && index < kMaxAuxObjects; aux = gSDK->NextObject(aux))
		{
			++index;
			const short type = gSDK->GetObjectTypeN(aux);
			text += "[aux " + std::to_string(index) + "] type=" + std::to_string(type);
			if (type != kDataObjectType)
			{
				text += "\n";
				continue;
			}

			const OSType tag = gSDK->GetDataTag(aux);
			text += " data tag='" + FourCC(tag) + "'\n";
			text += ProbeTaggedContainer(selected, tag);
		}
		if (index == 0)
			text += "(補助オブジェクトなし)\n";
		return text;
#endif
	}
} // namespace HomeskzIfcImport::draw
