//
//	draw/DocumentFile.h
//
//	**図面ファイルそのものを扱う小道具**——いま開いている図面のパス・同じファイルか・
//	まだ無い一時パス・別名保存・開き直し。
//
//	【なぜ 1 か所に出したか】これを使うのは 2 つある。**実機フィードバックの周**
//	（draw/Feedback）と**回帰テスト**（draw/Regression。M28）で、どちらも「まっさらな
//	作業ファイルを別名保存して採り、次の周（次の 1 件）の頭で開き直す」という同じ作法で
//	図面を取り込み前へ戻す。**同じ作法を 2 か所に書かない**（CLAUDE.md「重複を作らない
//	置き場所」）——ここには実機でしか分からなかった但し書きが詰まっていて、片方だけ直すと
//	必ず食い違う。
//
//	使う口は `ISDK::SaveActiveDocumentPath` / `CloseDocument` / `OpenDocumentPath` /
//	`GetActiveDocument`（[SDK リファレンス「Documents」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Documents.md)。
//	コマンド実行中に呼べること・カレント文書が即座に移ることは実機確認済み）。
//
//	**閉じるのは呼び出し側が `gSDK->CloseDocument()` を直に呼ぶ。** ここに包まないのは、
//	**戻り値で分岐してはいけない**から——false を返しても実際には閉じていることがあり、
//	包むと「閉じられたか」を返す関数に見えてしまう（実機 round 9 でそれを信じて分岐し、
//	どこにも属さない状態で描いて全要素 0 件になった。docs/DEV-NOTES.md M25）。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）と VCOM の IFileIdentifier を
//	include する。このヘッダは標準ライブラリしか参照しない（CLAUDE.md「依存の向き」）。
//

#pragma once

#include <string>

namespace HomeskzIfcImport::draw
{
	// いまアクティブな図面のパス（取得できなければ空）。**開けたかどうかは
	// `OpenDocumentAt` の戻り値ではなくこれで判定する**（読み戻して確かめる。
	// SDK リファレンス「Investigation Techniques」）。
	//
	// **無題の図面でもパスは返る**——「アプリケーションのあるディレクトリ＋名称未設定 N」
	// が入る（実機確認済み。SDK リファレンス「Documents」）。
	std::string ActiveDocumentPath();

	// 同じファイルを指しているか。**大文字小文字や区切りの差で外さない**よう
	// std::filesystem に正規化させ、それが効かない場面（まだ無いファイルなど）は
	// 素の比較に落とす。
	bool SamePath(const std::string& left, const std::string& right);

	// **まだ無い一時パスを選ぶ**（`<temp>/<stem>-N.vwx`）。既にあるファイルへ別名保存
	// できなかった実測がある（実機 round 12。round 8 は同じ呼び出しが新規のパスで通って
	// いる）ので、上書きが効くことに賭けない。名前が尽きたら空を返す。
	std::string FreshTempPath(const std::string& stem);

	// アクティブな図面を指定のパスへ保存する（＝別名保存）。成功したら true。
	bool SaveActiveDocumentAs(const std::string& path);

	// 図面を開く。**戻り値は `OpenDocumentPath` が返したものそのまま**で、これを信じては
	// ならない——開けたかどうかは `ActiveDocumentPath` を読み戻して確かめること。戻り値を
	// 返すのは、うまくいかなかったときの記録に「SDK は何と言ったか」を残せるようにするため。
	//
	// ダイアログは出さない（bShowErrorMessages=false）——誰も見ていない周で止まらないため。
	bool OpenDocumentAt(const std::string& path);
} // namespace HomeskzIfcImport::draw
