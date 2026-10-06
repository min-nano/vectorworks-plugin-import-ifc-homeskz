//
//	draw/Verify.h
//
//	**開発ビルドにしかコンパイルしないもの**のスイッチを置く唯一の場所。ここで定義する
//	`#if` で囲んだところは dev ビルド（`VW_DEV_BUILD`）にしか入らない。2 つある。
//
//	  `VW_DRAW_VERIFY` … **描画したものを読み戻して検算する**コード（下記）。
//	  `VW_DRAW_TIMING` … **描画のどこで時間を使ったかを区間ごとに測る**コード
//	                     （`VW_DRAW_TIME` と core/DrawTiming）。
//
//	**基準は 2 つとも同じ**——「除外したら利用者の描画結果が変わるか」。変わらないなら囲む。
//	計測は図面を一切変更せず、出力される文言も開発者にしか意味が無いので囲む側である。
//
//	【なぜ分けるのか】実描画はローカルの VectorWorks でしか確認できない（CLAUDE.md
//	「テスト方針」）。そこで draw/ の各要素は、書いた値（ストーリバウンドの record・PIO の
//	パラメータ・PIO が持つパス）を**書いた直後に読み戻して命令と照合し**、食い違った
//	件数と 1 件目の実測を診断ログへ記録するようにしてある。これは**実機テストで描画結果の
//	破綻を数値から追跡するための補助の仕組み**であって（docs/DEV-NOTES.md M27「柱が
//	長さ 0 で描かれる」がまさにこれで解けた）、**利用者が本番ビルドで受け取るものではない**
//	——読み戻しはパラメータの走査を伴うので取り込みのたびに数百〜数千回実行され、出力される文言も
//	開発者にしか意味が無い。
//
//	【境界——何を囲み、何を囲まないか】囲むのは**読み戻した結果が診断にしかならないもの**
//	だけである。
//
//	  囲む   … 検算そのもの（描画結果の両端の絶対 Z と命令の照合）・その件数・
//	           実測を文字列にする道具（`DescribeSizeParams` / `DescribeStoryBound` /
//	           `DescribePioPath` / `DescribeParamsContaining`）・取り込み後の再測定
//	           （`recheckColumns` / `recheckShearWalls`）・パスの観測（`PathProbe`）・
//	           描画結果の実体の再測定（`MeasureDrawnMember` / `PioPathChord`）。
//	  囲まない … **読み戻した結果が描画結果を変えるもの**。`SetParamRealChecked`（実数で入らな
//	           ければ文字列で入れ直す）・`CreatePath` の `NurbsSetPt3D`（追加した点を入れ直す）・
//	           データタグのレイアウトの再取得（draw/Tag）・シンボルが置けたことを確認して
//	           から数える（draw/Symbol）。これらは検算ではなく**描画の一部**なので、本番でも
//	           実行される。
//
//	【「描画結果を変えるから本番に残す」を疑う】かつてここには**長さ 0 に退化した材のパスを
//	再生成する自己修復**（`retryWithFreshPath`）が並んでいた。
//	「削除すると退化した材がそのまま残る＝描画結果が変わる」という理屈だったが、
//	**修復した結果のほうが悪かった**——差し替えると `ResetObject` がバウンドの `fOffset`
//	を書き換え、利用者が階高を編集した瞬間に長さとして表に出る（撤去の経緯は
//	docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。
//	**「描画結果を変える」は「描画結果を良くする」ではない**ので、囲まない側へ置くときはその変え方が
//	正しいかまで確認する。
//
//	新しく検算を追加するときも同じ基準で分ける——**「除外したら利用者の描画結果が変わるか」**が
//	囲むか囲まないかを決める。変わらないなら囲む。
//
//	【置き場所の約束】このヘッダは SDK 型を持たないので、draw/*.h からも draw/*.cpp からも
//	include してよい（draw/DrawUtil.h や draw/StructuralMember.h のような「SDK 型を公開する
//	ヘッダ」とは扱いが違う）。**本体（ペイロード）側のファイルなので、追加しても殻は変わらない**
//	＝利用者は再起動せずに受け取れる（CLAUDE.md「アーキテクチャ: 殻と本体」）。
//

#pragma once

#include "core/DrawTiming.h"

#ifdef VW_DEV_BUILD
#	define VW_DRAW_VERIFY 1
#	define VW_DRAW_TIMING 1
#else
#	define VW_DRAW_VERIFY 0
#	define VW_DRAW_TIMING 0
#endif

//	区間を 1 つ測る。宣言した行から**その波括弧の終わりまで**が 1 区間で、名前ごとに
//	累計される（集計先は core::drawTiming()。整形と報告は draw/ExecuteDocument）。
//
//	    {
//	        VW_DRAW_TIME("構造材:リセット");
//	        gSDK->ResetObject(object);
//	    }
//
//	**区間を入れ子にしない**（core/DrawTiming.h「使う側の作法」）——同じ時間が 2 つの
//	区間へ二重に加算され、合計を読んだ人が必ず取り違える。ある処理をどの粒度で測るかは
//	1 か所で決めて、その内側では測らない。**共有の関数を区間の中から呼ぶときは、その
//	関数が自身でも区間を開いていないかを確認する**——見落としやすいので、計測を開かない版を
//	用意してそちらを呼ぶ（かつて draw/StructuralMember がそうしていた。長さ 0 の材の自己修復
//	だけが区間の中から呼んでいたので、それを撤去したときに分ける理由も無くなった）。
//	それでも見落としたときは、**入れ子になったこと自体が報告の末尾に警告として出力される**
//	（core/DrawTiming.h「入れ子の見張り」）。
//
//	**名前は文字列リテラルで渡す**（TimingScope は string_view で持つので、スコープより
//	短命なものを渡してはならない）。本番ビルドでは `((void)0)` に展開される。
#define VW_DRAW_TIME_CAT_(a, b) a##b
#define VW_DRAW_TIME_CAT(a, b) VW_DRAW_TIME_CAT_(a, b)
#if VW_DRAW_TIMING
#	define VW_DRAW_TIME(section)                                                                  \
		const ::HomeskzIfcImport::core::TimingScope VW_DRAW_TIME_CAT(vwDrawTimingScope_,           \
																	 __LINE__)(section)
#else
#	define VW_DRAW_TIME(section) ((void)0)
#endif
