#!/usr/bin/env python3
#
# vw-mcp-server.py — Claude と Vectorworks をつなぐ MCP サーバ（ブリッジの Claude 側）
#
#   Claude ──MCP(stdio/JSON-RPC)──▶ このスクリプト
#                                     │ <id>.req.json を書く／<id>.res.json を待つ
#                                     ▼
#                        スプール（一時ディレクトリの min-nano_structure-mcp）
#                                     ▲
#                                     │ 拾う／応える
#                               Vectorworks（起動している間ずっと。パレットの時計が拾う）
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
# 【Vectorworks を起こすのもこちら】道具 `vw_launch` が Vectorworks を起動し、橋が架かる
# まで待つ（launch_vectorworks）。起動の口は OS の作法にだけ頼る——macOS は `open -a`、
# Windows は既定のインストール先の実行ファイル。プラグインは起動していないので何も訊けない。
#
# 【スプールは探す】プラグイン側は自分の一時ディレクトリへ置くが、一時ディレクトリは
# 環境変数で決まるので両側で食い違いうる。**実機ではこれが実際に起きた**——Claude の
# デスクトップアプリはこのサーバを $TMPDIR の無い環境で起動するので gettempdir() は
# /tmp に落ちるが、Vectorworks（GUI アプリ）のそれは利用者ごとの /var/folders/…/T/ で、
# 橋は動いているのに見つけられなかった（DEV-NOTES M24）。そこでこちらが候補を順に見て、
# **生きた印がある場所**を使う（spool_candidates / Bridge.status）。利用者ごとの一時
# ディレクトリは環境変数ではなく利用者から決まるので、confstr で直に引ける。
#
# 環境変数:
#   VW_MCP_SPOOL   スプールの場所を明示する（プラグイン側と同じ値にすること）
#   VW_MCP_PLUGIN  プラグイン名（既定 min-nano_structure。開発版は min-nano_structureDev）
#   VW_MCP_TIMEOUT 1 件あたりの待ち時間（秒。既定 30）
#   VW_MCP_APP     vw_launch が起動するもの（macOS は .app のパスかアプリ名、Windows は
#                  .exe のパス。既定は Vectorworks 2026 の標準のインストール先）
#
# 【受け渡しの作法はプラグイン側と対になっている】ファイル名の綴り・原子的な書き方
# （.tmp へ書いてから rename）・生存の印の見方は src/core/Bridge.h に書いてある。
# どちらかを変えるときは必ず両方を直す。

import glob
import json
import os
import secrets
import stat
import subprocess
import sys
import tempfile
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

# vw_launch の既定。**プラグインは Vectorworks 2026 用**なので、その版だけを探す
# （別の版を起こしても、このプラグインは読み込まれない）。
DEFAULT_MAC_APP = "Vectorworks 2026"
DEFAULT_WIN_EXE_GLOBS = (
    r"%ProgramFiles%\Vectorworks 2026\Vectorworks2026.exe",
    r"%ProgramFiles%\Vectorworks 2026*\Vectorworks*.exe",
)
# 橋が架かるまで待つ既定（秒）。起動そのものに数十秒かかり、パレットの最初の刻みは
# さらに数秒遅らせてある（resources/common.vwr/html/mcp.html の FIRST_TICK_MS）。
DEFAULT_LAUNCH_WAIT = 120.0
LAUNCH_POLL_SECONDS = 1.0


def log(message):
    """診断は stderr へ（stdout は JSON-RPC 専用）。"""
    print("[vw-mcp] " + message, file=sys.stderr, flush=True)


# confstr の名前。**CPython の os.confstr_names には載っていない**ので、名前では引けず
# 番号で引く（macOS の <unistd.h> の _CS_DARWIN_USER_TEMP_DIR）。
CS_DARWIN_USER_TEMP_DIR = 65537


def darwin_user_temp_dir():
    """macOS の利用者ごとの一時ディレクトリ（`/var/folders/…/T/`）を環境変数に頼らず引く。

    **実機で繋がらなかったのは正にここである。** Claude のデスクトップアプリは MCP サーバを
    **$TMPDIR の無い環境で起動する**ので、こちらの `gettempdir()` は `/tmp` に落ちる。
    一方 Vectorworks（GUI アプリ）の一時ディレクトリは利用者ごとの `/var/folders/…/T/` で、
    スプールはそちらに在る——探す場所が `/tmp` だけになり、動いている橋を見つけられない。

    この値は環境変数ではなく利用者から決まるので、**同じ利用者なら両側で必ず一致する**。
    引き方は 3 手: 名前（将来 CPython の表に載ったとき）→ 番号 → getconf(1)。
    """
    if sys.platform != "darwin":
        return ""
    for name in ("CS_DARWIN_USER_TEMP_DIR", CS_DARWIN_USER_TEMP_DIR):
        try:
            value = os.confstr(name)
        except (AttributeError, OSError, ValueError):
            # 名前が表に無い（いまの CPython はこちら）か、この環境には無い。次の手へ。
            continue
        if value:
            return value
    try:
        done = subprocess.run(
            ["/usr/bin/getconf", "DARWIN_USER_TEMP_DIR"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        # getconf が無い・動かない。候補が 1 つ減るだけで、下の走査がまだ残っている。
        return ""
    if done.returncode != 0:
        return ""
    return done.stdout.decode("utf-8", "replace").strip()


def spool_is_safe(directory):
    """そのスプールを使ってよいか（持ち主と権限）。

    **`/tmp` は同じ計算機の誰でも書ける。** 偽の印を置かれれば、こちらは要求をそこへ書いて
    しまい——引数（レイヤ名など）が漏れ、偽の応答を掴まされる。プラグイン側は自分が作る
    場所を 0700・自分の持ち物に限っている（`src/core/Bridge.cpp` の prepare）ので、
    こちらも同じ物差しで見て、合わないものは使わない。
    """
    if os.name == "nt":
        # Windows の ACL は stat では測れない。プラグイン側と同じくここでは見ない。
        return True
    try:
        info = os.stat(directory)
    except OSError:
        return False  # 無い（大半はこちら）。「ここではない」で正しい。
    if not stat.S_ISDIR(info.st_mode):
        return False
    if info.st_uid != os.geteuid():
        return False
    return (info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)) == 0


def spool_candidates():
    """スプールの候補を、確からしい順に並べて返す。

    **探すのはこちらの仕事である。** プラグイン側は自分の一時ディレクトリへ素直に置く
    （`<temp>/<プラグイン名>-mcp`）が、一時ディレクトリは環境変数で決まるので**両側で
    食い違いうる**——macOS の $TMPDIR は利用者ごとの `/var/folders/…` で、Claude の
    デスクトップアプリから起動されたこちらにはそれが無く `/tmp` に落ちる。そこで候補を
    順に見て、**生きた印（bridge.json）があるところ**を使う（Bridge.status）。

    **当てずっぽうは持たない。** 候補はどれも「プラグイン側が一時ディレクトリを決めるのに
    使うのと同じ仕組み」から出したものだけにする——利用者ごとの一時ディレクトリ（confstr）と、
    環境変数から来る場所（`gettempdir` / `TMPDIR` / `TMP` / `TEMP`）。**`/tmp` への
    フォールバックや `/var/folders` の総当たりは置かない**: 前者は同じ計算機の誰でも書ける
    場所で、後者は同じ利用者の**別のセッション**の橋を掴みうる。どちらも「たまたま繋がる」
    ことがあり、そのとき何処へ繋がったのかが分かりにくい。**場所が知れているなら
    `VW_MCP_SPOOL` で名指しするほうが確かで、分からないなら繋がらないほうが良い。**

    VW_MCP_SPOOL が指定されていれば、それだけを候補にする（両側で同じ値にすること）。
    """
    override = os.environ.get("VW_MCP_SPOOL", "")
    if override:
        return [override]

    plugin = os.environ.get("VW_MCP_PLUGIN", "") or DEFAULT_PLUGIN
    roots = []

    def add(root):
        # **末尾の区切りを落としてから見比べる。** `$TMPDIR` は `/var/…/T/` の形で来るが
        # `getconf` や利用者の設定は `/T` のこともあり、揃えないと同じ場所が
        # `searched` に 2 行並ぶ（繋がらないときに読むのは正にこの一覧なので、濁らせない）。
        root = root.rstrip("/\\") or root
        if root and root not in roots:
            roots.append(root)

    # **利用者ごとの一時ディレクトリを最初に見る。** GUI アプリ（＝Vectorworks）が使うのは
    # ここで、$TMPDIR を渡されないこちらの gettempdir() は /tmp に落ちるため。
    user_temp = darwin_user_temp_dir()
    add(user_temp)
    add(tempfile.gettempdir())
    for name in ("TMPDIR", "TMP", "TEMP"):
        add(os.environ.get(name, ""))

    if user_temp:
        # **macOS で利用者ごとの場所が引けたなら、/tmp は見ない。** そこは Vectorworks の
        # 一時ディレクトリになり得ない（GUI アプリは launchd から /var/folders/…/T/ を
        # 受け取る）ので、残しても当たらないか、誰かが置いたものに当たるかのどちらかになる。
        # 上の gettempdir() が $TMPDIR 不在で /tmp に落ちた結果もここで消える。
        roots = [root for root in roots if root != "/tmp"]

    return [os.path.join(root, plugin + "-mcp") for root in roots]


def call_timeout():
    try:
        return float(os.environ.get("VW_MCP_TIMEOUT", "") or DEFAULT_TIMEOUT)
    except ValueError:
        return DEFAULT_TIMEOUT


class BridgeDown(Exception):
    """Vectorworks 側でブリッジが動いていない（＝メニューを実行していない）。"""


class Bridge:
    """スプール越しに Vectorworks を呼ぶ（1 往復＝ファイル 2 つ）。"""

    def __init__(self, candidates):
        self.candidates = candidates
        # いまの当て（生きた印が見つかるまでは先頭。案内の文言にも使う）。
        self.dir = candidates[0] if candidates else ""
        self.seq = 0

    # --- 生存確認 ---------------------------------------------------------
    @staticmethod
    def _read_status(directory):
        """その場所の印を読む。生きていなければ None。"""
        if not spool_is_safe(directory):
            # 持ち主か権限が違う（＝誰かが置いた偽物かもしれない）。使わない。
            return None
        try:
            with open(os.path.join(directory, STATUS_FILE), "r", encoding="utf-8") as handle:
                status = json.load(handle)
        except (OSError, ValueError):
            # 無い（大半はこちら）か、書きかけを読んだ。どちらも「ここではない」。
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

    def status(self):
        """動いていれば status の dict、動いていなければ None。

        **見つけた場所を憶える。** 以降の要求はそこへ置く。
        """
        for directory in self.candidates:
            status = self._read_status(directory)
            if status is not None:
                self.dir = directory
                return status
        return None

    def require_status(self):
        status = self.status()
        if status is None:
            raise BridgeDown(
                "Vectorworks 側でブリッジが動いていません。\n"
                "Vectorworks が起動していなければ vw_launch で起動してください。"
                "起動しているのに繋がらないときは、Vectorworks のメニュー"
                "「MCP ブリッジを表示…」を 1 回実行してください。\n"
                "（探した場所: %s）" % ", ".join(self.candidates)
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
                    # 消せなくても応答は手に入っている。残骸は次の開始時に
                    # プラグイン側が掃除する（src/core/Bridge.h の sweep）。
                    pass
                return response
            except (OSError, ValueError):
                # **まだ書かれていない**（大半はこちら）か、書きかけを読んだ。
                # どちらも「もう一度見る」が正しい——下の締切までは回り続ける。
                pass
            if time.time() >= deadline:
                # 置いたままの要求を引き上げる（次のセッションが拾わないように）。
                try:
                    os.remove(request_path)
                except OSError:
                    # 引き上げられなくても害は小さい（拾われれば応答が 1 つ残るだけで、
                    # それも次の開始時に掃除される）。諦めた理由は下で必ず伝える。
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
            # **キャッシュは無くても困らない**（次にブリッジへ繋がったときに取り直す）。
            # 書けないことを理由に道具の一覧を返せなくするほうが困る。
            pass

    def _load_cache(self):
        # まだ繋がっていないと self.dir は当てでしかないので、候補を順に見る。
        for directory in self.candidates:
            if not spool_is_safe(directory):
                continue  # 偽物かもしれない一覧を Claude に見せない（_read_status と同じ）。
            try:
                with open(os.path.join(directory, TOOLS_CACHE_FILE), "r", encoding="utf-8") as h:
                    tools = json.load(h)
                if isinstance(tools, list):
                    return tools
            except (OSError, ValueError):
                continue
        try:
            with open(self._cache_path(), "r", encoding="utf-8") as handle:
                tools = json.load(handle)
            if isinstance(tools, list):
                return tools
        except (OSError, ValueError):
            # 一度も繋がっていない（＝キャッシュが無い）か、壊れている。
            # どちらも「一覧はまだ分からない」で、下の空リストがその答え。
            pass
        return []


# --- このサーバ自身が答える道具 ----------------------------------------------
# **ブリッジが動いていなくても答えられる**ことが要件（「なぜ繋がらないのか」を
# Claude 自身が調べられるように）。
LAUNCH_TOOL = {
    "name": "vw_launch",
    "description": (
        "Vectorworks を起動し、ブリッジが受け付けるまで待つ。"
        "既に受け付けていれば何もしない。"
    ),
    "inputSchema": {
        "type": "object",
        "properties": {
            "wait": {
                "type": "boolean",
                "description": "ブリッジが受け付けるまで待つか（既定 true）",
            },
            "timeout": {
                "type": "number",
                "description": "待つ上限（秒。既定 %d）" % int(DEFAULT_LAUNCH_WAIT),
            },
        },
        "additionalProperties": False,
    },
}

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
            "searched": bridge.candidates,
            "hint": (
                "Vectorworks が起動していなければ vw_launch で起動してください。"
                "起動していれば、Vectorworks のメニュー「MCP ブリッジを表示…」を 1 回"
                "実行してください（パレットが出て、以後は Vectorworks が終わるまで"
                "受け付けます）。"
                "それでも見つからないときは、開発版のプラグイン名"
                "（環境変数 VW_MCP_PLUGIN に min-nano_structureDev）か、"
                "スプールの場所（環境変数 VW_MCP_SPOOL）を確かめてください。"
            ),
        }
    result = {"running": True, "spool": bridge.dir}
    result.update(status)
    return result


# --- Vectorworks を起こす（vw_launch）-----------------------------------------
# **ブリッジが動いていなくても答えられる**道具のもう 1 つ。橋の向こう（プラグイン）は
# Vectorworks が起動するまで居ないので、起こすのはこちらの仕事になる。


def launch_command():
    """Vectorworks を起動するコマンド（argv）と、人に見せる名前を返す。見つからなければ
    (None, 理由)。

    **起動の口は OS の作法にだけ頼る。** macOS は `open -a`（アプリ名でも .app のパスでも
    LaunchServices が引く。既に動いていれば手前に出すだけで、2 つ目は起きない）。Windows は
    既定のインストール先の実行ファイルを探す。VW_MCP_APP で名指しできる——そのときは
    macOS でも .app で終わらなければ実行ファイルとしてそのまま起こす（テストの代役もこの経路）。
    """
    override = os.environ.get("VW_MCP_APP", "")
    if sys.platform == "darwin":
        app = override or DEFAULT_MAC_APP
        if override and not override.rstrip("/").endswith(".app"):
            if not os.path.isfile(override):
                return None, "VW_MCP_APP が指すファイルがありません: %s" % override
            return [override], override
        return ["/usr/bin/open", "-a", app], app
    if override:
        if not os.path.isfile(override):
            return None, "VW_MCP_APP が指すファイルがありません: %s" % override
        return [override], override
    if os.name == "nt":
        for pattern in DEFAULT_WIN_EXE_GLOBS:
            found = sorted(glob.glob(os.path.expandvars(pattern)))
            if found:
                return [found[0]], found[0]
        return None, (
            "Vectorworks 2026 が既定の場所（%s）に見つかりません。"
            "環境変数 VW_MCP_APP に Vectorworks2026.exe のパスを書いてください。"
            % os.path.expandvars(r"%ProgramFiles%\Vectorworks 2026")
        )
    return None, "この OS では Vectorworks を起動できません（VW_MCP_APP で名指ししてください）。"


def windows_process_running(exe_path):
    """Windows で同じ実行ファイルが既に動いているか（分からなければ False）。

    **Windows では 2 つ目を起こさない。** macOS の `open -a` と違い、実行ファイルを直に
    起こすと 2 つ目の Vectorworks が立ち上がりうる。橋が見えないのに動いているなら、
    足りないのはパレット（メニュー「MCP ブリッジを表示…」）のほうである。
    """
    if os.name != "nt":
        return False
    name = os.path.basename(exe_path)
    try:
        done = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq %s" % name, "/NH", "/FO", "CSV"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return ('"%s"' % name.lower()) in done.stdout.decode("utf-8", "replace").lower()


def spawn_detached(argv):
    """起こして手を放す（このサーバが終わっても Vectorworks は残る）。"""
    kwargs = {
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
        "close_fds": True,
    }
    if os.name == "nt":
        kwargs["creationflags"] = 0x00000008 | 0x00000200  # DETACHED | NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen(argv, **kwargs)


def launch_timeout(args):
    try:
        value = float(args.get("timeout", DEFAULT_LAUNCH_WAIT))
    except (TypeError, ValueError):
        value = DEFAULT_LAUNCH_WAIT
    return max(0.0, min(value, 600.0))


def launch_vectorworks(bridge, args, notify):
    """vw_launch の中身。結果（dict）と、エラーかどうかを返す。

    notify は橋が架かったときに呼ぶ（MCP の tools/list_changed を送る——起動前の一覧は
    自前の道具だけなので、Claude に取り直させる）。
    """
    status = bridge.status()
    if status is not None:
        return {"launched": False, "running": True, "spool": bridge.dir,
                "note": "既にブリッジが受け付けています。"}, False

    argv, label = launch_command()
    if argv is None:
        return {"launched": False, "running": False, "error": label}, True

    if windows_process_running(argv[0]):
        return {
            "launched": False,
            "running": False,
            "error": (
                "Vectorworks は起動していますが、ブリッジが受け付けていません。"
                "Vectorworks のメニュー「MCP ブリッジを表示…」を 1 回実行してください。"
            ),
            "searched": bridge.candidates,
        }, True

    try:
        spawn_detached(argv)
    except OSError as error:
        return {"launched": False, "running": False,
                "error": "起動できませんでした（%s）: %s" % (label, error)}, True
    log("Vectorworks を起動しました: %s" % " ".join(argv))

    wait = args.get("wait", True)
    if wait is False:
        return {"launched": True, "running": False, "app": label,
                "note": "起動だけしました。受け付けたかは vw_bridge_status で確かめてください。"}, False

    deadline = time.time() + launch_timeout(args)
    while True:
        status = bridge.status()
        if status is not None:
            notify()
            result = {"launched": True, "running": True, "app": label, "spool": bridge.dir}
            result.update(status)
            return result, False
        if time.time() >= deadline:
            break
        time.sleep(LAUNCH_POLL_SECONDS)

    return {
        "launched": True,
        "running": False,
        "app": label,
        "searched": bridge.candidates,
        "hint": (
            "Vectorworks は起動しましたが、待っている間にブリッジが受け付けませんでした。"
            "起動に時間がかかっているなら、少し待って vw_bridge_status で確かめてください。"
            "Vectorworks が開いているのに受け付けないときは、メニュー"
            "「MCP ブリッジを表示…」を 1 回実行してください（パレットが開いていないと"
            "受け付けません）。"
        ),
    }, True


# --- MCP（JSON-RPC over stdio）------------------------------------------------


def rpc_result(request_id, result):
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def rpc_error(request_id, code, message):
    return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}


def text_content(text, is_error=False):
    """MCP の tools/call が返す形。中身は JSON テキスト 1 つ。"""
    return {"content": [{"type": "text", "text": text}], "isError": is_error}


def send(message):
    """stdout へ JSON-RPC を 1 行（応答も通知も同じ口を通す）。"""
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def notify_tools_changed():
    """道具の一覧が変わったと Claude へ知らせる（橋が架かって本物の一覧が取れるようになった）。"""
    send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})


def handle_tools_call(bridge, params):
    name = params.get("name", "")
    args = params.get("arguments") or {}

    if name == STATUS_TOOL["name"]:
        return text_content(
            json.dumps(bridge_status_result(bridge), ensure_ascii=False, indent=2)
        )
    if name == LAUNCH_TOOL["name"]:
        result, is_error = launch_vectorworks(bridge, args, notify_tools_changed)
        return text_content(json.dumps(result, ensure_ascii=False, indent=2), is_error=is_error)

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
                # **一覧は変わる。** Vectorworks が起動していない間は自前の道具しか
                # 返せないので、vw_launch で橋が架かったら取り直してもらう。
                "capabilities": {"tools": {"listChanged": True}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            },
        )
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "ping":
        return rpc_result(request_id, {})
    if method == "tools/list":
        return rpc_result(request_id, {"tools": [STATUS_TOOL, LAUNCH_TOOL] + bridge.tools()})
    if method == "tools/call":
        return rpc_result(request_id, handle_tools_call(bridge, params))
    if request_id is None:
        return None  # 知らない通知は黙って捨てる
    return rpc_error(request_id, -32601, "知らないメソッドです: %s" % method)


def main():
    bridge = Bridge(spool_candidates())
    log("探す場所: %s" % ", ".join(bridge.candidates))
    if bridge.status() is None:
        log("いまブリッジは動いていません（vw_launch で Vectorworks を起動できます）。")

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
            send(reply)


if __name__ == "__main__":
    main()
