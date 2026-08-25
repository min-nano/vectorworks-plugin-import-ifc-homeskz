#
#	scripts/vw-dump-pio-fields.py
#
#	【CI 用ではない】VectorWorks の「スクリプト編集（Python）」に貼って**実機で走らせる**調査用
#	スクリプト。選択した 1 オブジェクト（プラグインオブジェクト＝PIO を想定）について
#	  * パラメトリックレコードの全フィールド（番号・名前・型・値）
#	  * 付いているレコードフォーマットの全フィールド
#	  * **オブジェクト間の関連（association）** ── 「消したら一緒に消える／リセットされる」links
#	  * **PIO の中に生成されている図形**（型・名前・UUID・レコード名・UUID らしき値）
#	  * （任意）オブジェクト変数の総なめ ── 既定値でないものだけ
#	をテキストへ書き出し、あわせて**文書内のグラフィック凡例と全ビューポートの一覧**を出す。
#
#	【なぜ要るか】VW 標準 PIO の設定には**SDK にも VectorScript にも API が無いもの**がある。
#	グラフィック凡例の「ビューポートでフィルタ」がその 1 つで、公開されているのは
#	`ovGraphicLegend*`（PIO のリセット中しか有効でない画像再生成用）だけ
#	（docs/DEV-NOTES.md「グラフィック凡例」）。この種の設定は**UI で手作業をしたオブジェクトと、
#	していないオブジェクトのダンプを見比べる**以外に突き止めようが無い（`ModifySlab` の
#	噛み合わせも断面ビューポートの範囲も、同じやり方で決着した＝「打ち切った調査」）。
#
#	【分かっていること】「ビューポートでフィルタ」は**パラメトリックレコードには載らない**
#	（実機のダンプで確認済み。フィルタの前後で 36 フィールド中 `BoxWidth` しか変わらない＝
#	それは凡例が細くなった結果にすぎない）。したがって次に見るのは関連（association）と
#	PIO の中身で、それがこの版で増えた 2 節。
#
#	【使い方】
#	  1. VW で ツール > スクリプト > VectorScript 編集… に Python スクリプトとして貼る。
#	  2. 調べたいオブジェクトを**1 つだけ**選択して実行する。書き出し先がダイアログに出る。
#	     **1 枚だけ手で「ビューポートでフィルタ」した状態**にしておくと、同じ文書の中に
#	     「フィルタ済みの凡例」と「していない凡例」が並ぶので、**1 回の実行で見比べられる**
#	     （凡例の一覧は文書内の全グラフィック凡例を出す）。
#	  3. それでも差が出なければ `DUMP_OBJECT_VARIABLES = True` にして取り直す。
#
#	【読み取りしかしない】このスクリプトは図面を一切変更しない。
#

import os
import re
import time

import vs

# オブジェクト変数（1〜）の総なめ。既定は False ── 呼び出しが 1 万回を超えるので、
# レコード・関連・中身で足りないと分かってから True にする。
DUMP_OBJECT_VARIABLES = False
OBJECT_VARIABLE_RANGE = range(1, 2200)

# PIO の中身をたどる深さと、書き出す行数の上限（凡例は中に画像・枠・文字を大量に持つ）。
CHILD_MAX_DEPTH = 3
CHILD_MAX_LINES = 300

# 「どこかに紛れ込んだビューポートの UUID」を拾うための形。フィルタ先が UUID で
# 持たれているなら、この形の文字列がどこかに現れるはず。
UUID_PATTERN = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")


def _safe(name, *args):
	"""`vs` の関数を**名前で引いて**呼ぶ。戻り値はそのまま、駄目なら "<error: …>" を返す。

	名前で引くのは、**VW のバージョンによって在ったり無かったりする関数がある**ため
	（`vs.GetObjectUuid` は 2018.4 以降）。`_safe(vs.Foo, …)` と書くと、呼ぶ前の属性参照の
	時点で AttributeError になってスクリプトごと止まる——調査の道具がそれで死んでは困る。
	"""
	call = getattr(vs, name, None)
	if call is None:
		return "<missing: vs.%s>" % name
	try:
		return call(*args)
	except Exception as error:  # noqa: BLE001 - 何が飛んでも調査は続ける
		return "<error: %s>" % error


def _bad(value):
	"""_safe が返した「呼べなかった」印か。"""
	return isinstance(value, str) and (value.startswith("<error:") or value.startswith("<missing:"))


def _describe(handle):
	"""1 行の素性書き。ハンドルそのものは実行ごとに変わるので出さない（UUID で突き合わせる）。"""
	return "type=%s name=%s uuid=%s" % (
		_safe("GetTypeN", handle),
		_safe("GetName", handle),
		_safe("GetObjectUuid", handle),
	)


def _record_name(handle):
	"""PIO の登録名（＝パラメトリックレコードの名前）。PIO でなければ空。"""
	record = _safe("GetParametricRecord", handle)
	if not record or _bad(record):
		return ""
	name = _safe("GetName", record)
	return "" if _bad(name) else name


def _dump_record(out, handle, record, title):
	"""レコード 1 つのフィールドを「番号 名前 (型) = 値」で書き出す。"""
	name = _safe("GetName", record)
	count = _safe("NumFields", record)
	out.append("")
	out.append("--- %s: %s (fields=%s) ---" % (title, name, count))
	if not isinstance(count, int):
		return
	for index in range(1, count + 1):
		field = _safe("GetFldName", record, index)
		kind = _safe("GetFldType", record, index)
		value = _safe("GetRField", handle, name, field)
		out.append("%4d  %-44s (type=%s) = %s" % (index, field, kind, value))


def _association_lines(handle):
	"""オブジェクトに張られた関連を 1 行ずつ。索引の起点が 0 か 1 か不明なので両方なめる。"""
	lines = []
	count = _safe("GetNumAssociations", handle)
	lines.append("associations=%s" % count)
	if not isinstance(count, int) or count == 0:
		return lines
	for index in range(0, count + 1):
		info = _safe("GetAssociation", handle, index)
		if _bad(info) or info is None:
			lines.append("  [%d] %s" % (index, info))
			continue
		# Python 版は (HANDLE, associationkind, value) を返す。
		try:
			target, kind, value = info
		except (TypeError, ValueError):
			lines.append("  [%d] %s" % (index, info))
			continue
		lines.append("  [%d] kind=%s value=%s -> %s" % (index, kind, value, _describe(target)))
	return lines


def _dump_associations(out, handle, title):
	out.append("")
	out.append("--- %s ---" % title)
	out.extend(_association_lines(handle))


def _dump_children(out, handle):
	"""PIO が中に生成している図形をたどる。UUID らしき値を持つレコード欄はその場で出す。"""
	out.append("")
	out.append("--- objects inside the selected object (depth<=%d) ---" % CHILD_MAX_DEPTH)
	budget = [CHILD_MAX_LINES]

	def walk(parent, depth):
		if depth > CHILD_MAX_DEPTH or budget[0] <= 0:
			return
		child = _safe("FInGroup", parent)
		while child and not _bad(child) and budget[0] > 0:
			budget[0] -= 1
			pad = "  " * depth
			out.append("%s%s record=%s" % (pad, _describe(child), _record_name(child)))
			record = _safe("GetParametricRecord", child)
			if record and not _bad(record):
				name = _safe("GetName", record)
				count = _safe("NumFields", record)
				if isinstance(count, int):
					for index in range(1, count + 1):
						field = _safe("GetFldName", record, index)
						value = _safe("GetRField", child, name, field)
						if isinstance(value, str) and UUID_PATTERN.search(value):
							out.append("%s  * %s = %s" % (pad, field, value))
			walk(child, depth + 1)
			child = _safe("NextObj", child)

	walk(handle, 0)
	if budget[0] <= 0:
		out.append("… (%d 行で打ち切り)" % CHILD_MAX_LINES)


def _layer_name(handle):
	layer = _safe("GetLayer", handle)
	if not layer or _bad(layer):
		return ""
	name = _safe("GetLName", layer)
	return "" if _bad(name) else name


def _dump_legends(out, selected):
	"""文書内の全グラフィック凡例。**フィルタ済みの 1 枚と、していない残り**が並ぶので、
	1 回の実行でそのまま見比べられる。"""
	out.append("")
	out.append("--- graphic legends in this document ---")
	rows = []

	def collect(handle):
		if _record_name(handle) != "GraphicLegend":
			return
		mark = " <== selected" if handle == selected else ""
		head = "%s layer=%s width=%s%s" % (
			_describe(handle),
			_layer_name(handle),
			_safe("GetRField", handle, "GraphicLegend", "BoxWidth"),
			mark,
		)
		rows.append("\n".join([head] + ["  " + line for line in _association_lines(handle)]))

	_safe("ForEachObject", collect, "(T=PLUGINOBJECT)")
	out.extend(sorted(rows))


def _dump_viewports(out):
	"""文書内の全ビューポート。凡例側のダンプに出た文字列と突き合わせるための対応表。"""
	out.append("")
	out.append("--- viewports in this document ---")
	rows = []

	def collect(handle):
		rows.append(
			"name=%-24s uuid=%-40s %s"
			% (_safe("GetName", handle), _safe("GetObjectUuid", handle), _association_lines(handle)[0])
		)

	_safe("ForEachObject", collect, "(T=VIEWPORT)")
	out.extend(sorted(rows))


def _dump_object_variables(out, handle):
	"""オブジェクト変数の総なめ。既定値（0 / False / 空文字 / NULL）は落とす。"""
	out.append("")
	out.append("--- object variables (non-default only) ---")
	getters = (
		("int", "GetObjectVariableInt"),
		("bool", "GetObjectVariableBoolean"),
		("real", "GetObjectVariableReal"),
		("string", "GetObjectVariableString"),
		("handle", "GetObjectVariableHandle"),
	)
	for index in OBJECT_VARIABLE_RANGE:
		for kind, getter in getters:
			value = _safe(getter, handle, index)
			if value in (0, 0.0, False, None, "", "NULL") or _bad(value):
				continue
			if kind == "handle":
				value = "%s (%s)" % (value, _describe(value))
			out.append("%5d %-6s = %s" % (index, kind, value))


def main():
	handle = vs.FSActLayer()
	if handle is None:
		vs.AlrtDialog("オブジェクトを 1 つ選択してから実行してください。")
		return

	out = []
	out.append("vw-dump-pio-fields  %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
	out.append("selected: %s" % _describe(handle))

	parametric = _safe("GetParametricRecord", handle)
	if parametric and not _bad(parametric):
		_dump_record(out, handle, parametric, "parametric record")
	else:
		out.append("")
		out.append("--- parametric record: なし（PIO ではない） ---")

	attached = _safe("NumRecords", handle)
	if isinstance(attached, int):
		for index in range(1, attached + 1):
			record = _safe("GetRecord", handle, index)
			if record and not _bad(record):
				_dump_record(out, handle, record, "attached record #%d" % index)

	_dump_associations(out, handle, "associations of the selected object")
	_dump_children(out, handle)

	if DUMP_OBJECT_VARIABLES:
		_dump_object_variables(out, handle)

	_dump_legends(out, handle)
	_dump_viewports(out)

	path = os.path.join(
		os.path.expanduser("~/Desktop"),
		"vw-pio-dump-%s.txt" % time.strftime("%Y%m%d-%H%M%S"),
	)
	with open(path, "w", encoding="utf-8") as stream:
		stream.write("\n".join(out) + "\n")
	vs.AlrtDialog("書き出しました:\n%s" % path)


main()
