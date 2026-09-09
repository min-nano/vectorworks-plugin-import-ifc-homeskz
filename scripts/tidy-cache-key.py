#!/usr/bin/env python3
#
# tidy-cache-key.py — clang-tidy の結果キャッシュのキーを 1 翻訳単位ぶん出す。
#
# なぜこれがあるか
# ----------------
# PR の待ち時間を決めているのは**ビルドではなく clang-tidy** である（実測 2026-09、
# 41 翻訳単位: build-mac 1 分 50 秒 / build-windows 2 分 7 秒に対し、tidy-windows は
# 6 分 55 秒）。clang-tidy はビルドを走らせずに解析するので PCH を使えず、翻訳単位ごとに
# SDK のアンブレラヘッダを丸ごと解析し直す（CMakeLists.txt の VW_ENABLE_PCH）。1 本あたり
# 20〜40 秒がそのまま積み上がる。
#
# コンパイルの側は ccache が「入力が同じなら結果を使い回す」で解いている。clang-tidy には
# それが無い——**出力がオブジェクトではなく診断だから**で、ccache は面倒を見てくれない。
# そこで同じ原理を自前でやる: **翻訳単位の入力すべてを 1 つのハッシュにまとめ、それを鍵に
# 「この入力では診断が 1 つも出なかった」という事実だけをキャッシュする**。次の実行で同じ
# 鍵が出たら、その翻訳単位の解析は丸ごと省ける。
#
# 【鍵に入るもの】——ここに漏れがあると「直したのに緑」という最悪の事故になる。
#
#   1. **その .cpp が推移的に include する src/ 配下のファイル**の中身（下記の走査）
#   2. compile_commands.json に載っているその翻訳単位のコンパイル指令（＝フラグと定義。
#      チャンネルの VW_DEV_BUILD も VW_SHELL_ID もここに現れる）
#   3. clang-tidy の版（--tidy-version）
#   4. .clang-tidy の中身（規則そのもの。ソースの位置から根まで遡って全部）
#   5. SDK の同一性（--sdk-key。build.yml の VW_SDK_CACHE_KEY。SDK ヘッダは膨大なので
#      1 つずつハッシュせず、「どの SDK を取ってきたか」を決めているこの鍵で代表させる。
#      **SDK を差し替えるときは必ずこの鍵を上げる**という既存の約束（build.yml）が
#      そのままキャッシュの正しさを担保する）
#   6. 追加の clang-tidy 引数（--extra。Windows の -fdelayed-template-parsing 等）
#   7. この仕組み自体の版（SCHEME。作りを変えたら上げる＝全部を捨てる）
#
# 【include の走査は「多めに拾う」側へ倒す】条件コンパイル（#if）は評価せず、**行として
# 書かれている #include をすべて辿る**。実際には読まれない include まで依存に数えることは
# あるが、それは「本当は使い回せたのに解析し直した」だけで無害である。逆に取りこぼすと
# 「変わったのに使い回す」になり、こちらは診断の見落としに直結する。
#
# 【外にあるものは辿らない】追いかけるのは src/ 配下に解決できた include だけ。SDK や
# 標準ライブラリのヘッダはそこに解決できないので、上の 3・5 が代表する。CI では SDK が
# チェックアウトの中（<workspace>/vw-sdk）に置かれるため、「リポジトリの中か」ではなく
# 「src/ の中か」で線を引く必要がある。
#
# 【使い回せないと分かったら潔く諦める】compile_commands.json に載っていない・
# `#include SOME_MACRO` のように綴りが実行時にしか決まらない——こういう翻訳単位は
# 終了コード 3 で「キャッシュ不可」を返す。呼び出し側（scripts/clang-tidy-sdk.sh）は
# その 1 本を必ず解析する。**分からないときは必ず「解析する」へ倒す。**
#
# 使い方:
#   tidy-cache-key.py -p <compile-db-dir> [--root DIR] [--follow DIR]
#                     [--tidy-version STR] [--sdk-key STR] [--extra STR] <source>
#
# 終了コード: 0 = 鍵を標準出力へ / 2 = 使い方の誤り / 3 = キャッシュ不可（理由を stderr へ）

import argparse
import hashlib
import json
import os
import re
import sys

# キャッシュの作り方そのものの版。走査や鍵の組み立てを変えたら上げる（＝既存の
# キャッシュを一斉に無効にする）。
SCHEME = "tidy-cache-v1"

# 行として書かれた #include の綴りを取り出す。"..." と <...> の両方。
INCLUDE_RE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*(?:"([^"\n]*)"|<([^>\n]*)>)')

# 綴りがマクロで決まる #include。中身を追えないので、その翻訳単位はキャッシュ不可にする。
COMPUTED_RE = re.compile(rb"^[ \t]*#[ \t]*include[ \t]+[A-Za-z_]")


class Uncacheable(Exception):
    """この翻訳単位は使い回せない（呼び出し側は必ず解析する）。"""


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def resolve_include(spec, includer_dir, search_dirs, quoted):
    """include の綴りを実ファイルへ解決する。見つからなければ None。

    "..." は includer のディレクトリを先に見る（コンパイラと同じ順序）。<...> は見ない。
    見つからないものは「src/ の外にあるもの」＝ SDK か標準ライブラリなので、鍵の側では
    --tidy-version / --sdk-key が代表する。
    """
    candidates = []
    if quoted:
        candidates.append(os.path.join(includer_dir, spec))
    candidates.extend(os.path.join(d, spec) for d in search_dirs)
    for candidate in candidates:
        if os.path.isfile(candidate):
            return os.path.realpath(candidate)
    return None


def is_under(path, directory):
    try:
        return os.path.commonpath([path, directory]) == directory
    except ValueError:  # 別ドライブ（Windows）
        return False


def include_closure(source, follow_dirs, search_dirs):
    """source から辿れる follow_dirs 配下のファイルを、重複なく集める（source を含む）。

    #if は評価しない——書かれている include をすべて辿る（多めに拾う側へ倒す）。
    """
    seen = set()
    pending = [source]
    found = []
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        found.append(current)
        current_dir = os.path.dirname(current)
        for line in read_bytes(current).splitlines():
            if b"#" not in line:
                continue
            match = INCLUDE_RE.match(line)
            if not match:
                if COMPUTED_RE.match(line):
                    raise Uncacheable(
                        "綴りがマクロで決まる #include があります: "
                        f"{current}: {line.decode('utf-8', 'replace').strip()}"
                    )
                continue
            quoted = match.group(1) is not None
            raw = match.group(1) if quoted else match.group(2)
            spec = raw.decode("utf-8", "surrogateescape")
            resolved = resolve_include(spec, current_dir, search_dirs, quoted)
            if resolved and any(is_under(resolved, d) for d in follow_dirs):
                pending.append(resolved)
    return sorted(found)


def load_entries(db_dir, source):
    """compile_commands.json から、その source のコンパイル指令を（複数あれば全部）拾う。

    殻と本体は別ターゲットなので、同じ .cpp が 2 つの指令を持ちうる。clang-tidy は
    最初の 1 つで解析するが、鍵には**全部**を入れる（どちらが変わっても解析し直す）。
    """
    path = os.path.join(db_dir, "compile_commands.json")
    if not os.path.isfile(path):
        raise Uncacheable(f"compile_commands.json がありません: {path}")
    with open(path, "r", encoding="utf-8") as handle:
        database = json.load(handle)
    wanted = os.path.realpath(source)
    entries = []
    for entry in database:
        entry_file = entry.get("file", "")
        if not os.path.isabs(entry_file):
            entry_file = os.path.join(entry.get("directory", ""), entry_file)
        if os.path.realpath(entry_file) == wanted:
            command = entry.get("command")
            if command is None:
                command = " ".join(entry.get("arguments", []))
            entries.append(command)
    if not entries:
        raise Uncacheable(f"compile_commands.json に載っていません: {source}")
    return sorted(entries)


def normalize_paths(text, root):
    """絶対パスを @ROOT@ へ畳む。

    CI のワークスペース（/Users/runner/work/... や D:\\a\\...）はローカルと綴りが違うし、
    SDK も build-tidy もその下にあるので、根さえ畳めば指令は環境をまたいで同じ文字列に
    なる。Windows は大文字小文字を区別しない。
    """
    variants = {root, root.replace("\\", "/"), root.replace("/", "\\")}
    for variant in sorted(variants, key=len, reverse=True):
        if not variant:
            continue
        flags = re.IGNORECASE if os.name == "nt" else 0
        text = re.sub(re.escape(variant), "@ROOT@", text, flags=flags)
    return text


def tidy_configs(source, root):
    """source の位置から root まで遡って見つかる .clang-tidy を、近い順に返す。"""
    found = []
    current = os.path.dirname(os.path.realpath(source))
    root = os.path.realpath(root)
    while True:
        candidate = os.path.join(current, ".clang-tidy")
        if os.path.isfile(candidate):
            found.append(candidate)
        if current == root or not is_under(current, root):
            break
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    return found


def main(argv):
    parser = argparse.ArgumentParser(add_help=True, description=__doc__)
    parser.add_argument("-p", dest="db", required=True, help="compile_commands.json のある場所")
    parser.add_argument("--root", default=None, help="リポジトリの根（既定: このスクリプトの ..）")
    parser.add_argument(
        "--follow",
        action="append",
        default=None,
        help="include を辿る範囲（root からの相対。既定: src）",
    )
    parser.add_argument("--tidy-version", default="", help="clang-tidy --version の出力")
    parser.add_argument("--sdk-key", default="", help="SDK の同一性（VW_SDK_CACHE_KEY）")
    parser.add_argument("--extra", default="", help="clang-tidy への追加引数（そのまま）")
    parser.add_argument("source", help="解析する翻訳単位")
    args = parser.parse_args(argv)

    root = os.path.realpath(args.root or os.path.join(os.path.dirname(__file__), ".."))
    source = os.path.realpath(args.source)
    if not os.path.isfile(source):
        print(f"tidy-cache-key.py: ソースがありません: {args.source}", file=sys.stderr)
        return 3

    follow = [os.path.realpath(os.path.join(root, d)) for d in (args.follow or ["src"])]
    # include の検索パスは「src/」1 つでよい。CMakeLists.txt が渡す残りの -I は
    # すべて SDK 側で、そちらは辿らないからである（冒頭「外にあるものは辿らない」）。
    search_dirs = list(follow)

    try:
        entries = load_entries(args.db, source)
        deps = include_closure(source, follow, search_dirs)
    except Uncacheable as reason:
        print(f"tidy-cache-key.py: キャッシュ不可: {reason}", file=sys.stderr)
        return 3

    digest = hashlib.sha256()

    def feed(label, value):
        digest.update(label.encode("utf-8"))
        digest.update(b"\0")
        digest.update(value if isinstance(value, bytes) else value.encode("utf-8"))
        digest.update(b"\n")

    feed("scheme", SCHEME)
    feed("tidy-version", args.tidy_version)
    feed("sdk-key", args.sdk_key)
    feed("extra", args.extra)
    for config in tidy_configs(source, root):
        feed("config:" + os.path.relpath(config, root).replace("\\", "/"),
             hashlib.sha256(read_bytes(config)).hexdigest())
    for command in entries:
        feed("command", normalize_paths(command, root))
    for dep in deps:
        feed("dep:" + os.path.relpath(dep, root).replace("\\", "/"),
             hashlib.sha256(read_bytes(dep)).hexdigest())

    print(digest.hexdigest())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
