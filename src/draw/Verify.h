//
//	draw/Verify.h
//
//	**開発ビルドにしかコンパイルしないもの**のスイッチを置く唯一の場所。ここで定義する
//	`#if` で囲んだところは dev ビルド（`VW_DEV_BUILD`）にしか入らない。2 つある。
//
//	  `VW_DRAW_VERIFY` … **描いたものを読み戻して検算する**コード（下記）。
//	  `VW_DRAW_TIMING` … **描画のどこで時間を使ったかを区間ごとに測る**コード
//	                     （`VW_DRAW_TIME` と core/DrawTiming）。
//
//	**基準は 2 つとも同じ**——「外したら利用者の絵が変わるか」。変わらないなら囲む。
//	計測は図面に一切触らず、出てくる文言も開発者にしか意味が無いので囲む側である。
//
//	【なぜ分けるのか】実描画はローカルの VectorWorks でしか確認できない（CLAUDE.md
//	「テスト方針」）。そこで draw/ の各要素は、書いた値（ストーリバウンドの record・PIO の
//	パラメータ・PIO が持つパス）を**書いた直後に読み戻して命令と引き比べ**、食い違った
//	件数と 1 件目の実測を診断ログへ持ち帰るようにしてある。これは**実機フィードバックの
//	往復で絵の破綻を数字から手繰るための足場**であって（docs/DEV-NOTES.md M27「柱が
//	長さ 0 で描かれる」がまさにこれで解けた）、**利用者が本番ビルドで受け取るものではない**
//	——読み戻しはパラメータの走査を伴うので取り込みのたびに数百〜数千回走り、出てくる文言も
//	開発者にしか意味が無い。
//
//	【境界——何を囲み、何を囲まないか】囲むのは**読み戻した結果が診断にしかならないもの**
//	だけである。
//
//	  囲む   … 検算そのもの（描き上がった両端の絶対 Z と命令の引き比べ）・その件数・
//	           実測を文字列にする道具（`DescribeSizeParams` / `DescribeStoryBound` /
//	           `DescribePioPath` / `DescribeParamsContaining`）・取り込み後の測り直し
//	           （`recheckColumns`）・パスの観測（`PathProbe`）。
//	  囲まない … **読み戻した結果が絵を変えるもの**。`SetParamRealChecked`（実数で入らな
//	           ければ文字列で入れ直す）・`CreatePath` の `NurbsSetPt3D`（足した点を入れ直す）・
//	           潰れた材のパスを作り直す自己修復（`retryWithFreshPath`）・データタグの
//	           レイアウトの取り直し（draw/Tag）・シンボルが置けたことを確かめてから数える
//	           （draw/Symbol）。これらは検算ではなく**描画の一部**なので、本番でも走る。
//
//	新しく検算を足すときも同じ基準で分ける——**「外したら利用者の絵が変わるか」**が
//	囲むか囲まないかを決める。変わらないなら囲む。
//
//	【置き場所の約束】このヘッダは SDK 型を持たないので、draw/*.h からも draw/*.cpp からも
//	include してよい（draw/DrawUtil.h や draw/StructuralMember.h のような「SDK 型を公開する
//	ヘッダ」とは扱いが違う）。**本体（ペイロード）側のファイルなので、足しても殻は変わらない**
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
//	区間へ二重に積まれ、合計を読んだ人が必ず取り違える。ある処理をどの粒度で測るかは
//	1 か所で決めて、その内側では測らない。
//
//	**名前は文字列リテラルで渡す**（TimingScope は string_view で持つので、スコープより
//	短命なものを渡してはならない）。本番ビルドでは `((void)0)` へ畳まれる。
#define VW_DRAW_TIME_CAT_(a, b) a##b
#define VW_DRAW_TIME_CAT(a, b) VW_DRAW_TIME_CAT_(a, b)
#if VW_DRAW_TIMING
#	define VW_DRAW_TIME(section)                                                                  \
		const ::HomeskzIfcImport::core::TimingScope VW_DRAW_TIME_CAT(vwDrawTimingScope_,           \
																	 __LINE__)(section)
#else
#	define VW_DRAW_TIME(section) ((void)0)
#endif
