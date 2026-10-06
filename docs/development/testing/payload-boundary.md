# 殻と本体の境界のテスト（ホットリロード）

（テストの一覧は [`tests/README.md`](../../../tests/README.md#何をテストしているか)）

プラグインは**殻**（VectorWorks が起動時に読み込むモジュール）と**本体**
（殻が自分で読み込む `.vwpayload`）に割れています（CLAUDE.md「アーキテクチャ: 殻と本体」）。
実際の読み込み・アンロードは実機でしか確かめられませんが、**境界の 2 つは SDK 抜きで
テストできる**ので、そこだけ切り出してあります。

1. **`PayloadPathTests`** … 本体を探すパスの組み立て（`src/PayloadHost.h` の
   `payloadpath`）。ここが狂うと症状は「本体が見つからない」だけになり、どこで曲がったのか
   分からなくなります。特に **mac でバンドルの中を指してしまう**のは静かな事故で、署名が
   壊れて次の起動から読み込めなくなりえます（`tests/PayloadPathTests.cpp`）。
2. **`PayloadHostHolderTests`** … **殻の記憶域を本体が持ち続けないこと**
   （`src/PayloadHostHolder.h`）。境界を越えて渡した構造体をポインタのまま持つと、腐った
   ポインタから関数ポインタを読んで**VectorWorks ごと落ちます**——SDK リファレンス側で
   実際に落ちた壊れ方の回帰テストです。コンパイルもリンクも CI の実ビルドも通ってしまう
   ので、**渡した記憶域を後から塗り潰しても中身が生きているか**で確かめます
   （`tests/PayloadHostHolderTests.cpp`）。

「入れ替えに再起動が要るか」の判断（`InstalledShellId` / `NeedsRestartAfterInstall`）は
純粋関数なので `UpdaterParseTests` に、その分岐を通したフロー全体は `UpdaterFlowTests` に
あります。**殻が本体へ貸すもの**（同梱スクリプトの実行。M23）も `PayloadHostHolderTests`
の担当で、引数がそのまま渡ること・**返ってきた文字列を写していること**・貸されなかった
古い殻でも本体が動くことを確かめます。

`Updater.cpp` は残った

- 自分自身のバイナリ位置の解決（`dladdr` / `GetModuleFileName`）
- スクリプトの起動（`popen` / `_popen`）
- ネイティブダイアログの表示（`gSDK->AlertInform` / `AlertQuestion`、`VWDialog`）

という **プラットフォーム／SDK 固有のグルーだけ** を担います。

`UpdaterParse.h` の関数は 3 層に分かれます。

| 層 | 関数 | 役割 |
|----|------|------|
| スクリプト出力の解析 | `Trim` / `ValueOf` / `ParseDevBuilds` | `key=value` 行・`build\t…` 行の解析 |
| コマンドライン生成 | `ShellQuote` / `CmdQuote` | `/bin/sh`・cmd.exe 用の安全なクオート |
| 自パスからの導出 | `Mac*FromBinary` / `Win*FromPath/Dir` | 同梱スクリプト・Plug-Ins フォルダのパス導出 |
| **更新フローの判断** | `EvaluateStable` / `ResolveCurrentDevBuild` / `DevSwitchCandidates` / `FindDevBuildForBranch` / `ResolveDevSelection` / `InstallReportedOk` / `InstallErrorText` / `InstalledShellId` / `NeedsRestartAfterInstall` | 「更新があるか」「**いま入っているのはどのブランチのどのビルドか**」「切替候補はどれか」「同じブランチの新しいビルドはどれか」「選択→ビルド」「インストール成否」「**再起動が要るか、本体の読み直しで済むか**」 |

最後の「更新フローの判断」層は、もともと `Updater.cpp` の `gSDK` 呼び出しの合間に
インラインで書かれていた分岐です。純粋関数として切り出したことで単体テストの対象になり、
`Updater.cpp` 側は判断結果を受けてダイアログを出すだけになりました。
