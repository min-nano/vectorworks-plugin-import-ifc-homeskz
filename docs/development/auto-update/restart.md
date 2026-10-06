# 殻まで変わったときの再起動

コンパイル済みの殻は起動時にしか読み込まれないため、インストールしただけでは新しい殻は
動きません。そこでこの場合の表示は**通知ではなく質問**にしてあり、**「再起動」ボタン**を
その場に出します（`src/UpdaterFlow.cpp` の `OfferRestart`）。

- **「再起動」** → **Vectorworks 自身に終了と起動し直しを頼みます**（`src/Updater.cpp` の
  `CVectorworksUpdaterHost::Restart` → SDK の
  `CloseAllFilesAndQuitVectorworks(bAskForSave: true, bRestart: true)`）。開いている
  ファイルは**通常どおり保存を確認**してから閉じられ、保存ダイアログで取り消せば
  Vectorworks は落ちません（その場合もインストール済みのファイルはディスクに残るため、
  次回の起動で反映されます）。
- **「後で」** → 何もしません。反映は次に Vectorworks を起動したときです。

インストールに失敗したときは（当然）再起動を尋ねず、失敗の理由だけを表示します。再起動を
**頼めなかった**とき（SDK をまだ掴めていない）は「手動で再起動してください」と案内します
——押しても何も起きないように見えるのを避けるためです。

## 以前は SDK に頼めなかった（実機で確かめた失敗と、その前提が消えた経緯）

かつてこの再起動は、**終了要求も起動し直しも切り離した（detached）ヘルパープロセス**に
任せていました。SDK の `CloseAllFilesAndQuitVectorworks` が macOS 実機で次のように
失敗したためです。

1. `bRestart: true`（終了＋再起動を SDK に任せる）→ 古いインスタンスが終了しきる前に新しい
   インスタンスが立ち上がり、**「サポートファイルの読み込みに失敗しました。」**で落ちる。
2. `bRestart: false`（終了だけ SDK に任せ、起動し直しは自前）→ **同じダイアログが出る**。

原因は呼ぶ**時機**でした。当時のアップデート確認は**プラグインのロード中**（スプラッシュ
表示中、`plugin_module_main` の中）に走っており、Vectorworks 本体がまだ自分を終了させ
られる状態になっていなかったのです。SDK には「起動完了後に実行する」フックが無く
（`RegisterNotificationProcedure` の通知一覧にも起動完了に相当するものは無い）、いつ呼べば
安全かを当てにいくのは筋が悪いので、OS 経由の通常の終了要求（macOS: `quit` Apple event、
Windows: `CloseMainWindow()`）を送るヘルパーへ逃がしていました。

**その前提は M23 で消えました。** 確認の入口が起動時からメニューコマンドと取り込み
コマンドへ移り（[「いつ確認するか」](when-to-check.md)）、**Vectorworks が完全に動いている最中にしか
呼ばれなくなった**ので、素直に SDK へ頼めます。ヘルパーの一式（`MacRelaunchCommand` /
`WinRelaunchCommand` / `PowerShellQuote` / `MacAppBundleFromExecutable` と、それらが
組み立てる shell / PowerShell を検証していたテスト）はまとめて削除しました。

> **もし将来また起動時に確認したくなったら、この失敗を思い出してください。** SDK の
> 終了は「Vectorworks が動いていること」を前提にしています。

新しいビルドが実際にロードされるのは、この再起動（または手動での再起動）以降です。
