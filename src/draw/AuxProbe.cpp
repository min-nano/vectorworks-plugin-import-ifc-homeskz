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
#include "VWFC/VWObjects/VWDocumentObj.h"

#include <array>
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
		constexpr const char* kGraphicLegendPlugin = "GraphicLegend";

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
		constexpr Sint32 kMaxTaggedTagID = 255;
		constexpr Sint32 kMaxValuesPerTag = 32;
		constexpr std::size_t kMaxDumpBytes = 512;
		constexpr std::size_t kMaxTextBytes = 1024;
		constexpr std::size_t kMaxLegends = 32;
		constexpr std::size_t kMaxValueBufferBytes = 64;

		// タグ付きデータの型 ID（Kernel/API/MiniCadCallBacks.h）。**要素の大きさが 0 のものは
		// 件数だけ**出す（行列・点列まで解くと読み取りの手間の割に得るものが無い）。
		// **文字列専用の型 ID は無い**——`CTaggedDataContainer::CreateTagString` は byte 配列
		// （型 1）に載せるので、型 1 を文字列としても出すのが今回の肝（draw/AuxProbe.h）。
		struct TaggedType
		{
			Sint32 id = 0;
			const char* name = "";
			std::size_t size = 0;
		};
		constexpr std::array<TaggedType, 13> kTaggedTypes = {{
			{1, "byte/string", sizeof(Uint8)},
			{2, "flags", sizeof(Sint32)},
			{6, "double", sizeof(double)},
			{7, "matrix", 0},
			{8, "uint32", sizeof(Sint32)},
			{13, "colorref", sizeof(Sint32)},
			{15, "objectref", sizeof(Sint32)},
			{17, "planarref", sizeof(Sint32)},
			{18, "matrixarray", 0},
			{20, "worldpt3", 0},
			{21, "floatpt3", 0},
			{22, "wallcompjoin", 0},
			{23, "refnumber", sizeof(Sint32)},
		}};

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

		// バイト列を「読める文字」として出す。印字できないものは '.' に落とす
		// （**UTF-16 のときは 1 バイトおきに 0 が入る**ので、そのまま出しても "H.e.l.l.o." の
		// 形で読める＝どちらの符号化でも人には判る）。
		std::string AsText(const std::vector<Uint8>& bytes)
		{
			std::string text;
			for (std::size_t i = 0; i < bytes.size() && i < kMaxTextBytes; ++i)
			{
				const Uint8 value = bytes[i];
				text += (value >= 0x20 && value < 0x7F) ? static_cast<char>(value) : '.';
			}
			return text;
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

		// タグ付きデータの 1 つの容れ物を総なめする。要素数が返る（＝そこに何か入っている）
		// 組み合わせだけを書き出す。
		std::string DumpTaggedContainer(MCObjectHandle owner, OSType container)
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

					text += "    [" + FourCC(container) + "] type=" + std::to_string(type.id) +
							"(" + type.name + ") tag=" + std::to_string(tagID) +
							" count=" + std::to_string(count) + "\n";
					if (type.size == 0)
						continue;

					// byte 配列は**文字列としても**出す（ここに検索条件が載っている見込み）。
					if (type.size == sizeof(Uint8))
					{
						std::vector<Uint8> bytes;
						for (Sint32 index = 0; index < count && bytes.size() < kMaxTextBytes;
							 ++index)
						{
							Uint8 value = 0;
							if (!gSDK->TaggedDataGet(owner, container, type.id, tagID, index,
													 &value))
								break;
							bytes.push_back(value);
						}
						if (bytes.empty())
							continue;
						text += "      text=\"" + AsText(bytes) + "\"\n";
						text += HexDump(bytes.data(), bytes.size());
						continue;
					}

					text += "      values:";
					for (Sint32 index = 0; index < count && index < kMaxValuesPerTag; ++index)
					{
						std::array<Uint8, kMaxValueBufferBytes> buffer{};
						if (!gSDK->TaggedDataGet(owner, container, type.id, tagID, index,
												 buffer.data()))
						{
							text += " (読めず)";
							break;
						}
						if (type.size == sizeof(double))
						{
							double value = 0.0;
							std::memcpy(&value, buffer.data(), sizeof(value));
							text += " " + std::to_string(value);
							continue;
						}
						Sint32 value = 0;
						std::memcpy(&value, buffer.data(), sizeof(value));
						text += " " + std::to_string(value);
						if (type.id == 15)
						{
							text += " -> " + DescribeObject(gSDK->InternalIndexToHandle(
												 static_cast<InternalIndex>(value)));
						}
					}
					text += "\n";
				}
			}
			return text;
		}

		// 補助オブジェクト 1 つ（データオブジェクトなら中身と容れ物も）。
		std::string DumpAuxObject(MCObjectHandle owner, MCObjectHandle aux, int index)
		{
			const short type = gSDK->GetObjectTypeN(aux);
			std::string text = "  [aux " + std::to_string(index) + "] type=" + std::to_string(type);
			if (type != kDataObjectType)
				return text + "\n";

			const OSType tag = gSDK->GetDataTag(aux);
			text += " data tag='" + FourCC(tag) + "'";

			// 中身の +86 に入っている「本当の容れ物 ID」。これで総なめすると中身が読める。
			OSType embedded = 0;
			std::size_t size = 0;
			gSDK->GSGetHandleSize(aux, size);
			text += " bytes=" + std::to_string(size) + "\n";

			const auto* bytes = reinterpret_cast<const Uint8*>(*aux);
			if (bytes != nullptr && size >= kContainerIdOffset + sizeof(Uint32))
			{
				Uint32 value = 0;
				std::memcpy(&value, bytes + kContainerIdOffset, sizeof(value));
				embedded = static_cast<OSType>(value);
				text += "      container='" + FourCC(embedded) + "'\n";
			}
			if (bytes != nullptr && size > 0)
				text += HexDump(bytes, size < kMaxDumpBytes ? size : kMaxDumpBytes);

			text += DumpTaggedContainer(owner, tag);
			if (embedded != 0 && embedded != tag)
				text += DumpTaggedContainer(owner, embedded);
			return text;
		}

		// その PIO がグラフィック凡例か。
		bool IsGraphicLegend(MCObjectHandle object)
		{
			try
			{
				const VWParametricObj pio(object);
				return pio.GetParametricName() == TXString(kGraphicLegendPlugin);
			}
			catch (...)
			{
				return false;
			}
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
			std::string text =
				"== [凡例 " + std::to_string(index) + "] " + DescribeObject(legend) +
				" internalIndex=" + std::to_string(gSDK->GetObjectInternalIndex(legend)) + " ==\n";
			try
			{
				const VWParametricObj pio(legend);
				text += DumpParams(pio);
			}
			catch (...)
			{
				text += "  (パラメトリックレコードを読めず)\n";
			}

			text += "  -- 補助オブジェクト --\n";
			int aux = 0;
			for (MCObjectHandle node = gSDK->FirstAuxObject(legend);
				 node != nil && aux < kMaxAuxObjects; node = gSDK->NextObject(node))
			{
				++aux;
				text += DumpAuxObject(legend, node, aux);
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
		// **グラフィック凡例を選んだときだけ**働く。他のものが選ばれていても、
		// 呼び出し側が普通のインポートを続けられるように空文字を返す。
		const MCObjectHandle selected = gSDK->FirstSelectedObject();
		if (selected == nil || !IsGraphicLegend(selected))
			return {};

		const std::vector<MCObjectHandle> legends = CollectLegends();
		std::string text =
			"グラフィック凡例 " + std::to_string(legends.size()) + " 枚\n" +
			"（選択中: internalIndex=" + std::to_string(gSDK->GetObjectInternalIndex(selected)) +
			"）\n\n";
		std::size_t index = 0;
		for (const MCObjectHandle legend : legends)
		{
			++index;
			text += DumpLegend(legend, index);
			text += "\n";
		}
		if (legends.empty())
			text += DumpLegend(selected, 1);
		return text;
#endif
	}
} // namespace HomeskzIfcImport::draw
