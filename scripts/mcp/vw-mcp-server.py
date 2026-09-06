#!/usr/bin/env python3
#
# vw-mcp-server.py — Claude と Vectorworks をつなぐ MCP サーバ（ブリッジの Claude 側）
#
#   Claude ──MCP(stdio/JSON-RPC)──▶ このスクリプト
#                                     │ <id>.req.json を書く／<id>.res.json を待つ
#                                     ▼
#                                 スプール（既定 ~/.min-nano_structure/mcp）
#                                     ▲
#                                     │ 拾う／応える
#                               Vectorworks（メニュー「MCP ブリッジを開始…」の実行中だけ）
#
# 【このスクリプトが持たないもの】**道具の一覧を持たない。** 何ができるか（名前・説明・
# 引数の形）はプラグイン側の表（src/draw/McpBridge.cpp の kTools）ただ 1 つが真実で、
# ここは起動時にそれを `vw_tools` で取りに行くだけ。だから**道具を足すのにこの
# スクリプトを直す必要が無い**（プラグインを更新すれば増える）。
#
# 【依存を持たない】標準ライブラリだけで書いてある。プラグインの zip に同梱して配るので、
# 利用者に pip を要求しないことが要件（Python 3.8 以降）。
#
# 使い方（Claude Code に登録する）:
#
#   claude mcp add vectorworks -- python3 <この scripts/mcp/vw-mcp-server.py のパス>
#
# 環境変数:
#   VW_MCP_SPOOL   スプールの場所を明示する（プラグイン側と同じ値にすること）
#   VW_MCP_PLUGIN  プラグイン名（既定 min-nano_structure。開発版は min-nano_structureDev）
#   VW_MCP_TIMEOUT 1 件あたりの待ち時間（秒。既定 30）
#
# 【受け渡しの作法はプラグイン側と対になっている】ファイル名の綴り・原子的な書き方
# （.tmp へ書いてから rename）・生存の印の見方は src/core/Bridge.h に書いてある。
# どちらかを変えるときは必ず両方を直す。

import json
import os
import secrets
import sys
import time

# --- 受け渡しの作法（src/core/Bridge.h と対）---------------------------------
REQUEST_SUFFIX = ".req.json"
RESPONSE_SUFFIX = ".res.json"
STATUS_FILE = "bridge.json"
TOOLS_CACHE_FILE = "tools-cache.json"
PROTOCOL_VERSION = 1

# 生存の印がこれより古ければ「動いていない」と見る（プラグイン側は数秒ごとに書き直す）。
STATUS_STALE_SECONDS = 15

# MCP の版（stdio + tools）。
MCP_PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "vectorworks-bridge"
SERVER_VERSION = "0.1.0"

DEFAULT_PLUGIN = "min-nano_structure"
DEFAULT_TIMEOUT = 30.0


def log(message):
    """診断は stderr へ（stdout は JSON-RPC 専用）。"""
    print("[vw-mcp] " + message, file=sys.stderr, flush=True)


def spool_dir():
    """使うスプール。**プラグイン側（core::bridgeSpoolDir）と同じ組み立て。**"""
    override = os.environ.get("VW_MCP_SPOOL", "")
    if override:
        return override
    plugin = os.environ.get("VW_MCP_PLUGIN", "") or DEFAULT_PLUGIN
    home = os.path.expanduser("~").rstrip("/\\")
    return home + "/." + plugin + "/mcp"


def call_timeout():
    try:
        return float(os.environ.get("VW_MCP_TIMEOUT", "") or DEFAULT_TIMEOUT)
    except ValueError:
        return DEFAULT_TIMEOUT


class BridgeDown(Exception):
    """Vectorworks 側でブリッジが動いていない（＝メニューを実行していない）。"""


class Bridge:
    """スプール越しに Vectorworks を呼ぶ（1 往復＝ファイル 2 つ）。"""

    def __init__(self, directory):
        self.dir = directory
        self.seq = 0

    # --- 生存確認 ---------------------------------------------------------
    def status(self):
        """動いていれば status の dict、動いていなければ None。"""
        path = os.path.join(self.dir, STATUS_FILE)
        try:
            with open(path, "r", encoding="utf-8") as handle:
                status = json.load(handle)
        except (OSError, ValueError):
            return None
        if not isinstance(status, dict):
            return None
        # **印が古びていたら動いていない。** Vectorworks ごと落ちた場合、印は残る。
        beat = status.get("beat")
        if not isinstance(beat, (int, float)):
            return None
        if time.time() - float(beat) > STATUS_STALE_SECONDS:
            return None
        return status

    def require_status(self):
        status = self.status()
        if status is None:
            raise BridgeDown(
                "Vectorworks 側でブリッジが動いていません。\n"
                "Vectorworks のメニュー「MCP ブリッジを開始…」を実行してから、"
                "もう一度お試しください。\n"
                "（接続先: %s）" % self.dir
            )
        if status.get("protocol") != PROTOCOL_VERSION:
            raise BridgeDown(
                "ブリッジの版が違います（プラグイン側 %s / このサーバ %d）。\n"
                "プラグインとこのスクリプトの新しいほうへ揃えてください。"
                % (status.get("protocol"), PROTOCOL_VERSION)
            )
        return status

    # --- 1 往復 -----------------------------------------------------------
    def call(self, tool, args, timeout=None):
        self.require_status()
        if timeout is None:
            timeout = call_timeout()

        self.seq += 1
        # 名前の昇順が送った順になるように連番を先頭へ置く（プラグイン側はこの順で拾う）。
        request_id = "%012d-%s" % (self.seq, secrets.token_hex(4))
        payload = json.dumps(
            {"id": request_id, "tool": tool, "args": args or {}}, ensure_ascii=False
        )

        request_path = os.path.join(self.dir, request_id + REQUEST_SUFFIX)
        temp_path = request_path + ".tmp"
        os.makedirs(self.dir, exist_ok=True)
        with open(temp_path, "w", encoding="utf-8") as handle:
            handle.write(payload)
        # **書き上がってから見せる**（プラグイン側に半端な内容を拾わせない）。
        os.replace(temp_path, request_path)

        response_path = os.path.join(self.dir, request_id + RESPONSE_SUFFIX)
        deadline = time.time() + timeout
        while True:
            try:
                with open(response_path, "r", encoding="utf-8") as handle:
                    response = json.load(handle)
                try:
                    os.remove(response_path)
                except OSError:
                    pass
                return response
            except (OSError, ValueError):
                pass
            if time.time() >= deadline:
                # 置いたままの要求を引き上げる（次のセッションが拾わないように）。
                try:
                    os.remove(request_path)
                except OSError:
                    pass
                if self.status() is None:
                    raise BridgeDown(
                        "待っている間にブリッジが止まりました（%s）。" % tool
                    )
                raise TimeoutError(
                    "Vectorworks からの応答がありません（%s、%.0f 秒待ちました）。"
                    % (tool, timeout)
                )
            time.sleep(0.05)

    # --- 道具の一覧（真実はプラグイン側にある）---------------------------
    def tools(self):
        """`vw_tools` を取りに行く。取れなければ前回のものを使う。"""
        try:
            response = self.call("vw_tools", {}, timeout=5.0)
            if response.get("ok"):
                tools = response.get("result", {}).get("tools", [])
                if isinstance(tools, list) and tools:
                    self._save_cache(tools)
                    return tools
        except (BridgeDown, TimeoutError, OSError) as error:
            log("道具の一覧を取りに行けませんでした: %s" % error)
        return self._load_cache()

    def _cache_path(self):
        return os.path.join(self.dir, TOOLS_CACHE_FILE)

    def _save_cache(self, tools):
        try:
            os.makedirs(self.dir, exist_ok=True)
            temp = self._cache_path() + ".cache-tmp"
            with open(temp, "w", encoding="utf-8") as handle:
                json.dump(tools, handle, ensure_ascii=False)
            os.replace(temp, self._cache_path())
        except OSError:
            pass

    def _load_cache(self):
        try:
            with open(self._cache_path(), "r", encoding="utf-8") as handle:
                tools = json.load(handle)
            if isinstance(tools, list):
                return tools
        except (OSError, ValueError):
            pass
        return []


# --- このサーバ自身が答える道具 ----------------------------------------------
# **ブリッジが動いていなくても答えられる**ことが要件（「なぜ繋がらないのか」を
# Claude 自身が調べられるように）。
STATUS_TOOL = {
    "name": "vw_bridge_status",
    "description": (
        "Vectorworks 側のブリッジが動いているかを確かめる。"
        "動いていないときは、どうすれば動くかを返す。"
    ),
    "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
}


def bridge_status_result(bridge):
    status = bridge.status()
    if status is None:
        return {
            "running": False,
            "spool": bridge.dir,
            "hint": (
                "Vectorworks のメニュー「MCP ブリッジを開始…」を実行してください。"
                "実行中は Vectorworks が進捗ダイアログの中で待ち、"
                "［キャンセル］か vw_stop_bridge で止まります。"
            ),
        }
    result = {"running": True, "spool": bridge.dir}
    result.update(status)
    return result


# --- MCP（JSON-RPC over stdio）------------------------------------------------


def rpc_result(request_id, result):
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def rpc_error(request_id, code, message):
    return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}


def text_content(text, is_error=False):
    """MCP の tools/call が返す形。中身は JSON テキスト 1 つ。"""
    return {"content": [{"type": "text", "text": text}], "isError": is_error}


def handle_tools_call(bridge, params):
    name = params.get("name", "")
    args = params.get("arguments") or {}

    if name == STATUS_TOOL["name"]:
        return text_content(
            json.dumps(bridge_status_result(bridge), ensure_ascii=False, indent=2)
        )

    try:
        response = bridge.call(name, args)
    except (BridgeDown, TimeoutError) as error:
        return text_content(str(error), is_error=True)
    except OSError as error:
        return text_content("スプールへ書けませんでした: %s" % error, is_error=True)

    if not response.get("ok"):
        return text_content(
            "Vectorworks 側でエラーになりました: %s" % response.get("error", "(理由不明)"),
            is_error=True,
        )
    return text_content(
        json.dumps(response.get("result", {}), ensure_ascii=False, indent=2)
    )


def handle(bridge, message):
    """1 件のリクエストに答える（通知なら None）。"""
    method = message.get("method", "")
    request_id = message.get("id")
    params = message.get("params") or {}

    if method == "initialize":
        return rpc_result(
            request_id,
            {
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            },
        )
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "ping":
        return rpc_result(request_id, {})
    if method == "tools/list":
        return rpc_result(request_id, {"tools": [STATUS_TOOL] + bridge.tools()})
    if method == "tools/call":
        return rpc_result(request_id, handle_tools_call(bridge, params))
    if request_id is None:
        return None  # 知らない通知は黙って捨てる
    return rpc_error(request_id, -32601, "知らないメソッドです: %s" % method)


def main():
    bridge = Bridge(spool_dir())
    log("接続先: %s" % bridge.dir)
    if bridge.status() is None:
        log("いまブリッジは動いていません（Vectorworks でメニューを実行してください）。")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            continue
        try:
            reply = handle(bridge, message)
        except Exception as error:  # サーバごと落とさない
            log("処理中の例外: %r" % error)
            reply = rpc_error(message.get("id"), -32603, "内部エラー: %s" % error)
        if reply is not None:
            sys.stdout.write(json.dumps(reply, ensure_ascii=False) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
