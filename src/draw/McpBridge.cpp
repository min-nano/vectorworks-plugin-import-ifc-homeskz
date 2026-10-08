//
//	draw/McpBridge.cpp
//
//	MCP ブリッジの実装（意図と制約は draw/McpBridge.h）。**道具を追加するときに変更するのは
//	kTools の 1 行と、その実装 1 つだけ**である。
//
//	【道具は 3 種類】（M38。Tool::kind）
//	  * **読む**（Read）… 図面・診断ログ・報告を読むだけで、何も生成・変更しない。
//	  * **長く走る**（Long）… `vw_run_test` / `vw_test_cleanup`（M42。実機テストの図面を
//	    閉じて一時ファイルを片付ける）。実機テストの 1 周（draw/Feedback.h）をこの場で
//	    実行し、終わってから応答する。図面を書くのは**本番の取り込みと同じ経路**
//	    （draw/ImportRun の runImportRound）だけで、undo の作法もそちらが持つ
//	    （ImportUndoScope）。実行中は生存の印に `busy_until` を書いておく
//	    （Python 側が「止まった」と誤認しないように）。
//	  * **殻に頼む**（Shell）… `vw_update` / `vw_restart`。本体は自分をアンロードできないので、
//	    要求を受け取って見え方の `action` に載せて返すだけで、**応答しない**。殻が処理した
//	    結果を次の呼び出しで受け取り（shellReport）、そのとき読み込まれている本体が応答する
//	    （src/PayloadAbi.h の VwPayloadMcpServeFn）。
//
//	**新しく図面を書く道具を追加するときは** undo の作法（[SDK リファレンス「Undo」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Undo.md)）を
//	必ず通すこと——不完全な記録を取り消すと図面が壊れる。**常駐なので、書く道具は人が図面を
//	操作している最中にも届きうる**——人の操作と undo の記録が混ざらないかを先に確かめること。
//
//	使う SDK API は、いずれも本プラグインの他の場所で既に使っているもの（＝実機で通ることが
//	確かめてあるもの）に限ってある。SDK の調査は本リポジトリでは行わない
//	（CLAUDE.md「SDK の調査はリファレンス側で行う」）:
//
//	  * VWDocument::GetDrawingHeaderFristMember() / gSDK->NextObject … レイヤの走査
//	    （綴りは SDK ママ。draw/DrawUtil.cpp の AllLayers と同じ）
//	  * VWLayerObj::IsLayerObject / GetLayerType / GetScale / GetObjectName … レイヤの素性
//	  * VWClass::ForEachClass(true, cb) ＋ gSDK->InternalIndexToNameN … クラスの一覧
//	  * gSDK->FirstMemberObj / NextObject / GetObjectTypeN / GetObjectClass … 中身の走査
//	  * gSDK->GetObjectName(h, TXString&)（**戻り値ではなく出力引数**）… 名前
//	  * gSDK->GetObjectBounds(h, WorldRect&) … 外接（WorldRect は top > bottom）
//	  * gSDK->GetCurrentLayer() … 文書が開いているかの判定を兼ねる
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/McpBridge.h"

#include "core/Bridge.h"
#include "core/Json.h"
#include "draw/Feedback.h"
#include "draw/ImportRun.h"
#include "parse/Feedback.h"

#include "VWFC/VWObjects/VWClass.h"
#include "VWFC/VWObjects/VWDocument.h"
#include "VWFC/VWObjects/VWLayerObj.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		using core::Json;

		// --- 受け付けの目安 -------------------------------------------------
		// 生存の印を書き直す間隔。Python 側はこれが古びていたら「動いていない」と判定する
		// （core::kBridgeStatusStaleSeconds）。**毎回は書かない**——パレットの時計は数百 ms
		// ごとに来るので、そのたびにファイルを書き換えるのは無駄が大きい。
		constexpr long long kStatusBeatSeconds = 2;
		// スプールを用意できなかったとき、次に試すまで。一時ディレクトリが読めない・
		// 素性が怪しい、はすぐには直らないので、毎回 mkdir / stat を呼び出さない。
		constexpr long long kPrepareRetrySeconds = 10;

		// --- 小さな共通ヘルパー ---------------------------------------------

		// TXString（UTF-8）→ std::string。空ハンドルでも異常終了しないように受ける。
		std::string Utf8(const TXString& text)
		{
			const char* const raw = static_cast<const char*>(text);
			return raw != nullptr ? std::string(raw) : std::string();
		}

		// オブジェクトの名前（無名なら空）。GetObjectName は**出力引数で返す**。
		std::string NameOf(MCObjectHandle object)
		{
			TXString name;
			gSDK->GetObjectName(object, name);
			return Utf8(name);
		}

		// InternalIndex（クラス・リソースの索引）→ 名前。
		std::string NameOfIndex(InternalIndex index)
		{
			if (index == 0)
				return {};
			TXString name;
			gSDK->InternalIndexToNameN(index, name);
			return Utf8(name);
		}

		// **種別番号の読み替えは分かっているものだけ。** `Kernel/API/Objs.TDType.h` の
		// 一覧は本リポジトリでは持たない（SDK の調査はリファレンス側。CLAUDE.md）ので、
		// 既に Findings / 本リポジトリのコードで名前の判明しているものだけを表にし、
		// それ以外は番号のまま返す。**推測で名前を付けない**——誤った名前は番号より悪い。
		const char* KnownTypeName(short type)
		{
			switch (type)
			{
			case 4:
				return "楕円";
			case 5:
				return "多角形";
			case 11:
				return "グループ";
			case 16:
				return "シンボル定義";
			case 17:
				return "ロクス";
			case 124:
				return "アソシエーション";
			default:
				return nullptr;
			}
		}

		// レイヤ 1 枚の中身の数。
		std::size_t CountMembers(MCObjectHandle layer)
		{
			std::size_t count = 0;
			for (MCObjectHandle h = gSDK->FirstMemberObj(layer); h != nil; h = gSDK->NextObject(h))
				++count;
			return count;
		}

		// 名前でデザイン／シートレイヤを 1 枚取得する（無ければ nil）。
		MCObjectHandle FindLayer(const std::string& name)
		{
			if (name.empty())
				return nil;
			return gSDK->GetNamedLayer(TXString(name.c_str()));
		}

		// 外接を JSON へ（読めなければ null）。単位は**図面の内部単位のまま**返す
		// ——換算を挟むと「何ミリのつもりか」を両側で取り違える。
		Json BoundsJson(MCObjectHandle object)
		{
			WorldRect bounds;
			if (!gSDK->GetObjectBounds(object, bounds))
				return Json::null();
			Json value = Json::object();
			value.set("left", Json::number(bounds.left));
			value.set("right", Json::number(bounds.right));
			value.set("top", Json::number(bounds.top));
			value.set("bottom", Json::number(bounds.bottom));
			return value;
		}

		// --- 道具の中身 ------------------------------------------------------

		Json PingTool(const Json& /*args*/, std::string& /*error*/)
		{
			Json value = Json::object();
			value.set("plugin", Json::string(PLUGIN_VWR_ID));
			value.set("commit", Json::string(VW_BUILD_VERSION));
			value.set("branch", Json::string(VW_BUILD_BRANCH));
			value.set("protocol", Json::integer(core::kBridgeProtocolVersion));

			const MCObjectHandle current = gSDK->GetCurrentLayer();
			value.set("document_open", Json::boolean(current != nil));
			value.set("current_layer",
					  current != nil ? Json::string(NameOf(current)) : Json::null());
			return value;
		}

		Json LayersTool(const Json& args, std::string& error)
		{
			const MCObjectHandle current = gSDK->GetCurrentLayer();
			if (current == nil)
			{
				error = "文書が開いていません。";
				return Json::null();
			}
			const bool includeSheets = args.at("include_sheets").asBool(true);

			Json list = Json::array();
			for (MCObjectHandle h = VWDocument::GetDrawingHeaderFristMember(); h != nil;
				 h = gSDK->NextObject(h))
			{
				if (!VWLayerObj::IsLayerObject(h))
					continue;
				const VWLayerObj layer(h);
				const bool sheet = layer.GetLayerType() == kLayerSheet;
				if (sheet && !includeSheets)
					continue;

				Json entry = Json::object();
				entry.set("name", Json::string(Utf8(layer.GetObjectName())));
				entry.set("kind", Json::string(sheet ? "sheet" : "design"));
				entry.set("scale", Json::number(layer.GetScale()));
				entry.set("objects", Json::integer(static_cast<long long>(CountMembers(h))));
				entry.set("current", Json::boolean(h == current));
				list.push(entry);
			}

			Json value = Json::object();
			value.set("layers", list);
			value.set("count", Json::integer(static_cast<long long>(list.items().size())));
			return value;
		}

		Json ClassesTool(const Json& /*args*/, std::string& error)
		{
			if (gSDK->GetCurrentLayer() == nil)
			{
				error = "文書が開いていません。";
				return Json::null();
			}
			Json list = Json::array();
			// doGuestClasses=true で参照ファイル由来のクラスも含める（draw/DrawUtil.cpp の
			// AllClasses と同じ指定）。
			VWClass::ForEachClass(true,
								  [&list](const VWClass& clas)
								  {
									  const InternalIndex index = clas;
									  const std::string name = NameOfIndex(index);
									  if (!name.empty())
										  list.push(Json::string(name));
								  });
			Json value = Json::object();
			value.set("classes", list);
			value.set("count", Json::integer(static_cast<long long>(list.items().size())));
			return value;
		}

		Json LayerObjectsTool(const Json& args, std::string& error)
		{
			const std::string layerName = args.at("layer").asString();
			const MCObjectHandle layer = FindLayer(layerName);
			if (layer == nil)
			{
				error = "レイヤが見つかりません: " + layerName;
				return Json::null();
			}
			// 既定は 50 件（Claude の文脈を要求 1 つで埋めないため）。0 以下は既定に戻す。
			auto limit = static_cast<long long>(args.at("limit").asNumber(50.0));
			if (limit <= 0)
				limit = 50;
			const long long skip =
				std::max<long long>(static_cast<long long>(args.at("offset").asNumber(0.0)), 0);
			// 種別で絞る（省略＝絞らない）。
			const bool filtered = args.has("type");
			const auto wanted = static_cast<short>(args.at("type").asNumber(0.0));

			Json list = Json::array();
			long long index = 0;
			long long matched = 0;
			for (MCObjectHandle h = gSDK->FirstMemberObj(layer); h != nil;
				 h = gSDK->NextObject(h), ++index)
			{
				const short type = gSDK->GetObjectTypeN(h);
				if (filtered && type != wanted)
					continue;
				++matched;
				if (matched <= skip)
					continue;
				// 符号の違う型を比べるので std::cmp_*（size() は符号なし・limit は符号つき）。
				if (std::cmp_greater_equal(list.items().size(), limit))
					continue; // 数え上げは続ける（total を正しく返すため）

				Json entry = Json::object();
				entry.set("index", Json::integer(index));
				entry.set("type", Json::integer(type));
				const char* const typeName = KnownTypeName(type);
				if (typeName != nullptr)
					entry.set("type_name", Json::string(typeName));
				entry.set("name", Json::string(NameOf(h)));
				entry.set("class", Json::string(NameOfIndex(gSDK->GetObjectClass(h))));
				entry.set("bounds", BoundsJson(h));
				list.push(entry);
			}

			Json value = Json::object();
			value.set("layer", Json::string(layerName));
			value.set("objects", list);
			value.set("returned", Json::integer(static_cast<long long>(list.items().size())));
			value.set("total", Json::integer(matched));
			return value;
		}

		Json ObjectCountsTool(const Json& args, std::string& error)
		{
			if (gSDK->GetCurrentLayer() == nil)
			{
				error = "文書が開いていません。";
				return Json::null();
			}
			const std::string only = args.at("layer").asString();

			// 種別番号 → 件数。**番号の昇順で返す**（列挙順に依存させない。
			// CLAUDE.md「決定性を守る」）。種別は 16 bit なので単純な配列で足りる。
			std::vector<std::pair<short, long long>> tally;
			const auto bump = [&tally](short type)
			{
				for (auto& entry : tally)
				{
					if (entry.first == type)
					{
						++entry.second;
						return;
					}
				}
				tally.emplace_back(type, 1);
			};

			long long total = 0;
			for (MCObjectHandle h = VWDocument::GetDrawingHeaderFristMember(); h != nil;
				 h = gSDK->NextObject(h))
			{
				if (!VWLayerObj::IsLayerObject(h))
					continue;
				const VWLayerObj layer(h);
				if (!only.empty() && Utf8(layer.GetObjectName()) != only)
					continue;
				for (MCObjectHandle member = gSDK->FirstMemberObj(h); member != nil;
					 member = gSDK->NextObject(member))
				{
					bump(gSDK->GetObjectTypeN(member));
					++total;
				}
			}
			std::ranges::sort(tally, [](const std::pair<short, long long>& a,
										const std::pair<short, long long>& b)
							  { return a.first < b.first; });

			Json list = Json::array();
			for (const auto& entry : tally)
			{
				Json row = Json::object();
				row.set("type", Json::integer(entry.first));
				const char* const typeName = KnownTypeName(entry.first);
				if (typeName != nullptr)
					row.set("type_name", Json::string(typeName));
				row.set("count", Json::integer(entry.second));
				list.push(row);
			}

			Json value = Json::object();
			if (!only.empty())
				value.set("layer", Json::string(only));
			value.set("types", list);
			value.set("total", Json::integer(total));
			return value;
		}

		// ファイルを丸ごと読む（読めなければ false）。
		bool ReadWholeFile(const std::string& path, std::string& out)
		{
			if (path.empty())
				return false;
			const std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;
			std::ostringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return true;
		}

		// 返す本文の上限（バイト）。引数で絞れる（0 以下は既定）。**既定は報告と同じ上限**
		// ——Claude の文脈を応答 1 つで埋めない。
		std::size_t MaxBytesArg(const Json& args)
		{
			const auto wanted = static_cast<long long>(args.at("max_bytes").asNumber(0.0));
			if (wanted <= 0)
				return parse::kMaxTestReportBytes;
			return static_cast<std::size_t>(wanted);
		}

		Json LogTool(const Json& args, std::string& error)
		{
			// **ファイルから読む**（メモリの core::trace::text() ではなく）。本体を入れ替えると
			// メモリの本文は消えるが、ファイルは残る——vw_update のあとでも前の取り込みの
			// ログを読めるように。
			const std::string path = importLogPath();
			std::string text;
			if (!ReadWholeFile(path, text))
			{
				error = "診断ログがまだありません（" + path +
						"）。取り込みか実機テストを 1 度実行すると書かれます。";
				return Json::null();
			}
			const std::size_t limit = MaxBytesArg(args);
			Json value = Json::object();
			value.set("path", Json::string(path));
			value.set("bytes", Json::integer(static_cast<long long>(text.size())));
			value.set("truncated", Json::boolean(text.size() > limit));
			value.set("log", Json::string(parse::keepTail(text, limit)));
			return value;
		}

		Json TestReportTool(const Json& /*args*/, std::string& error)
		{
			const std::string path = testReportPath();
			std::string text;
			if (!ReadWholeFile(path, text))
			{
				error = "実機テストの報告がまだありません（" +
						(path.empty() ? std::string("置き場所が分かりません") : path) +
						"）。vw_run_test を実行してください。";
				return Json::null();
			}
			Json value = Json::object();
			value.set("path", Json::string(path));
			value.set("report", Json::string(text));
			return value;
		}

		Json RunTestTool(const Json& args, std::string& error)
		{
			// **開いている図面は不要**（M39）——周はテンプレートから自分で図面を開く。
			// 再起動の直後は図面が 1 枚も開いていないのが普通なので、ここで弾かない。
			// **ダイアログを 1 枚も出さない周**（draw/Feedback.h）。条件は毎周渡してもらい
			// （M43）、使えなければ実行せず、その理由を message に入れて返す。
			TestRoundRequest request;
			request.ifcPath = args.at("ifc").asString();
			request.templatePath = args.at("template").asString();
			request.settings = args.at("settings");
			const TestRoundResult round = runTestRound(request);
			if (!round.ran)
			{
				error = round.message;
				return Json::null();
			}
			Json value = Json::object();
			value.set("commit", Json::string(VW_BUILD_VERSION));
			value.set("message", Json::string(round.message));
			value.set("report_path", Json::string(round.reportPath));
			value.set("report", Json::string(round.report));
			return value;
		}

		Json EndTestTool(const Json& /*args*/, std::string& /*error*/)
		{
			// **片付けられなかったこともエラーにしない**——何を残したかを message で返し、
			// 占有を解く側（Python）がそのまま Claude へ見せる。
			const TestCleanupResult cleanup = endTestSession();
			Json value = Json::object();
			value.set("done", Json::boolean(cleanup.done));
			value.set("message", Json::string(cleanup.message));
			return value;
		}

		// --- 道具の表 --------------------------------------------------------
		//
		// **道具を追加するときに変更するのはここ 1 行と、その実装 1 つだけ。** 一覧は
		// `vw_tools` として Python 側（MCP の tools/list）へそのまま渡るので、
		// **名前と引数の綴りをプラグインと Python の 2 か所へ書かなくてよい**。
		// schema は MCP の inputSchema（JSON Schema）をそのまま書く。

		using ToolFn = Json (*)(const Json& args, std::string& error);

		// 道具の種類（このファイルの冒頭「道具は 3 種類」）。
		enum class ToolKind
		{
			Read, // その場で答える
			Long, // その場で答えるが時間がかかる（生存の印に busy_until を書いてから実行する）
			Shell, // 殻に頼む（本体は応えず、見え方の action に載せて返す）
		};

		struct Tool
		{
			const char* name;
			const char* description;
			const char* schema;
			ToolFn run; // Shell の道具は nullptr（殻が処理する）
			ToolKind kind;
			// Python 側が応答を待つ上限（秒。0 は既定）。**表の外へ書き写さない**——
			// tools/list の元（ToolCatalog）に `timeoutSeconds` として載り、Python はそれを
			// 読んでから Claude へ見せる前に除外する（scripts/mcp/vw-mcp-server.py）。
			int timeoutSeconds;
		};

		// 長く走る道具・殻に頼む道具の待ち時間。実機テストは取り込みに 1 分以上、更新は
		// ダウンロードを含む。
		constexpr int kRunTestTimeoutSeconds = 1800;
		constexpr int kEndTestTimeoutSeconds = 120;
		constexpr int kUpdateTimeoutSeconds = 600;
		constexpr int kRestartTimeoutSeconds = 60;

		const auto kTools = std::to_array<Tool>({
			{"vw_ping", "ブリッジが生きているかと、いま開いている図面の素性を返す。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", &PingTool,
			 ToolKind::Read, 0},
			{"vw_layers",
			 "図面のレイヤ一覧（名前・デザイン/シート・縮尺・中身の数・カレントか）を返す。",
			 R"({"type":"object","properties":{"include_sheets":{"type":"boolean",)"
			 R"("description":"シートレイヤも含めるか（既定 true）"}},)"
			 R"("additionalProperties":false})",
			 &LayersTool, ToolKind::Read, 0},
			{"vw_classes", "図面のクラス名を一覧で返す。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", &ClassesTool,
			 ToolKind::Read, 0},
			{"vw_layer_objects",
			 "指定したレイヤの中身を返す（種別番号・名前・クラス・外接）。"
			 "種別番号の意味は図面によるので、まず vw_object_counts で当たりを付けるとよい。",
			 R"({"type":"object","properties":{)"
			 R"("layer":{"type":"string","description":"レイヤ名"},)"
			 R"("limit":{"type":"integer","description":"返す件数の上限（既定 50）"},)"
			 R"("offset":{"type":"integer","description":"先頭から読み飛ばす件数"},)"
			 R"("type":{"type":"integer","description":"この種別番号のものだけに絞る"}},)"
			 R"("required":["layer"],"additionalProperties":false})",
			 &LayerObjectsTool, ToolKind::Read, 0},
			{"vw_object_counts",
			 "図面（または 1 レイヤ）の中身を種別番号ごとに数える。何が入っているかの見当を"
			 "付けるための道具。",
			 R"({"type":"object","properties":{)"
			 R"("layer":{"type":"string","description":"このレイヤだけを数える（省略＝図面全体）"}},)"
			 R"("additionalProperties":false})",
			 &ObjectCountsTool, ToolKind::Read, 0},
			{"vw_log",
			 "直近の取り込み（本番の取り込みか実機テスト）の診断ログを返す。長いときは古いほうを"
			 "削って末尾を返す。",
			 R"({"type":"object","properties":{)"
			 R"("max_bytes":{"type":"integer","description":"返す上限（バイト。既定 60000）"}},)"
			 R"("additionalProperties":false})",
			 &LogTool, ToolKind::Read, 0},
			{"vw_test_report",
			 "直近の実機テストの報告（Markdown。要素の内訳・図面の状態・診断ログ）を返す。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", &TestReportTool,
			 ToolKind::Read, 0},
			{"vw_run_test",
			 "実機テストを 1 周走らせる——前の周の図面を保存せずに閉じ、テンプレートから開いた"
			 "新しい図面へ IFC を取り込み、報告を返す。ifc と template は毎回渡す（前の周の条件は"
			 "覚えていない）。設定はテンプレートの図面にあるもので既定を組み、settings に書いた"
			 "項目だけ上書きする。図面は開いていなくてよい。ダイアログは出さない。Vectorworks が"
			 "起動していなければ起動してから走らせる。取り込みに 1 分以上かかる。",
			 R"({"type":"object","properties":{)"
			 R"("ifc":{"type":"string","description":"取り込む IFC の絶対パス（tests/fixtures/ の IFC）"},)"
			 R"("template":{"type":"string","description":"テンプレート（.sta）の絶対パス（リポジトリの tests/fixtures/Default.sta）"},)"
			 R"("settings":{"type":"object","description":"取り込み設定の上書き（省略＝既定のまま）",)"
			 R"("properties":{)"
			 R"("symbols":{"type":"object","description":"役割の表示名（アンカーボルト（座金付き）/ アンカーボルト（座金なし）/ 床束 / 火打 / 仕口 / 伏図記号（柱）/ 伏図記号（小屋束）/ 継手）→ シンボル名（そのシンボルで取り込む）・true（既定のシンボルで取り込む）・false か空文字列（取り込まない）","additionalProperties":{"type":["string","boolean"]}},)"
			 R"("title_block":{"type":"string","description":"図面枠のスタイル名（空＝置かない）"},)"
			 R"("dimension":{"type":"string","description":"寸法規格の名前（空＝入れない）"},)"
			 R"("merge_levels":{"type":"array","items":{"type":"string"},"description":"前のレベルと同じ伏図にまとめる高さ（\"<階の番号>:<高さ mm>\"）"},)"
			 R"("skip_sections":{"type":"array","items":{"type":"string"},"description":"軸組図から除外する通りの図番"},)"
			 R"("rafter":{"type":"object","properties":{"width":{"type":"number"},"height":{"type":"number"}},"additionalProperties":false,"description":"垂木の断面（mm）"}},)"
			 R"("additionalProperties":false}},)"
			 R"("required":["ifc","template"],"additionalProperties":false})",
			 &RunTestTool, ToolKind::Long, kRunTestTimeoutSeconds},
			{"vw_test_cleanup",
			 "実機テストを終える——実機テストが保存した図面を保存せずに閉じ、一時ファイル"
			 "（各周の図面）・記録・報告を消す。vw_lock_release が占有を解くときに自動で呼ぶ"
			 "ので、ふつうは直接呼ばない。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", &EndTestTool,
			 ToolKind::Long, kEndTestTimeoutSeconds},
			{"vw_update",
			 "開発版の新しいビルドを入れ、本体を読み直す（尋ねない）。既定はいま入っているのと"
			 "同じブランチの最新。branch で別のブランチを名指しできる。殻まで変わったビルドは"
			 "再起動するまで効かない（restart_required が true）。restart_if_needed を true に"
			 "すると、そのときは続けて再起動する。",
			 R"({"type":"object","properties":{)"
			 R"("branch":{"type":"string","description":"入れるビルドのブランチ（省略＝いまのブランチ）"},)"
			 R"("restart_if_needed":{"type":"boolean","description":"再起動が要るなら続けて再起動するか（既定 false）"}},)"
			 R"("additionalProperties":false})",
			 nullptr, ToolKind::Shell, kUpdateTimeoutSeconds},
			{"vw_restart",
			 "Vectorworks を再起動する。開いている図面に未保存の変更があれば、Vectorworks の"
			 "保存の確認が出る（人の応答が要る）。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", nullptr,
			 ToolKind::Shell, kRestartTimeoutSeconds},
		});

		// 表を MCP の tools/list が求める形（name / description / inputSchema）で返す。
		Json ToolCatalog()
		{
			Json list = Json::array();
			for (const Tool& tool : kTools)
			{
				Json entry = Json::object();
				entry.set("name", Json::string(tool.name));
				entry.set("description", Json::string(tool.description));
				Json schema;
				std::string error;
				if (!Json::parse(tool.schema, schema, error))
					schema = Json::object(); // 表の綴り間違いで一覧ごと失敗させない
				entry.set("inputSchema", schema);
				if (tool.timeoutSeconds > 0)
					entry.set("timeoutSeconds", Json::integer(tool.timeoutSeconds));
				list.push(entry);
			}
			Json value = Json::object();
			value.set("tools", list);
			value.set("protocol", Json::integer(core::kBridgeProtocolVersion));
			return value;
		}

		// 名前で道具を検索する（無ければ nullptr）。
		const Tool* FindTool(const std::string& name)
		{
			for (const Tool& tool : kTools)
			{
				if (name == tool.name)
					return &tool;
			}
			return nullptr;
		}

		// 要求 1 件を処理する。**例外をここで受ける**（1 件の失敗で橋を停止させない）。
		// 殻に頼む道具（ToolKind::Shell）はここへ来ない（serveMcpBridge が受け取る）。
		core::BridgeResponse Handle(const core::BridgeRequest& request)
		{
			core::BridgeResponse response;
			response.id = request.id;
			try
			{
				// 道具の一覧そのものは表を検索せずに答える（Python の tools/list の元）。
				if (request.tool == "vw_tools")
				{
					response.ok = true;
					response.result = ToolCatalog();
					return response;
				}
				const Tool* const tool = FindTool(request.tool);
				if (tool == nullptr || tool->run == nullptr)
				{
					response.ok = false;
					response.error = "知らない道具です: " + request.tool;
					return response;
				}
				std::string error;
				const Json result = tool->run(request.args, error);
				if (!error.empty())
				{
					response.ok = false;
					response.error = error;
					return response;
				}
				response.ok = true;
				response.result = result.isNull() ? Json::object() : result;
			}
			catch (const std::exception& e)
			{
				response.ok = false;
				response.error = std::string("例外: ") + e.what();
			}
			catch (...)
			{
				response.ok = false;
				response.error = "不明な例外";
			}
			return response;
		}

		// --- スプールの場所 ---------------------------------------------------

		// いまの一時ディレクトリ（読めなければ空）。本体の複製を置くのと同じ場所で
		// （src/PayloadHost.cpp）、スプールの中身も正に一時データである。
		std::string TempDirectory()
		{
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
			if (ec)
				return {};
			return dir.string();
		}

		// 使うスプール。VW_MCP_SPOOL があればそれを優先する（Python 側も同じ）。
		//
		// **こちらは探索しない。** 一時ディレクトリが両側で食い違いうる（macOS の $TMPDIR は
		// 利用者ごとで、ssh や cron から起動したプロセスには無い）ことへの手当ては
		// Python 側が持つ——あちらが候補を順に確認して、有効な印のあるところへ要求を置く
		// （core/Bridge.h の bridgeSpoolDir）。
		std::string SpoolDirectory()
		{
			const char* const chosen = std::getenv("VW_MCP_SPOOL");
			if (chosen != nullptr && *chosen != '\0')
				return {chosen};
			return core::bridgeSpoolDir(TempDirectory(), PLUGIN_VWR_ID);
		}

		// いまの時刻（epoch 秒）。生存の印に載せる。
		long long NowSeconds()
		{
			const auto now = std::chrono::system_clock::now().time_since_epoch();
			return static_cast<long long>(
				std::chrono::duration_cast<std::chrono::seconds>(now).count());
		}

		// busyTool / busyUntil は長く走る道具の最中だけ（空・0 なら載せない）。**Python 側は
		// busy_until が未来なら、beat が古びていても「生きている」と判定する**——実機テストの
		// 1 周は 1 分以上かかり、その間この本体は印を書き直せない。
		Json StatusJson(long long served, long long failed, const std::string& busyTool = {},
						long long busyUntil = 0)
		{
			Json value = Json::object();
			value.set("plugin", Json::string(PLUGIN_VWR_ID));
			value.set("commit", Json::string(VW_BUILD_VERSION));
			value.set("branch", Json::string(VW_BUILD_BRANCH));
			value.set("protocol", Json::integer(core::kBridgeProtocolVersion));
			// **これが「生きているか」の判定に使われる。** Python 側は現在時刻と比べて、
			// 古びていたら「動いていない」と判定する。
			value.set("beat", Json::integer(NowSeconds()));
			value.set("served", Json::integer(served));
			value.set("failed", Json::integer(failed));
			if (!busyTool.empty())
			{
				value.set("busy", Json::string(busyTool));
				value.set("busy_until", Json::integer(busyUntil));
			}
			return value;
		}
	} // namespace

	// -----------------------------------------------------------------------
	namespace
	{
		// 受け付けの状態。**本体の静的データなので、本体を入れ替えると初めからになる**
		// （件数が 0 に戻るだけで、橋は途切れない——印は前の本体が書いたものが残っていて、
		// 次の 1 回で書き直される）。メインスレッドしかアクセスしないので排他は要らない。
		struct ServeState
		{
			bool prepared = false;
			std::string dir;
			std::string error;
			long long lastPrepareAt = -1;
			long long lastBeat = 0;
			long long served = 0;
			long long failed = 0;
			long long lastRequestAt = -1;
			std::string lastTool;
			// **殻に頼む要求**（この 1 回で受け取ったもの。無ければ空）。見え方の `action` に
			// 載せて殻へ渡し、結果は次の呼び出しの shellReport で戻ってくる。
			core::BridgeRequest action;
			bool hasAction = false;
			// 殻から受け取った結果を応答として書けたか（見え方の `reportDone`）。殻は
			// これが立つまで同じ結果を渡し直す。
			bool reportDone = false;
		};

		ServeState& State()
		{
			static ServeState sState;
			return sState;
		}

		// パレットに見せる見え方。**判断は JS に持たせない**ので、見せる文言もここで作る。
		std::string ViewJson(const ServeState& state, long long now)
		{
			Json view = Json::object();
			view.set("phase", Json::string(state.prepared ? "serving" : "error"));
			view.set("spool", Json::string(state.dir));
			view.set("served", Json::integer(state.served));
			view.set("failed", Json::integer(state.failed));
			view.set("lastTool", Json::string(state.lastTool));
			view.set("secondsSinceRequest",
					 Json::integer(state.lastRequestAt >= 0 ? now - state.lastRequestAt : -1));
			view.set("message", Json::string(state.prepared ? std::string() : state.error));
			view.set("commit", Json::string(VW_BUILD_VERSION));
			view.set("reportDone", Json::boolean(state.reportDone));
			if (state.hasAction)
			{
				Json action = Json::object();
				action.set("id", Json::string(state.action.id));
				action.set("tool", Json::string(state.action.tool));
				action.set("args",
						   state.action.args.isObject() ? state.action.args : Json::object());
				view.set("action", action);
			}
			return view.dump();
		}

		// スプールを用意する（済んでいれば何もしない）。**前の回の残骸は、稼働中の橋の後を
		// 継ぐのでなければ削除する**——本体の入れ替えのたびにここへ来るので、稼働中の橋が
		// あるうちに削除すると、入れ替えの直前に書いた応答や、Python が置いたばかりの要求まで消える
		// （core/Bridge.h の sweep）。
		bool Prepare(ServeState& state, long long now)
		{
			if (state.prepared)
				return true;
			if (state.lastPrepareAt >= 0 && now - state.lastPrepareAt < kPrepareRetrySeconds)
				return false;
			state.lastPrepareAt = now;
			state.dir = SpoolDirectory();
			core::BridgeSpool spool(state.dir);
			if (!spool.prepare(state.error))
				return false;
			if (!spool.statusIsLive(now, core::kBridgeStatusStaleSeconds))
				spool.sweep();
			state.error.clear();
			state.prepared = true;
			return true;
		}
	} // namespace

	namespace
	{
		// **殻が処理した要求の結果を応答として書く**（PayloadAbi.h の VwPayloadMcpServeFn）。
		// 形は `{"id":…,"ok":…,"result":{…},"error":"…"}`（src/Extensions/ExtMcpPalette.cpp）。
		// 書けたら true。**書いたのはいま読み込まれている本体**なので、その素性を結果に添える
		// ——vw_update のあと、新しい本体が応えたことを Claude が確かめられる。
		bool ReplyShellReport(core::BridgeSpool& spool, const std::string& text)
		{
			Json report;
			std::string error;
			if (!Json::parse(text, report, error) || !report.isObject())
				return true; // 読めない結果を何度渡されても書けない。受け取ったことにする
			core::BridgeResponse response;
			response.id = report.at("id").asString();
			if (!core::isValidBridgeId(response.id))
				return true;
			response.ok = report.at("ok").asBool(false);
			response.error = report.at("error").asString();
			Json result = report.at("result").isObject() ? report.at("result") : Json::object();
			result.set("payload_commit", Json::string(VW_BUILD_VERSION));
			response.result = result;
			std::string replyError;
			return spool.reply(response, replyError);
		}
	} // namespace

	std::string serveMcpBridge(const std::string& shellReport)
	{
		ServeState& state = State();
		long long now = NowSeconds();
		state.hasAction = false;
		state.action = core::BridgeRequest{};
		state.reportDone = false;
		try
		{
			if (!Prepare(state, now))
				return ViewJson(state, now);

			core::BridgeSpool spool(state.dir);
			if (!shellReport.empty())
				state.reportDone = ReplyShellReport(spool, shellReport);

			std::vector<std::string> broken;
			const std::vector<core::BridgeRequest> requests = spool.poll(broken);

			// 壊れていた要求にも必ず応える（Python 側を待たせ切らない）。
			for (const std::string& id : broken)
			{
				std::string replyError;
				spool.reply(core::bridgeFailure(id, "要求を読めませんでした。"), replyError);
				++state.failed;
			}
			for (const core::BridgeRequest& request : requests)
			{
				const Tool* const tool = FindTool(request.tool);
				if (tool != nullptr && tool->kind == ToolKind::Shell)
				{
					// **殻に頼む。** 1 回に受け取れるのは 1 つだけ（処理するあいだに本体が
					// 入れ替わりうるので、2 つ目は同じ本体に約束できない）。
					std::string replyError;
					if (state.hasAction)
					{
						spool.reply(core::bridgeFailure(request.id, "別の頼みごと（" +
																		state.action.tool +
																		"）を処理しています。"),
									replyError);
						++state.failed;
					}
					else
					{
						state.action = request;
						state.hasAction = true;
					}
					state.lastTool = request.tool;
					continue;
				}
				if (tool != nullptr && tool->kind == ToolKind::Long)
				{
					// 実行中は印を書き直せないので、先に「いつまでかかりうるか」を書く。
					std::string statusError;
					(void)spool.writeStatus(StatusJson(state.served, state.failed, tool->name,
													   now + tool->timeoutSeconds),
											statusError);
				}
				const core::BridgeResponse response = Handle(request);
				std::string replyError;
				spool.reply(response, replyError);
				if (response.ok)
					++state.served;
				else
					++state.failed;
				// vw_tools は Python が起動のたびに取得するだけなので、「最後の道具」には数えない。
				if (request.tool != "vw_tools")
					state.lastTool = request.tool;
				if (tool != nullptr && tool->kind == ToolKind::Long)
				{
					// 時計が大きく進んでいる。印をすぐ書き直す（busy を解除する）。
					now = NowSeconds();
					state.lastBeat = 0;
				}
			}
			if (!requests.empty() || !broken.empty())
				state.lastRequestAt = now;

			// 生存の印を書き直す（数秒に 1 回で足りる）。
			if (now - state.lastBeat >= kStatusBeatSeconds)
			{
				std::string statusError;
				if (spool.writeStatus(StatusJson(state.served, state.failed), statusError))
					state.lastBeat = now;
				else
				{
					// **書けなくなったら用意し直す**（一時ディレクトリが掃除された等）。
					// 印が古びれば Python 側は「動いていない」と言うので、黙って続けない。
					state.prepared = false;
					state.error = "生存の印を書けません: " + statusError;
				}
			}
		}
		catch (const std::exception& e)
		{
			state.prepared = false;
			state.error = std::string("例外: ") + e.what();
		}
		catch (...)
		{
			state.prepared = false;
			state.error = "不明な例外";
		}
		return ViewJson(state, now);
	}
} // namespace HomeskzIfcImport::draw
