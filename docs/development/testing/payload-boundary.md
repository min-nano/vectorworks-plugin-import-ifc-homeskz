# 殻と本体の境界のテスト（ホットリロード）

（テストの一覧は [`tests/README.md`](../../../tests/README.md#何をテストしているか)）

プラグインは**殻**（VectorWorks が起動時に読み込むモジュール）と**本体**
（殻が自分で読み込む `.vwpayload`）に分かれています（CLAUDE.md「アーキテクチャ: 殻と本体」）。
実際の読み込み・アンロードは実機でしか確かめられませんが、**境界の 2 つは SDK 抜きで
テストできる**ので、そこだけ切り出してあります。

1. **`PayloadPathTests`** … 本体を探すパスの組み立て（`src/PayloadHost.h` の
   `payloadpath`）。ここが誤ると症状は「本体が見つからない」だけになり、どこで誤ったのか
   分からなくなります。特に **mac でバンドルの中を指してしまう**のは静かな事故で、署名が
   壊れて次の起動から読み込めなくなりえます（`tests/PayloadPathTests.cpp`）。
2. **`PayloadHostHolderTests`** … **殻の記憶域を本体が持ち続けないこと**
   （`src/PayloadHostHolder.h`）。境界を越えて渡した構造体をポインタのまま持つと、無効に
   なったポインタから関数ポインタを読んで**VectorWorks ごと異常終了します**——SDK リファレンス側で
   実際に異常終了した不具合の回帰テストです。コンパイルもリンクも CI の実ビルドも通ってしまう
   ので、**渡した記憶域を後から上書きしても中身が保たれているか**で確かめます
   （`tests/PayloadHostHolderTests.cpp`）。

「入れ替えに再起動が要るか」の判断（`InstalledShellId` / `NeedsRestartAfterInstall`）は
純粋関数なので `UpdaterParseTests` に、その分岐を通したフロー全体は `UpdaterFlowTests` に
あります。**殻が本体へ貸すもの**（同梱スクリプトの実行。M23）も `PayloadHostHolderTests`
の担当で、引数がそのまま渡ること・**返ってきた文字列を複製していること**・貸されなかった
古い殻でも本体が動くことを確かめます。
