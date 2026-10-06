# インストール

（利用者向けのページの一覧は [`README.md`](../../README.md#使い方とドキュメント)）

このプラグインは社内利用向けで、**未署名**（Apple Developer ID 署名なし、Vectorworks
開発者クレデンシャルなし）で配布されます。

**stable と dev の 2 つが共存できます。** 同じソースから、共存できる別名のプラグインを
ビルドしています。

- **`min-nano_structure`** — *stable*（表示名「みんなの構造設計支援」）。`main` から
  ビルドされます。コマンド名は **IFC (ホームズ君) 取り込み…** と
  **アップデータを確認 (みんなの構造設計支援)**。
- **`min-nano_structureDev`** — *dev*（表示名「みんなの構造設計支援Dev」）。PR を開いて
  いる作業ブランチからビルドされます。コマンド名は **IFC (ホームズ君) 取り込み… (Dev)** と
  **アップデータを確認 (みんなの構造設計支援Dev)**。

出力名・リソース識別子・拡張機能 UUID がそれぞれ別なので、両方を同時に入れて使えます
（通常利用は stable、作業中のブランチを試すのは dev）。ビルドは
[GitHub のリリース](https://github.com/min-nano/vectorworks-plugin-import-ifc-homeskz/releases)
から取得します（stable は `stable` リリース、dev は `dev-<ブランチ名>` プレリリース。
ブランチ名の `/` などの記号は `-` に置き換わり、PR を閉じるとそのプレリリースは消えます）。

## 置き場所: プラグインは自分のフォルダを 1 つ持つ

配布物は **`Plug-Ins` の直下ではなく、プラグイン名のフォルダ**に入ります。

```
~/Library/Application Support/Vectorworks/2026/Plug-Ins/
  └── min-nano_structure/
        ├── min-nano_structure.vwlibrary
        ├── min-nano_structure.vwpayload
        ├── vw-mcp-server.py
        └── vw-uninstall.sh
```

こうしておくと**そのプラグインのものが 1 か所に閉じる**ので、消したいときはフォルダを
1 つ捨てるだけで済みます（同梱の `vw-uninstall.sh` / `vw-uninstall.ps1` も同じことを
します）。下記のインストーラを使えば、この形は自動的に作られます。

## 2 つのファイルで 1 つのプラグイン

**配布物は 2 つに割れています。** どちらも上のフォルダへ、**必ず一緒に**置いて
ください（片方だけでは動きません）。

| ファイル | 何か |
| --- | --- |
| `min-nano_structure.vwlibrary`（mac）／ `min-nano_structure.vlb`（win） | **殻**。Vectorworks が起動時に読み込むモジュールで、メニューと PIO（柱記号・耐力壁）の登録・アップデートを持ちます |
| `min-nano_structure.vwpayload` | **中身**。取り込みの処理と PIO の作図がすべて入っています。殻がこれを自分で読み込みます |

こう割ってあるのは、**アップデートで Vectorworks を再起動しなくて済むように**するため
です（[「アップデート」](update.md)）。`.vwpayload` は Vectorworks から見ればただのファイルなので、
プラグインとして二重に読み込まれることはありません。

## いちばん簡単な入れ方: インストーラを使う

**リリースにはインストーラのスクリプトが一緒に置いてあります。** これを落として実行すれば、
配布 zip の取得・展開・隔離解除・アドホック署名・`Plug-Ins` への配置まで全部やります。
**構成ファイルが増えても、その時点の正しい構成で入ります**（インストーラはリリースごとに
一緒に更新されるため）。

```sh
# --- macOS -------------------------------------------------------------------
curl -fsSLO https://github.com/min-nano/vectorworks-plugin-import-ifc-homeskz/releases/download/stable/vw-install.sh
bash vw-install.sh                       # 最新の stable を入れる
bash vw-install.sh --tag dev-<ブランチ名> # 開発版を入れる
```

```pwsh
# --- Windows -----------------------------------------------------------------
iwr -UseBasicParsing -OutFile vw-install.ps1 `
  https://github.com/min-nano/vectorworks-plugin-import-ifc-homeskz/releases/download/stable/vw-install.ps1
powershell -ExecutionPolicy Bypass -File vw-install.ps1
```

インストール先は Vectorworks 2026 のユーザフォルダ内の
`Plug-Ins/min-nano_structure/`（dev は `Plug-Ins/min-nano_structureDev/`）です。別の場所に
入れたいときは `--plugins-dir <パス>`（Windows は `-PluginsDir <パス>`）で `Plug-Ins` に
あたる場所を指定してください
（プラグイン名のフォルダはその中に作られます）。入れ終わったら、下記「macOS」の
手順 3・4（Windows は 2・3）——Vectorworks を起動してコマンドをワークスペースに追加する
——だけ行ってください。

**取り除くとき**も同じ場所にスクリプトがあります（インストールすると
`Plug-Ins/min-nano_structure/vw-uninstall.sh` にも入ります）。

```sh
bash vw-uninstall.sh --name min-nano_structure     # stable を取り除く
bash vw-uninstall.sh --name min-nano_structureDev  # dev を取り除く
```

```pwsh
powershell -ExecutionPolicy Bypass -File vw-uninstall.ps1 -Name min-nano_structure     # stable
powershell -ExecutionPolicy Bypass -File vw-uninstall.ps1 -Name min-nano_structureDev  # dev
```

`--name`（Windows は `-Name`）を省くと、`Plug-Ins` の中で見つかったものを 1 つだけ
取り除きます（stable と dev の両方が入っているときは、どちらを消すか名前で指定してください）。

以下は、zip を自分で展開して置く**手作業の手順**です。

## macOS

プラグインの入れ物は `min-nano_structure.vwlibrary` バンドルで、リソースはバンドル内に
含まれます。**中身（`min-nano_structure.vwpayload`）はバンドルの隣**に置きます（バンドルの
署名はリソースまで封をするので、中に入れると差し替えたときに署名が壊れます）。

1. **バンドルと `.vwpayload` をローカルディスクに置きます**（iCloud Drive は不可 — iCloud が
   ダウンロード隔離フラグを付け直すことがあります）。置き場所は Vectorworks 2026 の
   ユーザフォルダ内の `Plug-Ins/min-nano_structure/` です（`Plug-Ins` は
   Vectorworks ▸ 環境設定 ▸ *ユーザフォルダ* から探せます。`min-nano_structure`
   フォルダは自分で作ります）。

2. Gatekeeper がブロックしないよう、**macOS の隔離フラグを解除します**:

   ```sh
   xattr -dr com.apple.quarantine min-nano_structure.vwlibrary
   xattr -d  com.apple.quarantine min-nano_structure.vwpayload
   ```

   CI ビルドは既に**アドホック署名済み**です（Apple Silicon がバイナリをロードするために
   必須。無料であり、Developer ID 署名ではありません）。それでも macOS が「壊れている」と
   言う場合は、自分で署名し直してください:

   ```sh
   codesign --force --deep --sign - min-nano_structure.vwlibrary
   codesign --force --sign - min-nano_structure.vwpayload
   ```

3. **Vectorworks を起動します。** プラグインが未署名のため、Vectorworks 2026 は起動時に
   「不明／未署名のプラグイン」警告を表示し、既定で無効化することがあります。警告を
   了解してプラグインを有効化してください — 社内向け・クレデンシャルなしのプラグインでは
   想定どおりの挙動です。

4. **コマンドをワークスペースに追加します:** ツール ▸ ワークスペース ▸ 現在の
   ワークスペースを編集 ▸ *メニュー*。**みんなの構造設計支援**（dev 版は
   **みんなの構造設計支援Dev**）カテゴリに、このプラグインのコマンドがまとまって
   入っています——**IFC (ホームズ君) 取り込み…** と
   **アップデータを確認 (みんなの構造設計支援)** を、好きなメニューへドラッグして
   ください（2 つとも追加しておくと、更新を思い立ったときにすぐ確認できます）。
   開発版の **MCP ブリッジを表示… (Dev)** と **実機テストを実行… (みんなの構造設計支援Dev)**
   は開発用なので、使うときだけ追加すれば十分です（[「MCP ブリッジ」](mcp-bridge.md)）。

## Windows

プラグインの入れ物は `min-nano_structure.vlb`（DLL）で、リソースは同名の別ファイルとして
隣に置きます（SDK の Windows での作法）。

1. **`min-nano_structure.vlb` と `min-nano_structure.vwpayload` と `min-nano_structure.vwr` を
   一緒に**、Vectorworks 2026 のユーザフォルダ内の `Plug-Ins/min-nano_structure/` へ
   置きます（`min-nano_structure` フォルダは自分で作ります）。3 つは同名・同フォルダで
   ある必要があります。自動アップデートも使うなら、配布 zip の直下にあるものを
   `vw-install.ps1` 以外**すべて**一緒に置きます（`min-nano_structure.commit`・
   `min-nano_structure.branch`・`min-nano_structure.shell-id`・`vw-update.ps1`・
   `vw-token.ps1`・`vw-uninstall.ps1` など。インストーラと同じ置き方です）。

2. **Vectorworks を起動します**（未署名の警告は macOS と同じ）。

3. **コマンドをワークスペースに追加します**（macOS の手順 4 と同じ）。
