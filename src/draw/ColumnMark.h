//
//	draw/ColumnMark.h
//
//	Phase 2（VW 描画）の断面記号・伏図記号モジュール。Python 版 vw/column_mark.py に
//	対応する（ROADMAP.md M12）。ただし**描き方はまったく違う**——Python 版は姉妹
//	プロジェクトのカスタム PIO「柱束伏図記号」を置き、PIO 自身にリセット時の検索・作図を
//	させていた。本移植は PIO を使わず、
//	  * **断面記号**（drawColumnSectionMarks）… 素の直線（×／／）
//	  * **伏図記号**（drawColumnPlanMarks）… VW 本体同梱のデータタグ
//	だけで描く。理由は「カスタム PIO を導入していないマシンで図面の表示が崩れる」ことを
//	避けるため（parse/ColumnMark.h の「Python 版との差異」参照）。
//
//	【追随するのは伏図記号だけ】VW には素の図形を他の図形へ追随させる仕組みが無いので、
//	断面記号（直線）は静的なジオメトリになる。伏図記号は**データタグを柱へ関連付ける**
//	ことで、柱を動かせばタグも付いていく。関連付けには IDataTagSupport（グローバル
//	アクセサ gDataTagSupport）を使い、関連付け先の柱ハンドルは drawColumns が記録した
//	対応表（draw/ObjectHandles.h）から命令インデックスで引く。
//
//	【スタイルは作らない】伏図記号の絵（柱伏図記号／束伏図記号のシンボル）は**データタグ
//	スタイル**に焼き込まれている前提で、プラグインはスタイルを作らず名前で関連付けるだけ
//	（構造材の "木質構造材_横架材"・シンボル定義と同じ作法。draw/Symbol.cpp「設計上の
//	要点」）。スタイルが図面に無ければ件数を診断行に出し、「命令はあるのに記号が出ない」
//	原因がリソース側だと分かるようにする。
//
//	【レイヤの扱いが 2 つで違う】
//	  * 断面記号 … 柱と同じ span レイヤへ重ねる。ストーリが作るレイヤなので**無ければ
//	    スキップ**（ActivateExistingLayer の規約）。
//	  * 伏図記号 … "{to}-柱伏図記号" はストーリに属さない独立レイヤで、story 命令が作らない。
//	    通り芯の "共通" と同じく**無ければ作る**（PrepareLayer）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"
#include "draw/ObjectHandles.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 断面記号（columnSectionMark 命令）を直線で描く。配置先レイヤ（柱の span レイヤ）が
	// 無い命令はスキップする。**記号 1 つ＝柱 1 本**を 1 件として数え、実際に線を引けた
	// 記号の数を返す（× の 2 本は 1 件）。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**。
	std::size_t drawColumnSectionMarks(const core::Document& document,
									   core::ProgressReporter& progress,
									   std::string* outNote = nullptr);

	// 伏図記号（columnPlanMark 命令）をデータタグで置き、柱へ関連付ける。配置できた
	// タグの数を返す。columns は drawColumns が記録した柱ハンドルの対応表で、
	// **関連付け先が未配置の命令はタグを置かない**——どの柱にも付かないタグは、柱を
	// 動かしても追随せず、記号だけが取り残される（それなら最初から出さないほうがよい）。
	//
	// outNote には「タグスタイルが図面に無い」「関連付け先の柱が未配置」「タグを作れない
	// （VW のデータタグが使えない）」を件数で残す（完了ダイアログの診断。draw/Symbol と
	// 同じ流儀）。
	std::size_t drawColumnPlanMarks(const core::Document& document,
									core::ProgressReporter& progress, const ObjectHandles& columns,
									std::string* outNote = nullptr);

	// 伏図記号レイヤ名を**重複を除いて命令の出現順**で返す。draw/Sheet がビューポートの
	// レイヤ重ね順（core::desiredStoryLayerOrder の topLayers）へ、ストーリに属さない
	// 独立レイヤとして差し込むのに使う。
	//
	// 並びは「どれを先に積むか」だけを決め、**伏図に映るのは常に 1 枚**（切断の直下の
	// レベル。parse/Sheet）なので、記号レイヤどうしの前後関係は図に現れない。
	std::vector<std::string> planMarkLayerNames(const core::Document& document);
} // namespace HomeskzIfcImport::draw
