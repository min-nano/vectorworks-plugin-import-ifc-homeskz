#
#	scripts/vw-dump-pio-fields.py
#
#	【CI 用ではない】VectorWorks の「スクリプト編集（Python）」に貼って**実機で走らせる**調査用
#	スクリプト。選択した 1 オブジェクト（プラグインオブジェクト＝PIO を想定）の
#	  * パラメトリックレコードの全フィールド（番号・名前・型・値）
#	  * 付いているレコードフォーマットの全フィールド
#	  * （任意）オブジェクト変数の総なめ ── 既定値でないものだけ
#	  * 文書内の全ビューポート（名前・UUID・図面タイトル）
#	をテキストファイルへ書き出す。
#
#	【なぜ要るか】VW 標準 PIO の設定には**SDK に API が無いもの**がある。グラフィック凡例の
#	「ビューポートでフィルタ」がその 1 つで、2026 SDK のヘッダを全文検索しても、凡例に触れる
#	公開 API は `ovGraphicLegend*`（PIO のリセット中しか有効でない画像再生成用）と
#	条件ダイアログの文脈 `eGraphicLegendCriteria` しか無い（docs/DEV-NOTES.md
#	「グラフィック凡例」）。この種の設定は**UI で手作業をしたオブジェクトと、していない
#	オブジェクトのダンプを見比べる**以外に突き止めようが無い（`ModifySlab` の噛み合わせも
#	断面ビューポートの範囲も、同じやり方で決着した＝「打ち切った調査」）。
#
#	【使い方】
#	  1. VW で ツール > スクリプト > VectorScript 編集… に Python スクリプトとして貼る。
#	  2. 調べたいオブジェクトを**1 つだけ**選択して実行する。書き出し先がダイアログに出る。
#	  3. UI で設定を変える**前と後**の 2 回走らせ、2 つの出力の diff を取る
#	     （＝その設定がどこへ永続化されているかが分かる）。
#	  4. レコードに差が出なかったときは `DUMP_OBJECT_VARIABLES = True` にして取り直す
#	     （オブジェクト変数側に載っているかを同じ diff で確かめられる）。
#
#	【読み取りしかしない】このスクリプトは図面を一切変更しない。オブジェクト変数の総なめだけは
#	呼ぶ数が多いので既定で切ってある（DUMP_OBJECT_VARIABLES）。レコードのダンプで足りなければ
#	True にする。
#

import os
import time

import vs

# オブジェクト変数（1〜kObjectVariableMax）の総なめ。既定は False ──
# レコードのダンプで用が足りることが多く、総なめは 1 万回以上の呼び出しになるため。
DUMP_OBJECT_VARIABLES = False
OBJECT_VARIABLE_RANGE = range(1, 2200)


def _safe(call, *args):
	"""VW の API は引数が型に合わないと例外を投げることがある。調査スクリプトなので握り潰す。"""
	try:
		return call(*args)
	except Exception as error:  # noqa: BLE001 - 何が飛んでも調査は続ける
		return "<error: %s>" % error


def _dump_record(out, handle, record, title):
	"""レコード 1 つのフィールドを「番号 名前 (型) = 値」で書き出す。"""
	name = _safe(vs.GetName, record)
	count = _safe(vs.NumFields, record)
	out.append("")
	out.append("--- %s: %s (fields=%s) ---" % (title, name, count))
	if not isinstance(count, int):
		return
	for index in range(1, count + 1):
		field = _safe(vs.GetFldName, record, index)
		kind = _safe(vs.GetFldType, record, index)
		value = _safe(vs.GetRField, handle, name, field)
		out.append("%4d  %-44s (type=%s) = %s" % (index, field, kind, value))


def _dump_object_variables(out, handle):
	"""オブジェクト変数の総なめ。既定値（0 / False / 空文字 / NULL）は落とす。"""
	out.append("")
	out.append("--- object variables (non-default only) ---")
	getters = (
		("int", vs.GetObjectVariableInt),
		("bool", vs.GetObjectVariableBoolean),
		("real", vs.GetObjectVariableReal),
		("string", vs.GetObjectVariableString),
		("handle", vs.GetObjectVariableHandle),
	)
	for index in OBJECT_VARIABLE_RANGE:
		for kind, getter in getters:
			value = _safe(getter, handle, index)
			if value in (0, 0.0, False, None, "", "<error: ", "NULL"):
				continue
			if isinstance(value, str) and value.startswith("<error:"):
				continue
			if kind == "handle":
				value = "%s (name=%s type=%s)" % (
					value,
					_safe(vs.GetName, value),
					_safe(vs.GetTypeN, value),
				)
			out.append("%5d %-6s = %s" % (index, kind, value))


def _dump_viewports(out):
	"""文書内の全ビューポート。凡例側のダンプに出た文字列と突き合わせるための対応表。"""
	out.append("")
	out.append("--- viewports in this document ---")
	rows = []

	def collect(handle):
		rows.append(
			"name=%-24s uuid=%-40s type=%s"
			% (_safe(vs.GetName, handle), _safe(vs.GetObjectUUID, handle), _safe(vs.GetTypeN, handle))
		)

	_safe(vs.ForEachObject, collect, "(T=VIEWPORT)")
	out.extend(sorted(rows))


def main():
	handle = vs.FSActLayer()
	if handle is None:
		vs.AlrtDialog("オブジェクトを 1 つ選択してから実行してください。")
		return

	out = []
	out.append("vw-dump-pio-fields  %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
	out.append("selected: type=%s name=%s uuid=%s" % (
		_safe(vs.GetTypeN, handle),
		_safe(vs.GetName, handle),
		_safe(vs.GetObjectUUID, handle),
	))

	parametric = _safe(vs.GetParametricRecord, handle)
	if parametric:
		_dump_record(out, handle, parametric, "parametric record")
	else:
		out.append("")
		out.append("--- parametric record: なし（PIO ではない） ---")

	attached = _safe(vs.NumRecords, handle)
	if isinstance(attached, int):
		for index in range(1, attached + 1):
			record = _safe(vs.GetRecord, handle, index)
			if record:
				_dump_record(out, handle, record, "attached record #%d" % index)

	if DUMP_OBJECT_VARIABLES:
		_dump_object_variables(out, handle)

	_dump_viewports(out)

	path = os.path.join(
		os.path.expanduser("~/Desktop"),
		"vw-pio-dump-%s.txt" % time.strftime("%Y%m%d-%H%M%S"),
	)
	with open(path, "w", encoding="utf-8") as stream:
		stream.write("\n".join(out) + "\n")
	vs.AlrtDialog("書き出しました:\n%s" % path)


main()
