#!/usr/bin/env python3
#
# vw-mcp-server.py — Claude と Vectorworks をつなぐ MCP サーバ（ブリッジの Claude 側）
#
#   Claude ──MCP(stdio/JSON-RPC)──▶ このスクリプト
#                                     │ <id>.req.json を書く／<id>.res.json を待つ
#                                     ▼
#                        スプール（一時ディレクトリの min-nano_structureDev-mcp）
#                                     ▲
#                                     │ 読み取る／応答する
#                               Vectorworks（起動している間ずっと。殻の時計が読み取る。M41）
#
# 【開発版専用】（M38）ブリッジを持つのは開発版（min-nano_structureDev）のプラグインだけで、
# ローカルの Claude Code がこのリポジトリを開いたとき `.mcp.json` からこのサーバを起動する。
# 図面を読むだけでなく、診断ログと実機テストの報告を読み、新しいビルドを入れ、再起動し、
# 実機テストを走らせられる——**PR のコメントを介さずに実機確認を回す**ための道具である。
#
# 【このスクリプトが持たないもの】**道具の一覧を持たない。** 何ができるか（名前・説明・
# 引数の形・待ち時間）はプラグイン側の表（src/draw/McpBridge.cpp の kTools）ただ 1 つが
# 真実で、ここは起動時にそれを `vw_tools` で取得するだけ。だから**道具を追加するのにこの
# スクリプトを直す必要が無い**（プラグインを更新すれば増える）。例外は 2 つで、どちらも
# プラグイン側が動いていない間の状態はこちらからしか確認できないので持つ:
#   * 再起動の前後の待ち方（vw_restart / vw_update の restarting）… ブリッジが一度停止
#     するのを確認する（wait_for_restart）。
#   * 実機テストの開始のしかた（vw_run_test）… Vectorworks が起動していなければ、起動して
#     ブリッジが受け付けるようになってから要求する（M40。call_with_launch）。
#
# 【複数のセッションから呼ばれる】（M42）このサーバは Claude のセッションごとに 1 つ起動するが、
# Vectorworks は 1 つしか起動できない。ブリッジへ届く操作は**占有しているセッションだけ**が
# 送れ、他は断られて vw_lock_status で状況を購読する（Lease）。どの道具にも同じにかかるので、
# 道具の一覧を持たないことの例外ではない。
#
# 【依存を持たない】標準ライブラリだけで書いてある。プラグインの zip に同梱して配るので、
# 利用者に pip を要求しないことが要件（Python 3.8 以降）。
#
# 使い方（Claude Code に登録する）:
#
#   このリポジトリの .mcp.json が登録する（ローカルの Claude Code をリポジトリ直下で起動
#   すれば使える）。別の場所から使うなら:
#   claude mcp add vectorworks -- python3 <この scripts/mcp/vw-mcp-server.py のパス>
#
# 【Vectorworks の起動もこちらが担う】道具 `vw_launch` が Vectorworks を起動し、ブリッジが
# 受け付けるまで待つ（launch_vectorworks）。起動の手段は OS の標準の方法にだけ頼る——macOS
# は `open -a`、Windows は既定のインストール先の実行ファイル。起動前はプラグインが動いて
# いないので、プラグインには何も問い合わせられない。
#
# 【スプールは探す】候補を順に調べ、**有効な生存の印がある場所**を使う（spool_candidates /
# Bridge.status）。プラグイン側は自分の一時ディレクトリへ置くが、一時ディレクトリは
# 環境変数で決まるので両側で食い違いうるためである。利用者ごとの一時ディレクトリは
# 環境変数ではなく利用者から決まるので、confstr で直接取得できる。
# 経緯: **実機ではこれが実際に起きた**——Claude のデスクトップアプリはこのサーバを
# $TMPDIR の無い環境で起動するので gettempdir() は /tmp になるが、Vectorworks（GUI
# アプリ）のそれは利用者ごとの /var/folders/…/T/ で、ブリッジは動いているのに見つけ
# られなかった（DEV-NOTES M24）。
#
# 環境変数:
#   VW_MCP_SPOOL   スプールの場所を明示する（プラグイン側と同じ値にすること）
#   VW_MCP_PLUGIN  プラグイン名（既定 min-nano_structureDev。ブリッジは開発版にしか無い）
#   VW_MCP_TIMEOUT 1 件あたりの待ち時間（秒。既定 30。道具の表が timeoutSeconds を
#                  持つものはそちらが優先——実機テストの 1 周は 1 分以上かかる）
#   VW_MCP_LEASE_IDLE 占有を解くまでの、最後の操作からの秒数（既定 3600。M42）
#   VW_MCP_LEASE   占有の状態を置く場所（拡張子の前まで。既定はスプールの第 1 候補の隣）
#   VW_MCP_SESSION_LABEL 占有の表示に使うこのセッションの名前（既定は作業ディレクトリとブランチ）
#   VW_MCP_APP     vw_launch が起動するもの（macOS は .app のパスかアプリ名、Windows は
#                  .exe のパス。既定は Vectorworks 2026 の標準のインストール先）
#
# 【受け渡しの作法はプラグイン側と対になっている】ファイル名の綴り・原子的な書き方
# （.tmp へ書いてから rename）・生存の印の判定方法は src/core/Bridge.h に書いてある。
# どちらかを変えるときは必ず両方を直す。

import glob
import json
import os
import secrets
import signal
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

# 生存の印がこれより古ければ「動いていない」と判定する（プラグイン側は数秒ごとに書き直す）。
STATUS_STALE_SECONDS = 15

# MCP の版（stdio + tools）。
MCP_PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "vectorworks-bridge"
SERVER_VERSION = "0.1.0"

DEFAULT_PLUGIN = "min-nano_structureDev"
DEFAULT_TIMEOUT = 30.0

# vw_launch の既定。**プラグインは Vectorworks 2026 用**なので、その版だけを探す
# （別の版を起動しても、このプラグインは読み込まれない）。
DEFAULT_MAC_APP = "Vectorworks 2026"
DEFAULT_WIN_EXE_GLOBS = (
    r"%ProgramFiles%\Vectorworks 2026\Vectorworks2026.exe",
    r"%ProgramFiles%\Vectorworks 2026*\Vectorworks*.exe",
)
# ブリッジが受け付けるまで待つ既定（秒）。起動そのものに数十秒かかり、殻の時計の最初の
# 呼び出しは起動からさらに 10 秒遅らせてある（src/Extensions/ExtMcpPalette.cpp の kClockFirstTickSeconds）。
DEFAULT_LAUNCH_WAIT = 120.0
LAUNCH_POLL_SECONDS = 1.0
# 再起動を要求してから、ブリッジが**一度停止するのを**待つ上限（秒）。保存の確認が出ていると
# Vectorworks はそこで止まるので、これを過ぎたら「人の応答待ち」と返す。
RESTART_DOWN_WAIT = 60.0


def log(message):
    """診断は stderr へ（stdout は JSON-RPC 専用）。"""
    print("[vw-mcp] " + message, file=sys.stderr, flush=True)


# confstr の名前。**CPython の os.confstr_names には載っていない**ので、名前では取得できず
# 番号で取得する（macOS の <unistd.h> の _CS_DARWIN_USER_TEMP_DIR）。
CS_DARWIN_USER_TEMP_DIR = 65537


def darwin_user_temp_dir():
    """macOS の利用者ごとの一時ディレクトリ（`/var/folders/…/T/`）を環境変数に頼らず取得する。

    この値は環境変数ではなく利用者から決まるので、**同じ利用者なら両側で必ず一致する**。
    取得方法は 3 段階: 名前（将来 CPython の表に載ったとき）→ 番号 → getconf(1)。

    **実機で接続できなかった原因はここである。** Claude のデスクトップアプリは MCP サーバを
    **$TMPDIR の無い環境で起動する**ので、こちらの `gettempdir()` は `/tmp` になる。
    一方 Vectorworks（GUI アプリ）の一時ディレクトリは利用者ごとの `/var/folders/…/T/` で、
    スプールはそちらに在る——探す場所が `/tmp` だけになり、動いているブリッジを見つけられない。
    """
    if sys.platform != "darwin":
        return ""
    for name in ("CS_DARWIN_USER_TEMP_DIR", CS_DARWIN_USER_TEMP_DIR):
        try:
            value = os.confstr(name)
        except (AttributeError, OSError, ValueError):
            # 名前が表に無い（いまの CPython はこちら）か、この環境には無い。次の方法へ。
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

    プラグイン側は自分が作る場所を 0700・自分の所有に限っている（`src/core/Bridge.cpp` の
    prepare）ので、こちらも同じ基準で判定し、合わないものは使わない。
    理由: **`/tmp` は同じ計算機の誰でも書ける。** 偽の印を置かれれば、こちらは要求をそこへ
    書いてしまい——引数（レイヤ名など）が漏れ、偽の応答を受け取ってしまう。
    """
    if os.name == "nt":
        # Windows の ACL は stat では判定できない。プラグイン側と同じくここでは確認しない。
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

    候補を順に調べ、**有効な生存の印（bridge.json）があるところ**を使う（Bridge.status）。
    VW_MCP_SPOOL が指定されていれば、それだけを候補にする（両側で同じ値にすること）。

    **推測による候補は持たない。** 候補はどれも「プラグイン側が一時ディレクトリを決めるのに
    使うのと同じ仕組み」から出したものだけにする——利用者ごとの一時ディレクトリ（confstr）と、
    環境変数から来る場所（`gettempdir` / `TMPDIR` / `TMP` / `TEMP`）。**`/tmp` への
    フォールバックや `/var/folders` の総当たりは置かない**: 前者は同じ計算機の誰でも書ける
    場所で、後者は同じ利用者の**別のセッション**のブリッジに接続しうる。どちらも「偶然
    接続できる」ことがあり、そのとき何処へ接続したのかが分かりにくい。**場所が分かって
    いるなら `VW_MCP_SPOOL` で明示するほうが確実で、分からないなら接続しないほうが良い。**

    理由: スプールを探すのはこちらの役割である。プラグイン側は自分の一時ディレクトリへ
    そのまま置く（`<temp>/<プラグイン名>-mcp`）が、一時ディレクトリは環境変数で決まるので
    **両側で食い違いうる**——macOS の $TMPDIR は利用者ごとの `/var/folders/…` で、Claude の
    デスクトップアプリから起動されたこちらにはそれが無く `/tmp` になる。
    """
    override = os.environ.get("VW_MCP_SPOOL", "")
    if override:
        return [override]

    plugin = os.environ.get("VW_MCP_PLUGIN", "") or DEFAULT_PLUGIN
    roots = []

    def add(root):
        # **末尾の区切りを除去してから比較する。** `$TMPDIR` は `/var/…/T/` の形で来るが
        # `getconf` や利用者の設定は `/T` のこともあり、揃えないと同じ場所が
        # `searched` に 2 行並ぶ（接続できないときに読むのは正にこの一覧なので、重複させない）。
        root = root.rstrip("/\\") or root
        if root and root not in roots:
            roots.append(root)

    # **利用者ごとの一時ディレクトリを最初に調べる。** GUI アプリ（＝Vectorworks）が使うのは
    # ここで、$TMPDIR を渡されないこちらの gettempdir() は /tmp になるため。
    user_temp = darwin_user_temp_dir()
    add(user_temp)
    add(tempfile.gettempdir())
    for name in ("TMPDIR", "TMP", "TEMP"):
        add(os.environ.get(name, ""))

    if user_temp:
        # **macOS で利用者ごとの場所が取得できたなら、/tmp は調べない。** そこは Vectorworks の
        # 一時ディレクトリになり得ない（GUI アプリは launchd から /var/folders/…/T/ を
        # 受け取る）ので、残しても見つからないか、誰かが置いたものを見つけるかのどちらかになる。
        # 上の gettempdir() が $TMPDIR 不在で /tmp になった結果もここで除外される。
        roots = [root for root in roots if root != "/tmp"]

    return [os.path.join(root, plugin + "-mcp") for root in roots]


def call_timeout():
    try:
        return float(os.environ.get("VW_MCP_TIMEOUT", "") or DEFAULT_TIMEOUT)
    except ValueError:
        return DEFAULT_TIMEOUT


# --- 占有（複数のセッションから同時に呼ばれたとき。M42）-------------------------
# **Vectorworks は 1 つしか起動できず、このサーバは Claude のセッションごとに 1 つ起動する。**
# 2 つのセッションが同時に使うと、片方の実機テストの最中にもう片方が更新・再起動を要求したり、
# 片方の周の報告をもう片方が自分のものとして読んだりする。そこで**ブリッジへ届く操作は、
# 占有しているセッションだけが行える**ようにする。
#
#   * 占有はプラグイン側の道具（読むものも含む）と vw_launch を最初に呼んだときに自動で取る。
#     読む道具も含めるのは、他の周の報告・描画途中の図面を自分の結果と取り違えないため。
#   * 他のセッションが占有している間は**要求をスプールへ書かずに断り**、誰が何をしているかを返す。
#     断られた側は vw_lock_status に wait を渡して、解放されるまで経過を受け取りながら待てる。
#   * 解放は vw_lock_release・このサーバの終了・最後の操作から LEASE_IDLE_SECONDS 経過・
#     持ち主のプロセスの消滅（同じ計算機なので確認できる）のいずれか。操作の最中は持ち主が
#     心拍（heartbeat）を書き続け、それが LEASE_BUSY_STALE_SECONDS 途絶えたら停止とみなす。
#   * **解放するときは実機テストを片付ける**（プラグインの vw_test_cleanup。実機テストの図面を
#     閉じ、一時ファイル・記憶・報告を消す）。持ち主が自分で片付けられずに解かれたとき
#     （時間切れ・異常終了・Vectorworks が居なかった）は「片付けが残っている」と記録し、
#     次に占有したセッションが自分の操作の前に片付ける（end_test_session）。
#
# 状態は 1 つの JSON（<スプールの第 1 候補>.lease.json）に置き、読み書きは OS のファイル
# ロック（.lease.lock）の中でだけ行う。ロックはプロセスが終われば OS が外すので、異常終了
# しても残らない。**場所はスプールの第 1 候補の隣**——Vectorworks が起動する前（vw_launch）
# にも決まっていて、同じ利用者のセッションどうしで一致する必要があるため（macOS は利用者
# ごとの一時ディレクトリ。spool_candidates）。スプールの中に置かないのは、プラグイン側が
# 開始時に掃除するから（src/core/Bridge.h の sweep）。
LEASE_SUFFIX = ".lease.json"
LEASE_MUTEX_SUFFIX = ".lease.lock"
# 最後の操作からこれだけ経てば占有を解く（秒。VW_MCP_LEASE_IDLE で変えられる）。**CI の
# 往復（push → dev ビルド → vw_update）を待つ間も占有し続けられる長さにする**——その間に
# 他のセッションが周を走らせると、待っていた側の記憶と図面が置き換わる。続けて使わないなら
# 持ち主が vw_lock_release で片付けて解放する（運用。docs/development/live-test/running.md）。
LEASE_IDLE_SECONDS = 3600.0
# 操作の最中に心拍を書く間隔と、途絶えたら停止とみなす長さ（秒）。持ち主が待つ最長の
# 一続きの処理は osascript の 10 秒なので、十分に長くとる。
LEASE_HEARTBEAT_SECONDS = 5.0
LEASE_BUSY_STALE_SECONDS = 120.0
LEASE_EVENTS_KEPT = 50
# vw_lock_status の wait の上限（秒）。実機テストの 1 周の上限（30 分）に揃える。
LEASE_WAIT_MAX = 1800.0
LEASE_WAIT_POLL_SECONDS = 1.0
# 実機テストを片付けるプラグイン側の道具（src/draw/McpBridge.cpp の kTools）。
CLEANUP_TOOL = "vw_test_cleanup"
# サーバの終了のときに片付けを待つ上限（秒）。Claude Code はセッションを閉じるとこのサーバを
# 止めるので、長く待たない（間に合わなければ次に占有したセッションが片付ける）。
SHUTDOWN_CLEANUP_TIMEOUT = 20.0


def lease_idle_seconds():
    try:
        value = float(os.environ.get("VW_MCP_LEASE_IDLE", "") or LEASE_IDLE_SECONDS)
    except ValueError:
        return LEASE_IDLE_SECONDS
    return value if value > 0 else LEASE_IDLE_SECONDS


def format_time(value):
    """人に見せる時刻（ローカル時刻）。"""
    if not isinstance(value, (int, float)):
        return None
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(value))


def process_alive(pid):
    """そのプロセスが動いているか。分からなければ True（占有を早まって解かない）。

    **Windows では確かめない。** os.kill(pid, 0) は Windows では「終了コード 0 で終了させる」
    意味になるので使えない。そちらは心拍と最後の操作の時刻だけで判定する。
    """
    if os.name == "nt" or not isinstance(pid, int) or pid <= 0:
        return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True  # 権限が無い＝別の利用者のプロセスとして動いている
    return True


def session_label():
    """このセッションを人が見分けるための名前（作業ディレクトリとブランチ）。

    VW_MCP_SESSION_LABEL があればそれを使う。このサーバは Claude Code がリポジトリ（または
    worktree）で起動するので、ディレクトリとブランチで「どの作業か」が分かる。
    """
    override = os.environ.get("VW_MCP_SESSION_LABEL", "")
    if override:
        return override
    cwd = os.getcwd()
    label = os.path.basename(cwd.rstrip("/\\")) or cwd
    try:
        done = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=2,
            check=False,
        )
        branch = done.stdout.decode("utf-8", "replace").strip()
        if done.returncode == 0 and branch:
            label += " (%s)" % branch
    except (OSError, subprocess.SubprocessError):
        # git が無い・リポジトリの外。ディレクトリ名だけでも見分けられる。
        pass
    return label


class FileMutex:
    """OS のファイルロック（排他）。プロセスが終われば OS が外す。"""

    def __init__(self, path):
        self.path = path
        self.handle = None

    def __enter__(self):
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)
        self.handle = os.fdopen(fd, "r+b")
        if os.name == "nt":
            import msvcrt

            while True:
                try:
                    msvcrt.locking(self.handle.fileno(), msvcrt.LK_LOCK, 1)
                    break
                except OSError:
                    # LK_LOCK は 10 秒で諦める。ロックを持つのは読み書きの一瞬だけなので、
                    # 待ち続けてよい。
                    time.sleep(0.05)
        else:
            import fcntl

            fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        try:
            if os.name == "nt":
                import msvcrt

                self.handle.seek(0)
                msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
        finally:
            self.handle.close()
            self.handle = None
        return False


class Lease:
    """ブリッジの占有（このサーバ＝1 つの Claude セッションが持つ）。"""

    def __init__(self, base):
        self.path = base + LEASE_SUFFIX
        self.mutex_path = base + LEASE_MUTEX_SUFFIX
        self.id = "%d-%s" % (os.getpid(), secrets.token_hex(4))
        self.pid = os.getpid()
        self.label = session_label()
        self.cwd = os.getcwd()
        self.last_beat = 0.0
        # 直近の begin で、前の持ち主の片付けを引き継いだならその名前（run_exclusive が片付ける）。
        self.inherited = None

    # --- 読み書き（必ず FileMutex の中で呼ぶ）--------------------------------
    def _read(self):
        try:
            with open(self.path, "r", encoding="utf-8") as handle:
                state = json.load(handle)
        except (OSError, ValueError):
            # 無い（まだ誰も占有していない）か壊れている。どちらも「空き」から始める。
            state = None
        if not isinstance(state, dict):
            state = {}
        state.setdefault("seq", 0)
        state.setdefault("holder", None)
        state.setdefault("events", [])
        return state

    def _write(self, state):
        parent = os.path.dirname(self.path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        temp = self.path + ".%d.tmp" % self.pid
        fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(state, handle, ensure_ascii=False)
        os.replace(temp, self.path)

    def _mutex(self):
        parent = os.path.dirname(self.mutex_path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        return FileMutex(self.mutex_path)

    @staticmethod
    def _event(state, who, text):
        state["seq"] = int(state.get("seq", 0)) + 1
        state["events"].append(
            {"seq": state["seq"], "time": time.time(), "session": who, "event": text}
        )
        del state["events"][:-LEASE_EVENTS_KEPT]

    @staticmethod
    def _lapse(holder, now):
        """占有が切れていればその理由、続いていれば None。"""
        if not process_alive(holder.get("pid")):
            return "セッションが終了していた"
        if holder.get("busy"):
            if now - float(holder.get("heartbeat") or 0) > LEASE_BUSY_STALE_SECONDS:
                return "%s の最中に応答が途絶えた" % holder.get("busy")
            return None
        if now - float(holder.get("last_used") or 0) > lease_idle_seconds():
            return "最後の操作から %d 秒経った" % int(lease_idle_seconds())
        return None

    def _settle(self, state, now):
        """切れた占有を解く。解いたら True。"""
        holder = state.get("holder")
        if not isinstance(holder, dict):
            state["holder"] = None
            return False
        reason = self._lapse(holder, now)
        if reason is None:
            return False
        self._event(state, holder.get("label"), "占有を解いた（%s）" % reason)
        state["holder"] = None
        # **持ち主は自分で片付けられなかった。** 次に占有したセッションが片付ける。
        state["cleanup_pending"] = holder.get("label") or "(不明)"
        return True

    # --- 操作 ----------------------------------------------------------------
    def begin(self, tool):
        """tool を始める。始められれば None、他が占有していれば状況（view）を返す。"""
        with self._mutex():
            now = time.time()
            state = self._read()
            self._settle(state, now)
            holder = state["holder"]
            if holder is not None and holder.get("id") != self.id:
                self._write(state)
                return self._view(state, now)
            self.inherited = None
            if holder is None:
                # 前の持ち主の片付けが残っていれば引き継ぐ（自分の操作の前に片付ける）。
                self.inherited = state.pop("cleanup_pending", None)
                holder = {
                    "id": self.id,
                    "pid": self.pid,
                    "label": self.label,
                    "cwd": self.cwd,
                    "acquired": now,
                }
                state["holder"] = holder
                self._event(state, self.label, "占有した")
            holder["busy"] = tool
            holder["busy_since"] = now
            holder["heartbeat"] = now
            holder["last_used"] = now
            self._event(state, self.label, "%s を始めた" % tool)
            self._write(state)
            self.last_beat = now
            return None

    def end(self, tool, outcome):
        with self._mutex():
            now = time.time()
            state = self._read()
            holder = state.get("holder")
            if not isinstance(holder, dict) or holder.get("id") != self.id:
                return  # 途中で解かれていた（心拍が途絶えたとみなされた）。何もしない。
            holder["busy"] = None
            holder["busy_since"] = None
            holder["heartbeat"] = now
            holder["last_used"] = now
            self._event(state, self.label, "%s を終えた（%s）" % (tool, outcome))
            self._write(state)

    def beat(self):
        """操作の最中に心拍を書く（間隔は LEASE_HEARTBEAT_SECONDS まで間引く）。"""
        now = time.time()
        if now - self.last_beat < LEASE_HEARTBEAT_SECONDS:
            return
        self.last_beat = now
        try:
            with self._mutex():
                state = self._read()
                holder = state.get("holder")
                if isinstance(holder, dict) and holder.get("id") == self.id:
                    holder["heartbeat"] = now
                    self._write(state)
        except OSError as error:
            # 心拍が書けなくても操作は続ける（途絶えが続けば他から解かれるだけ）。
            log("占有の心拍を書けませんでした: %s" % error)

    def holds(self):
        """いま自分が占有しているか。"""
        with self._mutex():
            holder = self._read().get("holder")
            return isinstance(holder, dict) and holder.get("id") == self.id

    def note(self, text):
        """出来事を 1 つ記録する（片付けの結果など。購読している側へ届く）。"""
        with self._mutex():
            state = self._read()
            self._event(state, self.label, text)
            self._write(state)

    def release(self, reason, cleanup_pending=False):
        """自分の占有を解く。解いたら True。

        cleanup_pending は「片付けられなかった」（次に占有したセッションへ引き継ぐ）。
        """
        with self._mutex():
            state = self._read()
            holder = state.get("holder")
            if not isinstance(holder, dict) or holder.get("id") != self.id:
                return False
            state["holder"] = None
            if cleanup_pending:
                state["cleanup_pending"] = self.label
            self._event(state, self.label, "占有を解いた（%s）" % reason)
            self._write(state)
            return True

    def snapshot(self):
        """いまの状況（view）。切れた占有はここでも解く。"""
        with self._mutex():
            now = time.time()
            state = self._read()
            if self._settle(state, now):
                self._write(state)
            return self._view(state, now)

    def _view(self, state, now):
        holder = state.get("holder")
        view = {
            "you": self.label,
            "seq": state.get("seq", 0),
            "events": [
                {
                    "seq": event.get("seq"),
                    "time": format_time(event.get("time")),
                    "session": event.get("session"),
                    "event": event.get("event"),
                }
                for event in state.get("events", [])[-10:]
                if isinstance(event, dict)
            ],
        }
        if state.get("cleanup_pending"):
            view["cleanup_pending"] = state.get("cleanup_pending")
        if not isinstance(holder, dict):
            view["state"] = "free"
            return view
        view["state"] = "yours" if holder.get("id") == self.id else "held"
        shown = {
            "session": holder.get("label"),
            "cwd": holder.get("cwd"),
            "pid": holder.get("pid"),
            "since": format_time(holder.get("acquired")),
        }
        if holder.get("busy"):
            shown["busy"] = holder.get("busy")
            shown["busy_since"] = format_time(holder.get("busy_since"))
            shown["busy_seconds"] = int(now - float(holder.get("busy_since") or now))
        else:
            idle = now - float(holder.get("last_used") or now)
            shown["idle_seconds"] = int(idle)
            shown["released_in_seconds"] = max(0, int(lease_idle_seconds() - idle))
        view["holder"] = shown
        return view

    def events_after(self, seq):
        """seq より後の出来事（購読のため。切れた占有はここでも解く）。"""
        with self._mutex():
            now = time.time()
            state = self._read()
            if self._settle(state, now):
                self._write(state)
            events = [
                event
                for event in state.get("events", [])
                if isinstance(event, dict) and int(event.get("seq", 0)) > seq
            ]
            return events, self._view(state, now)


def lease_base(candidates):
    """占有の状態を置く場所（拡張子の前まで）。スプールの第 1 候補の隣。

    VW_MCP_LEASE で明示できる（テストが本物の占有に触れないように使う）。
    """
    override = os.environ.get("VW_MCP_LEASE", "")
    if override:
        return override
    if candidates:
        return candidates[0].rstrip("/\\")
    plugin = os.environ.get("VW_MCP_PLUGIN", "") or DEFAULT_PLUGIN
    return os.path.join(tempfile.gettempdir(), plugin + "-mcp")


class BridgeDown(Exception):
    """Vectorworks 側でブリッジが動いていない（起動していないか、パレットが開いていない）。"""


class Bridge:
    """スプール越しに Vectorworks を呼ぶ（1 往復＝ファイル 2 つ）。"""

    def __init__(self, candidates):
        self.candidates = candidates
        # 現在の候補（有効な印が見つかるまでは先頭。案内の文言にも使う）。
        self.dir = candidates[0] if candidates else ""
        self.seq = 0
        # 最後の tools/list で Claude に示したプラグイン側の道具の名前。**これと食い違う一覧を
        # 取得したら list_changed を送る**（announce_if_changed）。
        self.listed = []
        # 最後に再取得を促した一覧（同じ一覧で何度も促さない——アプリが通知に応じない
        # 場合に、道具を呼ぶたびに通知が積み上がるのを防ぐ）。
        self.announced = []
        # 道具ごとの待ち時間（秒）。**表の真実はプラグイン側**（kTools の timeoutSeconds）で、
        # 一覧を取得するたびにここへコピーする（remember_timeouts）。
        self.timeouts = {}
        # 占有（M42）。操作の最中に待つループは tick() で心拍を書く。
        self.lease = Lease(lease_base(candidates))

    def tick(self):
        """待っている間に呼ぶ（占有の心拍）。"""
        self.lease.beat()

    # --- 生存確認 ---------------------------------------------------------
    @staticmethod
    def _read_status(directory):
        """その場所の印を読む。有効でなければ None。"""
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
        # **印が古ければ動いていない。** Vectorworks ごと異常終了した場合、印は残る。
        beat = status.get("beat")
        if not isinstance(beat, (int, float)):
            return None
        now = time.time()
        if now - float(beat) > STATUS_STALE_SECONDS:
            # **長く走る道具の最中は、印を書き直せない**（実機テストの 1 周）。プラグインは
            # 走り出す前に busy_until（いつまでかかりうるか）を書いていくので、それが未来なら
            # 動作中と判定する（src/draw/McpBridge.cpp の StatusJson）。
            busy_until = status.get("busy_until")
            if not isinstance(busy_until, (int, float)) or now > float(busy_until):
                return None
        return status

    def status(self):
        """動いていれば status の dict、動いていなければ None。

        **見つけた場所を記録する。** 以降の要求はそこへ置く。
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
                "起動しているのに繋がらないときは、起動から 10 秒ほど待つか、"
                "（M41 より古い開発版なら）図面を開いてメニュー"
                "「MCP ブリッジを表示…」（開発版だけ）を 1 回実行してください。\n"
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
    def remember_timeouts(self, tools):
        """一覧から道具ごとの待ち時間をコピーする。"""
        for tool in tools:
            if not isinstance(tool, dict):
                continue
            value = tool.get("timeoutSeconds")
            if isinstance(value, (int, float)) and value > 0:
                self.timeouts[tool.get("name")] = float(value)

    def call(self, tool, args, timeout=None):
        self.require_status()
        if timeout is None:
            timeout = max(call_timeout(), self.timeouts.get(tool, 0.0))

        self.seq += 1
        # 名前の昇順が送った順になるように連番を先頭へ置く（プラグイン側はこの順で読み取る）。
        request_id = "%012d-%s" % (self.seq, secrets.token_hex(4))
        payload = json.dumps(
            {"id": request_id, "tool": tool, "args": args or {}}, ensure_ascii=False
        )

        request_path = os.path.join(self.dir, request_id + REQUEST_SUFFIX)
        temp_path = request_path + ".tmp"
        os.makedirs(self.dir, exist_ok=True)
        with open(temp_path, "w", encoding="utf-8") as handle:
            handle.write(payload)
        # **書き終えてから公開する**（プラグイン側に書きかけの内容を読ませない）。
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
                    # 消せなくても応答は取得できている。残骸は次の開始時に
                    # プラグイン側が掃除する（src/core/Bridge.h の sweep）。
                    pass
                return response
            except (OSError, ValueError):
                # **まだ書かれていない**（大半はこちら）か、書きかけを読んだ。
                # どちらも「もう一度読む」が正しい——下の締切までは繰り返す。
                pass
            if time.time() >= deadline:
                # 置いたままの要求を取り下げる（次のセッションが読み取らないように）。
                try:
                    os.remove(request_path)
                except OSError:
                    # 取り下げられなくても害は小さい（読み取られれば応答が 1 つ残るだけで、
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
            self.tick()
            time.sleep(0.05)

    # --- 道具の一覧（真実はプラグイン側にある）---------------------------
    def tools(self):
        """`vw_tools` を取得する。取得できなければ前回のものを使う。"""
        try:
            response = self.call("vw_tools", {}, timeout=5.0)
            if response.get("ok"):
                tools = response.get("result", {}).get("tools", [])
                if isinstance(tools, list) and tools:
                    self._save_cache(tools)
                    self.remember_timeouts(tools)
                    return tools
        except (BridgeDown, TimeoutError, OSError) as error:
            log("道具の一覧を取りに行けませんでした: %s" % error)
        return self._load_cache()

    def live_tools(self):
        """いま動いているプラグインから一覧を取得する。取得できなければ None（キャッシュは使わない）。"""
        try:
            response = self.call("vw_tools", {}, timeout=5.0)
        except (BridgeDown, TimeoutError, OSError):
            return None
        if not response.get("ok"):
            return None
        tools = response.get("result", {}).get("tools", [])
        if not isinstance(tools, list):
            return None
        self._save_cache(tools)
        self.remember_timeouts(tools)
        return tools

    def announce_if_changed(self, tools, notify):
        """取得した一覧が最後に示したものと違えば、Claude に再取得を促す。

        vw_launch 以外の経路でブリッジが受け付けるようになったとき（人が Vectorworks を
        起動した・パレットが自動で開き直された）にも気付けるよう、ブリッジにアクセスした
        道具のたびにここを通す。
        理由: **Claude のアプリは tools/list を起動したときに 1 回しか呼ばないことが多い**
        （実機で、Vectorworks より先にアプリを起動すると図面を読む道具が最後まで表示され
        なかった。M30）。
        """
        names = [tool.get("name") for tool in tools if isinstance(tool, dict)]
        if names and names != self.listed and names != self.announced:
            self.announced = names
            notify()

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
            # **キャッシュは無くても困らない**（次にブリッジへ接続したときに再取得する）。
            # 書けないことを理由に道具の一覧を返せなくするほうが困る。
            pass

    def _load_cache(self):
        # まだ接続していないと self.dir は推定でしかないので、候補を順に調べる。
        for directory in self.candidates:
            if not spool_is_safe(directory):
                continue  # 偽物かもしれない一覧を Claude に見せない（_read_status と同じ）。
            try:
                with open(os.path.join(directory, TOOLS_CACHE_FILE), "r", encoding="utf-8") as h:
                    tools = json.load(h)
                if isinstance(tools, list):
                    self.remember_timeouts(tools)
                    return tools
            except (OSError, ValueError):
                continue
        try:
            with open(self._cache_path(), "r", encoding="utf-8") as handle:
                tools = json.load(handle)
            if isinstance(tools, list):
                return tools
        except (OSError, ValueError):
            # 一度も接続していない（＝キャッシュが無い）か、壊れている。
            # どちらも「一覧はまだ分からない」で、下の空リストがその答え。
            pass
        return []


def public_tools(tools):
    """Claude に示す形へ（プラグインの表にしか意味の無い timeoutSeconds を除去する）。

    MCP の tool は name / description / inputSchema を持つ。余計な鍵を嫌うアプリもあるので、
    待ち時間はこのサーバの中だけで使う（Bridge.remember_timeouts）。
    """
    shown = []
    for tool in tools:
        if isinstance(tool, dict):
            tool = {key: value for key, value in tool.items() if key != "timeoutSeconds"}
        shown.append(tool)
    return shown


# --- このサーバ自身が答える道具 ----------------------------------------------
# **ブリッジが動いていなくても答えられる**ことが要件（「なぜ接続できないのか」を
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
        "動いていれば、図面を読む道具（プラグイン側）の一覧も返す——道具の一覧に"
        "見えていない道具は vw_call で呼べる。"
        "動いていないときは、どうすれば動くかを返す。"
    ),
    "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
}

# **汎用の呼び出し口。** プラグイン側の道具（vw_layers など）を名前で呼ぶ。
#
# この口は常に一覧に並ぶので、一覧が古くても道具を呼び出せる。何を呼べるかは
# vw_bridge_status が返す（ここに道具の名前を重複して書かない）。
# 理由: 道具の一覧はプラグインから取得する（一覧の真実はプラグイン側）が、Claude の
# アプリは多くの場合 tools/list を起動時に 1 回しか呼ばず、list_changed の通知にも
# 応じないことがある。すると Vectorworks より先にアプリを起動しただけで、図面を読む
# 道具が**最後まで表示されない**（実機で起きた。M30）。
CALL_TOOL = {
    "name": "vw_call",
    "description": (
        "Vectorworks のプラグイン側の道具を名前で呼ぶ（道具の一覧に見えていないときに使う）。"
        "呼べる道具の名前と引数は vw_bridge_status の tools に並ぶ。"
    ),
    "inputSchema": {
        "type": "object",
        "properties": {
            "tool": {"type": "string", "description": "道具の名前（例: vw_layers）"},
            "arguments": {"type": "object", "description": "その道具への引数（省略可）"},
        },
        "required": ["tool"],
        "additionalProperties": False,
    },
}

# 占有の状況と購読（M42）。**他のセッションが使っている間も答えられる**ことが要件。
LOCK_STATUS_TOOL = {
    "name": "vw_lock_status",
    "description": (
        "Vectorworks をどの Claude セッションが占有しているか（何をしているか・いつ解放されるか）"
        "と、直近の出来事を返す。他のセッションが使用中で断られたときは wait（秒）を渡すと、"
        "解放されるまで（until=change なら次の出来事まで）待ち、その間の出来事を返す。"
    ),
    "inputSchema": {
        "type": "object",
        "properties": {
            "wait": {
                "type": "number",
                "description": "待つ上限（秒。既定 0＝待たない。上限 %d）" % int(LEASE_WAIT_MAX),
            },
            "until": {
                "type": "string",
                "enum": ["free", "change"],
                "description": "free（既定）は解放されるまで、change は次の出来事まで待つ",
            },
        },
        "additionalProperties": False,
    },
}

LOCK_RELEASE_TOOL = {
    "name": "vw_lock_release",
    "description": (
        "実機テストを片付けてから、このセッションの Vectorworks の占有を解く。"
        "片付けでは実機テストの図面を閉じ、一時ファイル・記憶・報告を消す。"
        "次の実行まで状態を持ち越さないなら（確認が済んだ・当分使わない）必ず呼ぶ。"
        "CI の往復を待って続けるなら呼ばない（最後の操作から 60 分は占有が続く）。"
    ),
    "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
}

# このサーバ自身が答える道具（ブリッジが動いていなくても一覧に並ぶ）。
LOCAL_TOOLS = [STATUS_TOOL, LAUNCH_TOOL, CALL_TOOL, LOCK_STATUS_TOOL, LOCK_RELEASE_TOOL]


def bridge_status_result(bridge, notify=None):
    status = bridge.status()
    if status is None:
        return {
            "running": False,
            "lock": bridge.lease.snapshot(),
            "searched": bridge.candidates,
            "hint": (
                "Vectorworks が起動していなければ vw_launch で起動してください。"
                "起動していれば、起動から 10 秒ほどで受け付け始めます（M41 以降の開発版）。"
                "それより古い開発版なら、図面を開いてメニュー「MCP ブリッジを表示…」を"
                "1 回実行してください（パレットが出ている間だけ受け付けます）。"
                "それでも見つからないときは、プラグイン名（環境変数 VW_MCP_PLUGIN。既定は"
                "min-nano_structureDev）か、スプールの場所（環境変数 VW_MCP_SPOOL）を"
                "確かめてください。"
            ),
        }
    result = {"running": True, "spool": bridge.dir}
    result.update(status)
    result["lock"] = bridge.lease.snapshot()
    tools = bridge.live_tools()
    if tools is not None:
        # 名前・説明・引数の形をそのまま示す（vw_call に渡す手掛かり）。
        result["tools"] = public_tools(tools)
        if notify is not None:
            bridge.announce_if_changed(tools, notify)
    return result


# --- Vectorworks を起動する（vw_launch）---------------------------------------
# **ブリッジが動いていなくても答えられる**道具のもう 1 つ。ブリッジの相手側（プラグイン）は
# Vectorworks が起動するまで動いていないので、起動はこちらの役割になる。


def launch_command():
    """Vectorworks を起動するコマンド（argv）と、人に見せる名前を返す。見つからなければ
    (None, 理由)。

    **起動の手段は OS の標準の方法にだけ頼る。** macOS は `open -a`（アプリ名でも .app の
    パスでも LaunchServices が解決する。既に動いていれば前面に出すだけで、2 つ目は起動
    しない）。Windows は既定のインストール先の実行ファイルを探す。VW_MCP_APP で明示できる
    ——そのときは macOS でも .app で終わらなければ実行ファイルとしてそのまま起動する
    （テスト用の代替プログラムもこの経路）。
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

    **Windows では 2 つ目を起動しない。** macOS の `open -a` と違い、実行ファイルを直接
    起動すると 2 つ目の Vectorworks が立ち上がりうる。ブリッジが見つからないのに動いて
    いるなら、足りないのはパレット（メニュー「MCP ブリッジを表示…」）のほうである。
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
    """起動して切り離す（このサーバが終わっても Vectorworks は残る）。"""
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

    notify はブリッジが受け付けるようになったときに呼ぶ（MCP の tools/list_changed を送る
    ——起動前の一覧は自前の道具だけなので、Claude に再取得させる）。
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
        bridge.tick()
        time.sleep(LAUNCH_POLL_SECONDS)

    return {
        "launched": True,
        "running": False,
        "app": label,
        "searched": bridge.candidates,
        "hint": (
            "Vectorworks は起動しましたが、待っている間にブリッジが受け付けませんでした。"
            "起動に時間がかかっているなら、少し待って vw_bridge_status で確かめてください。"
            "入っている開発版が M41 より古いと、図面を開いてメニュー"
            "「MCP ブリッジを表示…」を 1 回実行するまで受け付けません（パレットが"
            "出ている間だけ受け付ける）。"
        ),
    }, True


# --- 起動してから要求する（vw_run_test）---------------------------------------
# **実機テストは Vectorworks が起動していなくても要求できる**（M40）。ブリッジが動いて
# いなければこちらが Vectorworks を起動し、受け付けるようになってから同じ要求を出し直す。
# 起動するのは**要求された道具がこれのときだけ**。
# 理由: 1 周目から無人で回すため。図面を読むだけの道具のために Vectorworks を起動すると、
# ブリッジが停止した理由（パレットが閉じている等）を Claude が調べる前に覆い隠してしまう。
LAUNCH_ON_DEMAND_TOOLS = ("vw_run_test",)


def call_with_launch(bridge, name, args, notify):
    """ブリッジが動いていなければ Vectorworks を起動してから name を要求する。

    返すのは (応答, 起動の結果 or None)。起動してもブリッジが受け付けなければ BridgeDown を
    投げる（起動の結果の文言を載せて）。
    """
    try:
        return bridge.call(name, args), None
    except BridgeDown:
        if name not in LAUNCH_ON_DEMAND_TOOLS:
            raise
    launched, is_error = launch_vectorworks(bridge, {}, notify)
    if is_error:
        raise BridgeDown(
            "Vectorworks を起動してから %s を頼もうとしましたが、橋が架かりませんでした。\n%s"
            % (name, json.dumps(launched, ensure_ascii=False, indent=2))
        )
    # **待ち時間を再取得してから要求する。** 一度も接続したことが無いとキャッシュが無く、
    # 実機テストの 1 周（30 分まで）を既定の 30 秒で諦めてしまう。
    bridge.live_tools()
    return bridge.call(name, args), launched


# --- 再起動を確認する（vw_restart / vw_update の restarting）--------------------
# **再起動そのものはプラグインが要求する**（SDK の CloseAllFilesAndQuitVectorworks。開いて
# いる図面の保存確認は Vectorworks が通常どおり出す）。こちらは「一度停止して、また受け
# 付けるようになる」のを確認するだけ——プラグイン側が入れ替わる過程は、プラグイン側からは
# 確認できない。


def wait_for_restart(bridge, notify):
    """ブリッジが一度停止し、また受け付けるまで待つ。結果の dict を返す。"""
    deadline = time.time() + RESTART_DOWN_WAIT
    went_down = False
    while time.time() < deadline:
        if bridge.status() is None:
            went_down = True
            break
        bridge.tick()
        time.sleep(LAUNCH_POLL_SECONDS)
    if not went_down:
        return {
            "restarted": False,
            "hint": (
                "Vectorworks がまだ終了していません。未保存の図面があれば保存の確認が"
                "出ているはずなので、人に応えてもらってください（取り消すと再起動しません）。"
                "終わったかは vw_bridge_status で確かめられます。"
            ),
        }
    deadline = time.time() + DEFAULT_LAUNCH_WAIT
    while time.time() < deadline:
        status = bridge.status()
        if status is not None:
            notify()
            result = {"restarted": True, "spool": bridge.dir}
            result.update(status)
            return result
        bridge.tick()
        time.sleep(LAUNCH_POLL_SECONDS)
    return {
        "restarted": False,
        "hint": (
            "Vectorworks は終了しましたが、待っている間に橋が架かりませんでした。"
            "起動に時間がかかっているなら、少し待って vw_bridge_status で確かめてください。"
            "起動しなかったなら vw_launch で起動できます。"
        ),
    }


def mac_app_running(app):
    """macOS で、その名前のアプリが動いているか（分からなければ False）。"""
    try:
        done = subprocess.run(
            ["/usr/bin/osascript", "-e", 'application "%s" is running' % app],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return done.stdout.decode("utf-8", "replace").strip() == "true"


def restart_without_bridge(bridge, notify):
    """**ブリッジが動いていないときの再起動**（macOS だけ）。結果と、エラーかどうかを返す。

    OS の標準の方法で**通常どおり終了させてから**起動し直す（AppleScript の quit。開いて
    いる図面の保存確認は通常どおり出る）。強制終了はしない。
    理由: 殻まで変わったビルドを入れた直後は、新しい本体を古い殻が読めず（ABI の版が
    違う）、ブリッジが停止していることがある——そのときはプラグインに再起動を要求できない。
    """
    if sys.platform != "darwin" or os.environ.get("VW_MCP_APP", ""):
        return {
            "restarted": False,
            "error": (
                "ブリッジが受け付けていないので、プラグインに再起動を頼めません。"
                "Vectorworks を手で再起動してください（終了して起動し直すと、"
                "新しいビルドが読み込まれます）。"
            ),
            "searched": bridge.candidates,
        }, True
    app = DEFAULT_MAC_APP
    if not mac_app_running(app):
        result, is_error = launch_vectorworks(bridge, {}, notify)
        result["note"] = "Vectorworks は動いていなかったので、起動しました。"
        return result, is_error
    try:
        subprocess.run(
            ["/usr/bin/osascript", "-e", 'tell application "%s" to quit' % app],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        return {"restarted": False, "error": "終了を頼めませんでした: %s" % error}, True
    deadline = time.time() + DEFAULT_LAUNCH_WAIT
    while time.time() < deadline and mac_app_running(app):
        bridge.tick()
        time.sleep(LAUNCH_POLL_SECONDS)
    if mac_app_running(app):
        return {
            "restarted": False,
            "hint": (
                "Vectorworks がまだ終了していません。保存の確認が出ていれば、人に応えて"
                "もらってください。"
            ),
        }, True
    result, is_error = launch_vectorworks(bridge, {}, notify)
    result["restarted"] = not is_error
    return result, is_error


# --- 占有の状況・購読・解放（vw_lock_status / vw_lock_release。M42）---------------


def blocked_result(name, view):
    """他のセッションが占有しているので断るときの本文。"""
    holder = view.get("holder", {})
    hint = "別のセッション（%s）が Vectorworks を使用中なので、%s は送りませんでした。" % (
        holder.get("session"),
        name,
    )
    if holder.get("busy"):
        hint += "いま %s を %d 秒実行しています。" % (holder.get("busy"), holder.get("busy_seconds", 0))
    else:
        hint += "操作が無ければ %d 秒後に解放されます。" % holder.get("released_in_seconds", 0)
    hint += (
        "vw_lock_status に wait（秒）を渡すと、解放されるまで経過を受け取りながら待てます。"
        "相手のセッションの作業を人に確かめずに上書きしないでください。"
    )
    return {"blocked": True, "tool": name, "hint": hint, "lock": view}


def lock_wait_seconds(args):
    try:
        value = float(args.get("wait", 0) or 0)
    except (TypeError, ValueError):
        value = 0.0
    return max(0.0, min(value, LEASE_WAIT_MAX))


def lock_status(bridge, args, progress=None):
    """vw_lock_status の中身。

    wait があれば、解放される（until=change なら何か起きる）まで待ち、その間の出来事を返す。
    progress は出来事のたびに呼ぶ（MCP の progress 通知。要求に progressToken があるときだけ）。
    """
    lease = bridge.lease
    view = lease.snapshot()
    wait = lock_wait_seconds(args)
    until = args.get("until") or "free"
    if wait <= 0:
        return view
    start_seq = int(view.get("seq", 0))
    seen = start_seq
    observed = []
    deadline = time.time() + wait
    while True:
        done = view["state"] != "held" if until == "free" else bool(observed)
        if done or time.time() >= deadline:
            break
        time.sleep(LEASE_WAIT_POLL_SECONDS)
        events, view = lease.events_after(seen)
        for event in events:
            seen = max(seen, int(event.get("seq", 0)))
            shown = {
                "time": format_time(event.get("time")),
                "session": event.get("session"),
                "event": event.get("event"),
            }
            observed.append(shown)
            if progress is not None:
                progress(len(observed), "%s: %s" % (shown["session"], shown["event"]))
    result = dict(view)
    result["observed"] = observed
    result["waited_until"] = until
    if view["state"] == "held" and until == "free":
        result["timed_out"] = True
    return result


def end_test_session(bridge, timeout=None):
    """実機テストを片付ける（プラグインの vw_test_cleanup）。結果の dict を返す。

    pending が True なら「片付けられなかった（次に占有したセッションへ引き継ぐ）」。
    Vectorworks が居ない・1 周の最中（busy_until）・応答が無いときがそれに当たる。
    **プラグインが道具を知らない**（M42 より古い開発版）ときは引き継がない（何度試しても同じ）。
    """
    status = bridge.status()
    if status is None:
        return {"done": False, "pending": True,
                "message": "Vectorworks のブリッジが動いていないので、片付けは次に占有した"
                           "セッションへ引き継ぎました。"}
    busy_until = status.get("busy_until")
    if isinstance(busy_until, (int, float)) and busy_until > time.time():
        return {"done": False, "pending": True,
                "message": "Vectorworks が %s の最中なので、片付けは次に占有したセッションへ"
                           "引き継ぎました。" % status.get("busy", "別の処理")}
    try:
        if timeout is None:
            response = bridge.call(CLEANUP_TOOL, {})
        else:
            response = bridge.call(CLEANUP_TOOL, {}, timeout=timeout)
    except (BridgeDown, TimeoutError, OSError) as error:
        return {"done": False, "pending": True,
                "message": "片付けを頼めませんでした（%s）。次に占有したセッションへ"
                           "引き継ぎました。" % error}
    if not response.get("ok"):
        return {"done": False, "pending": False,
                "message": "Vectorworks 側で片付けられませんでした: %s"
                           % response.get("error", "(理由不明)")}
    result = response.get("result", {})
    if not isinstance(result, dict):
        result = {}
    return {"done": result.get("done") is True, "pending": False,
            "message": result.get("message", "")}


def release_with_cleanup(bridge, reason, timeout=None):
    """片付けてから自分の占有を解く。占有していなければ何もしない。結果の dict を返す。"""
    if not bridge.lease.holds():
        return {"released": False, "cleanup": None}
    bridge.lease.begin(CLEANUP_TOOL)
    cleanup = end_test_session(bridge, timeout)
    bridge.lease.end(CLEANUP_TOOL, "片付けた" if cleanup["done"] else "片付けきれなかった")
    released = bridge.lease.release(reason, cleanup_pending=cleanup["pending"])
    return {"released": released, "cleanup": cleanup}


def run_exclusive(bridge, name, action):
    """占有を取ってから action（content を返す）を実行する。他が占有していれば断る。

    前の持ち主の片付けを引き継いだら、action の前に片付ける（その結果も返す）。
    """
    try:
        view = bridge.lease.begin(name)
    except OSError as error:
        return text_content("占有の状態を書けませんでした: %s" % error, is_error=True)
    if view is not None:
        return text_content(
            json.dumps(blocked_result(name, view), ensure_ascii=False, indent=2), is_error=True
        )
    inherited_note = None
    if bridge.lease.inherited is not None:
        # **引き継いだ片付けは 1 度だけ試す。** Vectorworks が居なければ図面は開いておらず、
        # 残るのはファイルだけ（PR が閉じれば周の頭で片付く）。ここで再び引き継ぐと、
        # この操作（vw_run_test など）が作った自分の図面を次の操作の前に消してしまう。
        cleanup = end_test_session(bridge)
        inherited_note = "前の持ち主（%s）の実機テストの片付け: %s" % (
            bridge.lease.inherited, cleanup["message"])
        bridge.lease.note(inherited_note)
    outcome = "失敗"
    try:
        content = action()
        if inherited_note is not None:
            content = dict(content)
            content["content"] = list(content["content"]) + [
                {"type": "text", "text": inherited_note}]
        if not content.get("isError"):
            outcome = "成功"
        return content
    finally:
        try:
            bridge.lease.end(name, outcome)
        except OSError as error:
            log("占有の状態を書けませんでした: %s" % error)


# --- MCP（JSON-RPC over stdio）------------------------------------------------


def rpc_result(request_id, result):
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def rpc_error(request_id, code, message):
    return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}


def text_content(text, is_error=False):
    """MCP の tools/call が返す形。中身は JSON テキスト 1 つ。"""
    return {"content": [{"type": "text", "text": text}], "isError": is_error}


def send(message):
    """stdout へ JSON-RPC を 1 行書く（応答も通知も同じ経路を通す）。"""
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def notify_tools_changed():
    """道具の一覧が変わったと Claude へ知らせる（ブリッジが受け付けるようになり、本来の一覧を取得できるようになった）。"""
    send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})


def progress_sender(params):
    """要求に progressToken があれば、MCP の progress 通知を送る関数を返す。"""
    meta = params.get("_meta") if isinstance(params, dict) else None
    token = meta.get("progressToken") if isinstance(meta, dict) else None
    if token is None:
        return None

    def progress(count, message):
        send({
            "jsonrpc": "2.0",
            "method": "notifications/progress",
            "params": {"progressToken": token, "progress": count, "message": message},
        })

    return progress


def handle_tools_call(bridge, params):
    name = params.get("name", "")
    args = params.get("arguments") or {}

    if name == LOCK_STATUS_TOOL["name"]:
        result = lock_status(bridge, args, progress_sender(params))
        return text_content(json.dumps(result, ensure_ascii=False, indent=2))
    if name == LOCK_RELEASE_TOOL["name"]:
        result = release_with_cleanup(bridge, "vw_lock_release")
        result.update(bridge.lease.snapshot())
        return text_content(json.dumps(result, ensure_ascii=False, indent=2))
    if name == STATUS_TOOL["name"]:
        return text_content(
            json.dumps(
                bridge_status_result(bridge, notify_tools_changed), ensure_ascii=False, indent=2
            )
        )
    if name == LAUNCH_TOOL["name"]:
        def launch():
            result, is_error = launch_vectorworks(bridge, args, notify_tools_changed)
            return text_content(
                json.dumps(result, ensure_ascii=False, indent=2), is_error=is_error
            )

        return run_exclusive(bridge, name, launch)
    if name == CALL_TOOL["name"]:
        name = args.get("tool", "")
        args = args.get("arguments") or {}
        if not isinstance(name, str) or not name:
            return text_content("vw_call には tool（道具の名前）が要ります。", is_error=True)
        if any(name == tool["name"] for tool in LOCAL_TOOLS):
            # 自前の道具はそのまま自分で答える（vw_call の入れ子も防ぐ）。
            return handle_tools_call(
                bridge, {"name": name, "arguments": args, "_meta": params.get("_meta")}
            )

    # プラグイン側の道具は、占有してから送る（M42）。
    return run_exclusive(bridge, name, lambda: call_plugin_tool(bridge, name, args))


def call_plugin_tool(bridge, name, args):
    """プラグイン側の道具を呼ぶ（占有を取ったあと）。"""
    launched = None
    try:
        response, launched = call_with_launch(bridge, name, args, notify_tools_changed)
    except BridgeDown as error:
        if name == "vw_restart":
            # ブリッジが停止していても再起動だけは要求したい（殻まで変わった直後がそれ）。
            result, is_error = restart_without_bridge(bridge, notify_tools_changed)
            return text_content(
                json.dumps(result, ensure_ascii=False, indent=2), is_error=is_error
            )
        return text_content(str(error), is_error=True)
    except TimeoutError as error:
        return text_content(str(error), is_error=True)
    except OSError as error:
        return text_content("スプールへ書けませんでした: %s" % error, is_error=True)

    if not response.get("ok"):
        return text_content(
            "Vectorworks 側でエラーになりました: %s" % response.get("error", "(理由不明)"),
            is_error=True,
        )
    result = response.get("result", {})
    # **再起動を要求した・要求された**なら、ブリッジが再び受け付けるまで確認する（応答はプラグインが
    # 終了する前に書いたもの）。
    restarting = name == "vw_restart" or (
        name == "vw_update" and isinstance(result, dict) and result.get("restarting") is True
    )
    if restarting:
        after = wait_for_restart(bridge, notify_tools_changed)
        if isinstance(result, dict):
            result = dict(result)
            result["after_restart"] = after
        return text_content(
            json.dumps(result, ensure_ascii=False, indent=2),
            is_error=after.get("restarted") is not True,
        )
    if launched is not None and isinstance(result, dict):
        # 起動したことも伝える（Claude が「起動していなかった」ことを知れるように）。
        result = dict(result)
        result["launched"] = launched
    if not bridge.listed:
        # ブリッジに届いたのに、Claude にはプラグインの道具を 1 つも示していない（先に起動した
        # アプリの一覧が古い）。再取得を促す。
        tools = bridge.live_tools()
        if tools:
            bridge.announce_if_changed(tools, notify_tools_changed)
    return text_content(json.dumps(result, ensure_ascii=False, indent=2))


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
                # 返せないので、vw_launch でブリッジが受け付けるようになったら再取得してもらう。
                "capabilities": {"tools": {"listChanged": True}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            },
        )
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "ping":
        return rpc_result(request_id, {})
    if method == "tools/list":
        tools = bridge.tools()
        bridge.listed = [tool.get("name") for tool in tools if isinstance(tool, dict)]
        return rpc_result(request_id, {"tools": LOCAL_TOOLS + public_tools(tools)})
    if method == "tools/call":
        return rpc_result(request_id, handle_tools_call(bridge, params))
    if request_id is None:
        return None  # 知らない通知は応答せずに捨てる
    return rpc_error(request_id, -32601, "知らないメソッドです: %s" % method)


def main():
    bridge = Bridge(spool_candidates())
    log("探す場所: %s" % ", ".join(bridge.candidates))
    if bridge.status() is None:
        log("いまブリッジは動いていません（vw_launch で Vectorworks を起動できます）。")
    # **終了するときは占有を解く。** Claude Code はセッションを閉じるとこのサーバを SIGTERM で
    # 止めるので、それも通常の終了（finally）として扱う。解けずに終わっても、持ち主の
    # プロセスが消えたことを他のセッションが確かめて解く（Lease._lapse）。
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        serve(bridge)
    finally:
        try:
            release_with_cleanup(
                bridge, "セッションを終了した", min(SHUTDOWN_CLEANUP_TIMEOUT, call_timeout())
            )
        except OSError as error:
            log("占有を解けませんでした: %s" % error)


def serve(bridge):
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
        except Exception as error:  # サーバごと異常終了させない
            log("処理中の例外: %r" % error)
            reply = rpc_error(message.get("id"), -32603, "内部エラー: %s" % error)
        if reply is not None:
            send(reply)


if __name__ == "__main__":
    main()
