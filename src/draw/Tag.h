//
//	draw/Tag.h
//
//	Phase 2（VW 描画）の断面寸法データタグモジュール。Python 版 vw/sheet.py の draw_tag に
//	対応する（ROADMAP.md M13）。ビューポート命令が持つタグ（core::ViewportCommand::tags）を
//	**そのビューポートの注釈**としてデータタグ（PIO "Data Tag"）で置き、対象の横架材へ
//	関連付ける。
//
//	【伏図と軸組図で 1 つの実装】どちらのビューポートも「命令が持つ tags を注釈へ置く」だけで
//	済むのは、**どのタグがどの図に載るかを解析側（parse/Tag）が決めている**ため。Python 版は
//	平らな tag 命令をレイヤ名で振り分けていたが、断面（軸組図）はレイヤでは選べないので、
//	振り分けごと解析側へ寄せてある（parse/Tag.h 冒頭）。
//
//	【スタイルは作って使う（既存のスタイルに頼らない）】タグの中身（断面寸法の書式）も
//	見た目もデータタグスタイルが決めるので、**そのスタイルを取り込みのたびに文書へ作る**
//	（createTagStyle）。以前は「文書に用意された "断面寸法" スタイルを名前で引く」形だったが、
//	それだと**受け取った文書にスタイルが無ければタグが空で並ぶ**——テンプレートを配る運用が
//	要るうえ、失敗が図面の上でしか分からない。作るのは
//	  * プラグインオブジェクトスタイル＝**サブタイプにデータタグの内部 ID を持つシンボル定義**
//	    （VWFC の VWSymbolDefObj::HasPluginStyleSupport が GetSymbolDefSubType > 0 で判定して
//	    いるのがその実体。SetSymbolDefSubType で作れる）
//	  * その中に置くデータタグ PIO 1 つ（スタイルのパラメータの持ち主）
//	  * PIO のプロファイルグループ＝**タグレイアウト**。中身は「断面寸法」フィールド 1 つ
//	    （リンクされたテキスト＝IDataTagTextLinkSupport で式を持たせたテキスト）
//	**名前は既存の資源とぶつけない**（同名のスタイルを持つ文書へ取り込んでも、ユーザーの
//	スタイルを乗っ取らない）。基準名で空いていなければ "-2"、"-3" … と後ろを足す。
//
//	【SDK 型を公開するヘッダ】ビューポートのハンドルを引数に取るため、このヘッダは
//	draw/DrawUtil.h・draw/StructuralMember.h と同じく**SDK 型を公開する共通ヘッダ**で、
//	自分で PluginPrefix.h を（DrawUtil.h 経由で）取り込む。したがって**要素ごとの draw/*.h から
//	include してはならない**（あちらは SDK を持たない翻訳単位＝Extensions/ExtMenu からも
//	include されるため。DrawUtil.h 冒頭の約束）。呼び出し元は draw/Sheet.cpp と
//	draw/Section.cpp の 2 つだけ。
//
//	【データタグの作法（ローカル確認を重ねて定まった順）】
//	  CreateCustomObject("Data Tag", 挿入点, 角度) → **対象の横架材へ関連付け** →
//	  スタイル（skipValidation）→ 引出線を OFF → ResetObject → ビューポート注釈へ追加 →
//	  タグを更新 → 実位置と実寸を測る → （全部置いてから）**目標の位置へ動かす**
//	引出線を OFF にするのは、**タグを部材の面ちょうどに置いても既定 ON だと引出線が描かれる**
//	ため（位置の決め方は parse/Tag.h）。関連付け先が無い（構造材ツールで描けずフォールバックの
//	直線になった）横架材のタグは、関連付けだけを省いて置く（Python 版と同じ）。
//
//	【ローカル確認で分かった落とし穴（この順序と後処理の理由）】
//	  1. **関連付けより先にスタイルを当てると警告ダイアログが出る**（「互換性のないデータ
//	     タグスタイルを選択しています」）。タグ付け対象が無い状態では、スタイルが読もうと
//	     している PIO タイプ／レコードが「対象に存在しない」と判定されるため。関連付けを
//	     先に済ませ、さらに SetDataTagStyle へ skipValidation=true を渡して、無人の
//	     インポートがダイアログで止まらないようにする。
//	  2. **タグは指定した挿入点に留まらない。** そこで**最後に GetObjectBounds で実位置と
//	     実寸を測って動かす**（タグの実寸はスタイルが決めるので、部材から逃がす量も描くまで
//	     分からない）。ローカル確認で診断行へ実測を出して確かめた落ち方は次のとおり。
//	       * 伏図: 実位置は指示した点から**タグ幅の半分だけ −X へ**寄る（実測 3 件が
//	         「指示 (−7788, 4241) → 実測 (−7940, 4241)・幅 305」のように一致）。VW は
//	         挿入点をタグの**右辺中央**として扱っている。
//	       * 軸組図: **落ちる場所はビューポートごとにばらばら**で、指示との差に規則が無い。
//	     どちらも測って動かすこの後処理が最終位置を決めるので、VW がどこへ置くかに依らない。
//	     **「VW が関連付け先へ吸着させる」わけではない**——それを前提に「VW が置いた位置
//	     からの相対」で決める作りを試したが、実測はどの基準点候補からも数 m 外れた
//	     （parse/Tag.h「［遠回りの記録］」）。位置は必ず命令の position で決める。
//	  3. **注釈へ後から足した図形のクラスはビューポートで非表示のまま**。ビューポートの
//	     クラス表示を決める ConfigureViewport はタグを置く前に走るので、タグのスタイルが
//	     その時点で文書に無かったクラスを持ち込むと（タグの中身はスタイルが決める）
//	     そのクラスが非表示のまま残る。置いた後に ShowAllViewportClasses で全クラスを
//	     表示へ戻し、ビューポートを再更新する。
//	  4. **断面（軸組図）の注釈空間は横方向の原点がモデルと違う**（高さは合うのに横だけ
//	     一定量ずれた）。補正は解析側が持つ（parse/Tag の sectionAlongOrigin）。
//	     **ローカル確認で正しい位置に出ることを確認済み。**
//
//	実描画（タグの見え方・スタイルの効き・注釈空間での位置）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M13「ローカル確認」）。
//

#pragma once

#include "draw/DrawUtil.h"
#include "draw/TagStyle.h"

#include "core/Document.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// タグ描画の集計。**実描画はローカルの VW でしか確認できない**ので、タグが 1 つも
	// 出ないときに原因（スタイルが無い／タグを作れない／関連付け先が無い）を切り分けられる
	// ように件数で持ち帰る（draw/Member の診断と同じ流儀）。

	struct TagCounts
	{
		std::size_t drawn = 0;	// 注釈に置けたタグ
		std::size_t failed = 0; // PIO を作れなかった／注釈に入れられなかった
		std::size_t unassociated = 0; // 関連付け先の横架材ハンドルが無かった
		std::size_t leaderLeft = 0; // 引出線を OFF にできなかった（引出線が残る）
		std::size_t classesShown = 0; // タグを置いた後に表示へ戻せたクラス数（0 なら映らない）
		std::size_t updateFailed = 0; // クラスを戻した後の再更新に失敗したビューポート
		std::size_t unmeasured = 0; // 実位置を測れず動かせなかったタグ
		bool styleMissing = false; // 生成したデータタグスタイルが無い（スタイル無しで置いた）
	};

	// データタグ PIO の定義を**設定ダイアログを出さない**で用意する。タグを 1 つでも置く
	// フェーズ（伏図・軸組図）の先頭で 1 回呼ぶ。
	//
	// CreateCustomObject は、その名前の PIO が**その文書に**まだ定義されていなければ
	// DefineCustomObject で定義を作る。既定は kCustomObjectPrefAlways なので、最初の 1 個を
	// 作るときだけ「オブジェクトの設定」ダイアログが出てインポートが止まる（M12 の記号 PIO で
	// 実機確認済みの落とし穴。draw/ColumnMark.cpp）。**静的フラグで 1 回だけにはしない**
	// ——定義は文書ごとなので、次の文書へのインポートで抜けてしまう。
	void prepareDataTagPlugin();

	// 断面寸法データタグスタイルを**この取り込みのために文書へ作る**（下記「スタイルは
	// 作って使う」）。命令セットに載っているタグのスタイル名（core::TagCommand::style ＝
	// parse/Tag の kTagStyle）を**基準名**として、文書で空いている名前を選んで作る。
	// タグが 1 つも無い文書では何もしない（使わない資源を文書へ足さない）。
	//
	// 作れたかどうか・付いた名前は style（draw/TagStyle）に残り、tagStyleDiagnostics が
	// 1 行にする。**作れなくてもタグは置く**（スタイル無しで置く。位置だけでも正しい
	// タグが残る方が原因を追いやすい）。
	void createTagStyle(const core::Document& document, TagStyle& style);

	// ビューポート 1 枚ぶんのタグを注釈として置く。置けた数を返し、内訳を counts へ積む
	// （複数のビューポートぶんを 1 つの counts へ積んでよい）。memberHandles は drawMembers が
	// 記録した「命令インデックス → 横架材ハンドル」の対応表。style は createTagStyle が
	// 作ったスタイル（作れていなければスタイル無しで置く）。
	std::size_t drawViewportTags(MCObjectHandle viewport, const core::ViewportCommand& command,
								 const ObjectHandleTable& memberHandles, const TagStyle& style,
								 TagCounts& counts);

	// 集計を人が読める 1 行の診断にする（異常が無ければ空文字）。label は図の種別
	// （"伏図" / "軸組図"）。
	std::string tagDiagnostics(const std::string& label, const TagCounts& counts);
} // namespace HomeskzIfcImport::draw
