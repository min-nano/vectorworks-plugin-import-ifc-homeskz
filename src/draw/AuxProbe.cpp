//
//	draw/AuxProbe.cpp
//
//	【一時計装 ── 役目を終えたら消す】ねらいと出力の読み方は draw/AuxProbe.h。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API:
//	  * gSDK->FirstSelectedObject()                     … 選択オブジェクト
//	  * VWDocument::GetDrawingHeaderFristMember() / gSDK->NextObject
//	                                                    … レイヤの走査（SDK の綴りママ）
//	  * gSDK->FirstMemberObj(layer) / NextObject        … レイヤ上のオブジェクトの走査
//	  * gSDK->FirstAuxObject(h) / NextObject(h)         … 補助オブジェクトの連鎖
//	  * gSDK->GetObjectTypeN(h) / GetObjectName(h, out) … 型（76 = データオブジェクト）と名前
//	  * gSDK->GetDataTag(h)                             … データオブジェクトの 4 文字タグ
//	  * gSDK->GSGetHandleSize(h, size)                  … ハンドルの中身の大きさ
//	  * gSDK->TaggedDataGetNumElements / TaggedDataGet  … タグ付きデータの読み取り
//	  * gSDK->TaggedDataGetNumElements / TaggedDataGet
//	                       … **型とタグが分かっている容れ物だけ**を読む（'GrLg' のフィルタ）
//	  * gSDK->InternalIndexToHandle(index)              … オブジェクト参照 → 実体
//	  * VWParametricObj の GetParamsCount / GetParamName / GetParamLocalizedName /
//	    GetParamStyle / GetParamValue                   … パラメトリックレコードの全欄
//
//	【読み取りしかしない】図面は一切変更しない。書き込み側の API（TaggedDataSet /
//	NewDataObject）はここでは呼ばない。
//

#include "PluginPrefix.h"
#include "draw/AuxProbe.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWLayerObj.h"
#include "VWFC/VWObjects/VWDocument.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace HomeskzIfcImport::draw
{
#ifdef VW_DEV_BUILD
	namespace
	{
		// グラフィック凡例の内部プラグイン名（draw/Legend.cpp の kGraphicLegendPlugin と同じ値。
		// **一時計装なのであちらを include して結びつけない**——消すときにこのファイルだけで済む）。
		// **日本語 UI の VW では `GetParametricName()` が表示名を返すことがある**ので、
		// どちらでも当たるように両方持つ（実機で計装が空振りした原因の第 1 候補）。
		constexpr const char* kGraphicLegendPlugin = "GraphicLegend";
		constexpr const char* kGraphicLegendLocalized = "グラフィック凡例";

		// データオブジェクトの型（Kernel/API/Objs.TDType.h の kUserDataNode = 76）と
		// ビューポートの型（Appendix D: Viewport = 122）。
		constexpr short kDataObjectType = 76;
		constexpr short kViewportType = 122;

		// **`'DMDT'` のデータオブジェクトは「入れ物の入れ物」**で、本当のタグ付きデータの
		// 容れ物 ID は中身のこのオフセットに**リトルエンディアンで**入っている（#86 で判明。
		// `67 4c 72 47` を 32 ビットで読むと `'GrLg'` ＝ Graphic Legend）。
		constexpr std::size_t kContainerIdOffset = 86;

		// 16 進の桁。**C の配列は clang-tidy が禁じる**（cppcoreguidelines-avoid-c-arrays）ので
		// string_view で持つ。
		constexpr std::string_view kHexDigits = "0123456789abcdef";

		// 走査と出力の上限（暴走よけ。**タグ ID は 255 まで**——1 度目は 64 までしか見ておらず、
		// 上の番号を使う設定があれば取りこぼしていた）。
		constexpr int kMaxAuxObjects = 64;
		constexpr std::size_t kMaxDumpBytes = 4096;
		constexpr std::size_t kMaxLegends = 32;

		// 16 ビットで読み直して並べる開始位置。`'DMDT'` のデータオブジェクトは +80 に
		// タグ、+86 に本当の容れ物 ID が入っており、その後ろがタグ表と中身になる
		// （docs/DEV-NOTES.md「グラフィック凡例」）。**構造を決め打ちで解かない**
		// ——16 進と 16 ビットの 2 通りで出しておけば、手で読むには足りる。
		constexpr std::size_t kWordDumpStart = 86;
		constexpr std::size_t kMaxWords = 128;

		// TXString → std::string（UTF-8）。
		std::string Std(const TXString& text)
		{
			return text.GetStdString();
		}

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

		// オブジェクトの素性（型と名前）。**参照の行き先がビューポートかどうか**が要るので、
		// 122 には目印を付ける。
		std::string DescribeObject(MCObjectHandle object)
		{
			if (object == nil)
				return "(解決できず)";
			const short type = gSDK->GetObjectTypeN(object);
			TXString name;
			gSDK->GetObjectName(object, name);
			std::string text = "type=" + std::to_string(type);
			if (!name.IsEmpty())
				text += " 「" + Std(name) + "」";
			if (type == kViewportType)
				text += "  <== VIEWPORT";
			return text;
		}

		// 16 進ダンプ（1 行 16 バイト、先頭にオフセット）。
		std::string HexDump(const Uint8* bytes, std::size_t size)
		{
			std::string text;
			for (std::size_t offset = 0; offset < size; offset += 16)
			{
				text += "      +" + std::to_string(offset) + ":";
				for (std::size_t i = offset; i < offset + 16 && i < size; ++i)
				{
					text += ' ';
					text += kHexDigits[(bytes[i] >> 4U) & 0x0FU];
					text += kHexDigits[bytes[i] & 0x0FU];
				}
				text += "\n";
			}
			return text;
		}

		// +86 から先を**16 ビットの並び**として出す。タグ付きデータの型は 6 種類しか無く
		// （byte / uint32 / double / 行列 / ColorRef / オブジェクト参照）、16 ビット幅の値も
		// 文字列も byte 配列に載るので、**中身は 16 ビットで読むと意味が見える**ことが多い
		// （実例: ソース定義の `'GrLe'` は 11 個の 16 ビット値だった）。
		std::string WordDump(const Uint8* bytes, std::size_t size)
		{
			if (size <= kWordDumpStart + 1)
				return {};
			std::string text = "      words(+" + std::to_string(kWordDumpStart) + "):";
			std::size_t count = 0;
			for (std::size_t offset = kWordDumpStart; offset + 1 < size && count < kMaxWords;
				 offset += 2, ++count)
			{
				const auto value = static_cast<Uint32>(bytes[offset]) |
								   (static_cast<Uint32>(bytes[offset + 1]) << 8U);
				text += " " + std::to_string(value);
			}
			return text + "\n";
		}

		// パラメトリックレコードの全欄（universal 名・OIP の表示名・種別・値）。
		// **「イメージの縮率」がどの欄で、いまいくつか**がここで分かる。
		std::string DumpParams(const VWParametricObj& pio)
		{
			std::string text = "  -- パラメトリックレコード --\n";
			const size_t count = pio.GetParamsCount();
			for (size_t i = 0; i < count; ++i)
			{
				const TXString name = pio.GetParamName(i);
				text += "    #" + std::to_string(i) + " " + Std(name) + "(" +
						Std(pio.GetParamLocalizedName(i)) + ")";
				try
				{
					text += " style=" + std::to_string(static_cast<int>(pio.GetParamStyle(name)));
					text += " value=\"" + Std(pio.GetParamValue(name)) + "\"";
				}
				catch (...)
				{
					// 読めない欄（ボタン等）は種別だけ分かれば足りる。
					text += " (値を読めず)";
				}
				text += "\n";
			}
			return text;
		}

		// **型とタグが分かっている容れ物だけ**を `TaggedData*` で読む。ここでは「ビューポート
		// でフィルタ」（容れ物 `'GrLg'` ・型 15 ・タグ 5）——#86 で確定しているので、
		// 何件でも（＝複数のビューポートを指定した凡例でも）実体まで解いて出せる。
		//
		// **総なめはしない。** `TaggedDataGetNumElements` は渡した型 ID を検証しないらしく、
		// 実際に入っている型と違っても同じ件数を返し、`TaggedDataGet` はその型の大きさで
		// 切り出したゴミを返す（1 度これに騙された。docs/DEV-NOTES.md「グラフィック凡例」）。
		std::string DumpFilterTargets(MCObjectHandle legend)
		{
			constexpr OSType kFilterContainer = 0x47724C67; // 'GrLg'
			constexpr Sint32 kFilterType = 15;
			constexpr Sint32 kFilterTag = 5;

			Sint32 count = 0;
			if (!gSDK->TaggedDataGetNumElements(legend, kFilterContainer, kFilterType, kFilterTag,
												&count) ||
				count <= 0)
				return "  -- ビューポートでフィルタ: 無し --\n";

			std::string text = "  -- ビューポートでフィルタ: " + std::to_string(count) + " 件 --\n";
			for (Sint32 index = 0; index < count; ++index)
			{
				InternalIndex reference = 0;
				if (!gSDK->TaggedDataGet(legend, kFilterContainer, kFilterType, kFilterTag, index,
										 &reference))
				{
					text += "    [" + std::to_string(index) + "] (読めず)\n";
					continue;
				}
				text += "    [" + std::to_string(index) + "] " + std::to_string(reference) +
						" -> " + DescribeObject(gSDK->InternalIndexToHandle(reference)) + "\n";
			}
			return text;
		}

		// 補助オブジェクト 1 つ（データオブジェクトなら中身と容れ物も）。
		std::string DumpAuxObject(MCObjectHandle aux, int index)
		{
			const short type = gSDK->GetObjectTypeN(aux);
			std::string text = "  [aux " + std::to_string(index) + "] type=" + std::to_string(type);
			if (type != kDataObjectType)
				return text + "\n";

			const OSType tag = gSDK->GetDataTag(aux);
			text += " data tag='" + FourCC(tag) + "'";

			// 中身の +86 に入っている「本当の容れ物 ID」。これで総なめすると中身が読める。
			std::size_t size = 0;
			gSDK->GSGetHandleSize(aux, size);
			text += " bytes=" + std::to_string(size) + "\n";

			const auto* bytes = reinterpret_cast<const Uint8*>(*aux);
			if (bytes != nullptr && size >= kContainerIdOffset + sizeof(Uint32))
			{
				Uint32 value = 0;
				std::memcpy(&value, bytes + kContainerIdOffset, sizeof(value));
				text += "      container='" + FourCC(static_cast<OSType>(value)) + "'\n";
			}
			if (bytes != nullptr && size > 0)
			{
				const std::size_t shown = size < kMaxDumpBytes ? size : kMaxDumpBytes;
				text += HexDump(bytes, shown);
				text += WordDump(bytes, shown);
			}
			return text;
		}

		// その PIO の登録名（読めなければ空）。**判定にも診断にも使う**——空振りしたときに
		// 「実際は何という名前だったのか」がダンプに残らないと、原因を実機へ聞き直すことに
		// なる。
		std::string ParametricName(MCObjectHandle object)
		{
			try
			{
				const VWParametricObj pio(object);
				return Std(pio.GetParametricName());
			}
			catch (...)
			{
				return {};
			}
		}

		// その PIO がグラフィック凡例か。**universal 名でも表示名でも当たるようにする。**
		bool IsGraphicLegend(MCObjectHandle object)
		{
			const std::string name = ParametricName(object);
			return name == kGraphicLegendPlugin || name == kGraphicLegendLocalized;
		}

		// 文書内のグラフィック凡例をすべて集める（レイヤ → その上のオブジェクトの順に走査）。
		std::vector<MCObjectHandle> CollectLegends()
		{
			std::vector<MCObjectHandle> legends;
			try
			{
				for (MCObjectHandle layer = VWDocument::GetDrawingHeaderFristMember();
					 layer != nil && legends.size() < kMaxLegends; layer = gSDK->NextObject(layer))
				{
					if (!VWLayerObj::IsLayerObject(layer))
						continue;
					for (MCObjectHandle object = gSDK->FirstMemberObj(layer);
						 object != nil && legends.size() < kMaxLegends;
						 object = gSDK->NextObject(object))
					{
						if (IsGraphicLegend(object))
							legends.push_back(object);
					}
				}
			}
			catch (...)
			{
				// 走査中の異常は「そこまでに拾えた凡例だけ返す」で足りる（読み取り専用）。
				return legends;
			}
			return legends;
		}

		// 凡例 1 枚ぶん。
		std::string DumpLegend(MCObjectHandle legend, std::size_t index)
		{
			std::string text = "== [凡例 " + std::to_string(index) + "] " + DescribeObject(legend) +
							   " 登録名=\"" + ParametricName(legend) + "\" internalIndex=" +
							   std::to_string(gSDK->GetObjectInternalIndex(legend)) + " ==\n";
			try
			{
				const VWParametricObj pio(legend);
				text += DumpParams(pio);
			}
			catch (...)
			{
				text += "  (パラメトリックレコードを読めず)\n";
			}

			text += DumpFilterTargets(legend);

			text += "  -- 補助オブジェクト --\n";
			int aux = 0;
			for (MCObjectHandle node = gSDK->FirstAuxObject(legend);
				 node != nil && aux < kMaxAuxObjects; node = gSDK->NextObject(node))
			{
				++aux;
				text += DumpAuxObject(node, aux);
			}
			if (aux == 0)
				text += "  (補助オブジェクトなし)\n";
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
		// **何か 1 つでも選ばれていれば働く。** 以前は「選択がグラフィック凡例なら」と
		// 絞っていたが、実機で空振りした（凡例を選んでいたのにインポートが始まった）。
		// 判定を通す条件を厳しくするほど原因が分からなくなるので、**選択があれば必ず
		// 書き出し、判定に落ちた理由（型・登録名）をダンプの頭に残す**。選択が無ければ
		// 空文字を返し、呼び出し側は普通のインポートへ進む。
		const MCObjectHandle selected = gSDK->FirstSelectedObject();
		if (selected == nil)
			return {};

		const std::vector<MCObjectHandle> legends = CollectLegends();
		std::string text = "選択中のオブジェクト: " + DescribeObject(selected) + " 登録名=\"" +
						   ParametricName(selected) + "\"" +
						   (IsGraphicLegend(selected) ? "（グラフィック凡例と判定）"
													  : "（**凡例と判定できず**）") +
						   "\n文書内のグラフィック凡例: " + std::to_string(legends.size()) +
						   " 枚\n\n";

		std::size_t index = 0;
		bool selectedIsListed = false;
		for (const MCObjectHandle legend : legends)
		{
			++index;
			selectedIsListed = selectedIsListed || legend == selected;
			text += DumpLegend(legend, index);
			text += "\n";
		}

		// 走査で拾えなかった（＝登録名が想定と違う）ときも、選んだものだけは必ず出す。
		if (!selectedIsListed)
		{
			text += DumpLegend(selected, index + 1);
			text += "\n";
		}
		return text;
#endif
	}
} // namespace HomeskzIfcImport::draw
