//
//	draw/McpBridge.cpp
//
//	MCP ブリッジの実装（意図と制約は draw/McpBridge.h）。**道具を足すときに触るのは
//	kTools の 1 行と、その実装 1 つだけ**である。
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
//	【読むだけにしてある】v1 の道具はすべて図面を**読む**だけで、何も作らず・変えない。
//	書く道具（作図・修正）を足すときは undo の作法（[SDK リファレンス「Undo」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Undo.md)）を
//	必ず通すこと——半端な記録を取り消すと図面が壊れる。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/McpBridge.h"
#include "draw/ProgressDialog.h"

#include "core/Bridge.h"
#include "core/Json.h"

#include "VWFC/VWObjects/VWClass.h"
#include "VWFC/VWObjects/VWDocument.h"
#include "VWFC/VWObjects/VWLayerObj.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		using core::Json;

		// --- ループの目安 ---------------------------------------------------
		// 1 周の待ち。短いほど応答が速く、短すぎるだけ Vectorworks を無駄に回す。
		// 人が対話する速さなので、この程度で体感の遅れにはならない。
		constexpr int kPollMilliseconds = 40;
		// 何も来ないまま止まるまで。**押しっぱなしを忘れても Vectorworks を永久に
		// 握らない**ための歯止め（要求が 1 つでも来れば数え直す）。
		constexpr int kIdleTimeoutSeconds = 30 * 60;
		// 生存の印を書き直す間隔。Python 側はこれが古びていたら「動いていない」と見る。
		constexpr int kStatusBeatSeconds = 2;

		// 止めてほしいと道具（vw_stop_bridge）から言われたか。**メインスレッドしか
		// 触らない**ので排他は要らない（CLAUDE.md / PayloadSession.h「スレッド」）。
		bool gStopRequested = false;

		// --- 小さな共通ヘルパー ---------------------------------------------

		// TXString（UTF-8）→ std::string。空ハンドルでも落ちないように受ける。
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
		// 既に Findings / 本リポジトリのコードで名前の割れているものだけを表にし、
		// それ以外は番号のまま返す。**推測で名前を付けない**——嘘の名前は番号より悪い。
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

		// 名前でデザイン／シートレイヤを 1 枚引く（無ければ nil）。
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

		Json StopTool(const Json& /*args*/, std::string& /*error*/)
		{
			gStopRequested = true;
			Json value = Json::object();
			value.set("stopping", Json::boolean(true));
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
			auto skip = static_cast<long long>(args.at("offset").asNumber(0.0));
			if (skip < 0)
				skip = 0;
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
				if (static_cast<long long>(list.items().size()) >= limit)
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
			// CLAUDE.md「決定性を守る」）。種別は 16 bit なので素直な配列で足りる。
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
			std::sort(tally.begin(), tally.end(),
					  [](const std::pair<short, long long>& a, const std::pair<short, long long>& b)
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

		// --- 道具の表 --------------------------------------------------------
		//
		// **道具を足すときに触るのはここ 1 行と、その実装 1 つだけ。** 一覧は
		// `vw_tools` として Python 側（MCP の tools/list）へそのまま渡るので、
		// **名前と引数の綴りをプラグインと Python の 2 か所へ書かなくてよい**。
		// schema は MCP の inputSchema（JSON Schema）をそのまま書く。

		using ToolFn = Json (*)(const Json& args, std::string& error);

		struct Tool
		{
			const char* name;
			const char* description;
			const char* schema;
			ToolFn run;
		};

		const auto kTools = std::to_array<Tool>({
			{"vw_ping", "ブリッジが生きているかと、いま開いている図面の素性を返す。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", &PingTool},
			{"vw_layers",
			 "図面のレイヤ一覧（名前・デザイン/シート・縮尺・中身の数・カレントか）を返す。",
			 R"({"type":"object","properties":{"include_sheets":{"type":"boolean",)"
			 R"("description":"シートレイヤも含めるか（既定 true）"}},)"
			 R"("additionalProperties":false})",
			 &LayersTool},
			{"vw_classes", "図面のクラス名を一覧で返す。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", &ClassesTool},
			{"vw_layer_objects",
			 "指定したレイヤの中身を返す（種別番号・名前・クラス・外接）。"
			 "種別番号の意味は図面によるので、まず vw_object_counts で当たりを付けるとよい。",
			 R"({"type":"object","properties":{)"
			 R"("layer":{"type":"string","description":"レイヤ名"},)"
			 R"("limit":{"type":"integer","description":"返す件数の上限（既定 50）"},)"
			 R"("offset":{"type":"integer","description":"先頭から読み飛ばす件数"},)"
			 R"("type":{"type":"integer","description":"この種別番号のものだけに絞る"}},)"
			 R"("required":["layer"],"additionalProperties":false})",
			 &LayerObjectsTool},
			{"vw_object_counts",
			 "図面（または 1 レイヤ）の中身を種別番号ごとに数える。何が入っているかの見当を"
			 "付けるための道具。",
			 R"({"type":"object","properties":{)"
			 R"("layer":{"type":"string","description":"このレイヤだけを数える（省略＝図面全体）"}},)"
			 R"("additionalProperties":false})",
			 &ObjectCountsTool},
			{"vw_stop_bridge", "ブリッジを止める（Vectorworks 側の進捗ダイアログが閉じる）。",
			 R"({"type":"object","properties":{},"additionalProperties":false})", &StopTool},
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
					schema = Json::object(); // 表の綴り間違いで一覧ごと落とさない
				entry.set("inputSchema", schema);
				list.push(entry);
			}
			Json value = Json::object();
			value.set("tools", list);
			value.set("protocol", Json::integer(core::kBridgeProtocolVersion));
			return value;
		}

		// 要求 1 件を捌く。**例外をここで受ける**（1 件の失敗で橋を落とさない）。
		core::BridgeResponse Handle(const core::BridgeRequest& request)
		{
			core::BridgeResponse response;
			response.id = request.id;
			try
			{
				// 道具の一覧そのものは表を引かずに答える（Python の tools/list の元）。
				if (request.tool == "vw_tools")
				{
					response.ok = true;
					response.result = ToolCatalog();
					return response;
				}
				for (const Tool& tool : kTools)
				{
					if (request.tool != tool.name)
						continue;
					std::string error;
					const Json result = tool.run(request.args, error);
					if (!error.empty())
					{
						response.ok = false;
						response.error = error;
						return response;
					}
					response.ok = true;
					response.result = result.isNull() ? Json::object() : result;
					return response;
				}
				response.ok = false;
				response.error = "知らない道具です: " + request.tool;
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

		// いまのホームディレクトリ（読めなければ空）。
		std::string HomeDirectory()
		{
#if defined(_WIN32)
			const char* const home = std::getenv("USERPROFILE");
#else
			const char* const home = std::getenv("HOME");
#endif
			return home != nullptr ? std::string(home) : std::string();
		}

		// 使うスプール。VW_MCP_SPOOL があればそれを優先する（Python 側も同じ）。
		std::string SpoolDirectory()
		{
			const char* const chosen = std::getenv("VW_MCP_SPOOL");
			if (chosen != nullptr && *chosen != '\0')
				return {chosen};
			return core::bridgeSpoolDir(HomeDirectory(), PLUGIN_VWR_ID);
		}

		// いまの時刻（epoch 秒）。生存の印に載せる。
		long long NowSeconds()
		{
			const auto now = std::chrono::system_clock::now().time_since_epoch();
			return static_cast<long long>(
				std::chrono::duration_cast<std::chrono::seconds>(now).count());
		}

		Json StatusJson(long long served, long long failed)
		{
			Json value = Json::object();
			value.set("plugin", Json::string(PLUGIN_VWR_ID));
			value.set("commit", Json::string(VW_BUILD_VERSION));
			value.set("branch", Json::string(VW_BUILD_BRANCH));
			value.set("protocol", Json::integer(core::kBridgeProtocolVersion));
			// **これが「生きているか」の判定に使われる。** Python 側は現在時刻と比べて、
			// 古びていたら「動いていない」と見る。
			value.set("beat", Json::integer(NowSeconds()));
			value.set("served", Json::integer(served));
			value.set("failed", Json::integer(failed));
			return value;
		}
	} // namespace

	// -----------------------------------------------------------------------
	void runMcpBridge()
	{
		gStopRequested = false;

		core::BridgeSpool spool(SpoolDirectory());
		std::string error;
		if (!spool.prepare(error))
		{
			gSDK->AlertInform("MCP ブリッジを開始できませんでした。", TXString(error.c_str()),
							  false);
			return;
		}
		// 前の回の残骸を消す（落ちたセッションの応答を新しいものと取り違えない）。
		spool.sweep();

		long long served = 0;
		long long failed = 0;
		long long lastBeat = 0;
		const long long startedAt = NowSeconds();
		long long lastRequestAt = startedAt;
		std::string stopReason = "［キャンセル］で止めました。";

		{
			// 進捗ダイアログが**橋の寿命そのもの**。開いている間だけ橋が架かり、
			// ［キャンセル］で降りる（draw/McpBridge.h「なぜループなのか」）。
			ProgressDialog dialog("MCP ブリッジ", "接続先: " + spool.dir(), true /* canCancel */);

			for (;;)
			{
				std::vector<std::string> broken;
				const std::vector<core::BridgeRequest> requests = spool.poll(broken);

				// 壊れていた要求にも必ず応える（Python 側を待たせ切らない）。
				for (const std::string& id : broken)
				{
					std::string replyError;
					spool.reply(core::bridgeFailure(id, "要求を読めませんでした。"), replyError);
					++failed;
				}
				for (const core::BridgeRequest& request : requests)
				{
					const core::BridgeResponse response = Handle(request);
					std::string replyError;
					spool.reply(response, replyError);
					if (response.ok)
						++served;
					else
						++failed;
				}
				if (!requests.empty() || !broken.empty())
					lastRequestAt = NowSeconds();

				// 生存の印を書き直す（毎周は書かない——数秒に 1 回で足りる）。
				const long long now = NowSeconds();
				if (now - lastBeat >= kStatusBeatSeconds)
				{
					std::string statusError;
					spool.writeStatus(StatusJson(served, failed), statusError);
					lastBeat = now;
				}

				// **ここで Vectorworks へ制御を返す**（再描画とキャンセル操作）。
				const std::string meter = "受けた要求 " + std::to_string(served + failed) +
										  " 件（うち失敗 " + std::to_string(failed) + " 件）";
				if (dialog.keepAlive(meter))
					break;
				if (gStopRequested)
				{
					stopReason = "Claude 側から止めました（vw_stop_bridge）。";
					break;
				}
				if (now - lastRequestAt >= kIdleTimeoutSeconds)
				{
					stopReason = "しばらく要求が無かったので止めました。";
					break;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(kPollMilliseconds));
			}
		} // ここでダイアログが閉じる（完了の知らせを重ねて出さないため）

		spool.removeStatus();

		gSDK->AlertInform(
			"MCP ブリッジを終了しました。",
			TXString((stopReason + "\n\n応えた要求: " + std::to_string(served) +
					  " 件\n失敗: " + std::to_string(failed) + " 件\n接続先: " + spool.dir())
						 .c_str()),
			false);
	}
} // namespace HomeskzIfcImport::draw
