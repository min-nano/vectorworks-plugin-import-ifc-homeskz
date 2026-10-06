# スクリプトのテスト（ソース＋スタブ方式）

（テストの一覧は [`tests/README.md`](../../../tests/README.md#何をテストしているか)）

「更新の実体」を担う `scripts/vw-update.sh`（macOS）と `scripts/vw-update.ps1`
（Windows。GitHub API 取得・zip 展開・委譲）、**配置そのもの**を担う
`scripts/vw-install.{sh,ps1}`、そして**取り除く** `scripts/vw-uninstall.{sh,ps1}` も、
C++ 側と同じ発想で SDK ／ネットワーク抜きに単体テストします（`tests/vw-update.test.sh` /
`tests/vw-update.Tests.ps1` / `tests/vw-install.test.sh` / `tests/vw-install.Tests.ps1` /
`tests/vw-uninstall.test.sh` / `tests/vw-uninstall.Tests.ps1`）。Pester や bats などの
外部フレームワークは使わず、`TestFramework.h` と同じ**依存ゼロの極小ハーネス**を各
ファイルに同梱しています。

いずれも「実行（プラグイン・手動）ではディスパッチが走り、テストでは `source`
（dot-source）して個々の関数を直接呼ぶ」という**シーム**をスクリプト末尾に用意して
あります（`UpdaterFlow.cpp` の `IUpdaterHost` に対応するスクリプト版）。

```sh
# vw-update.sh
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
```

```powershell
# vw-update.ps1
if ($MyInvocation.InvocationName -ne '.') {
    switch ($mode) { ... }   # 実行時のみディスパッチ
}
```

プラグインは `.sh` を `/bin/bash <script>`、`.ps1` を `powershell -File <script>` で
**実行**するので本番は不変。テストだけが dot-source して、外側の I/O を差し替えます。

**`vw-update.sh`（bash）**

| 差し替える関数 | 本番 | テスト |
|----------------|------|--------|
| `jval` | `plutil`（macOS 専用）で JSON 抽出 | `python3` で同等のキーパス抽出。実際の `asset_url` / `q_stable` / `q_dev` がフィクスチャ JSON に対して動く |
| `api_get` | `curl` で GitHub REST API | フィクスチャファイルを返す（オフラインも再現） |
| `download` | `curl` でアセット取得 | ローカルの zip を配置（失敗も再現） |
| `installed_commit` | `PlistBuddy`（macOS 専用） | 既定コミットを返す（「バンドル無し → none」の枝は本物を直接検証） |

> **PowerShell のハーネスは、エラーの扱いをローカルと CI で変えている。** これは
> 「CI では緩めない」という `VW_REQUIRE_SCRIPT_TESTS` の方針をそのまま延長したもので、
> `$ErrorActionPreference` を **ローカルでは `Continue`・CI では `Stop`** にする。
>
> `Continue` のままだと、テスト本体の文が落ちても次へ進むため、その `CheckXxx` は
> **呼ばれないまま数にも入らない**——検査が空振りしたのに「PASS: all N checks」と出て、
> N だけが静かに減る。実際に `Join-Path 'C:\x' …`（Linux の pwsh に C: ドライブは無い）
> で 2 件が黙って抜けた。CI 側を `Stop` にすると、その文でスクリプトが終了して exit 1 に
> なるので、必ず気付ける。ローカルを `Continue` のままにしてあるのは、直すときは失敗を
> 一覧できたほうが速いからで、これも skip / hard-fail の使い分けと同じ考え方である。
>
> **bash のハーネスには同じ手が使えない。** あちらは `RUN` の直後に `$?` を見る書き方を
> しており（`check_eq "$?" "1"` など）、`set -e` にすると意図した失敗でハーネスごと
> 止まってしまう。代わりに、各関数を `set -euo pipefail` の**サブシェル**で走らせて
> 本物の挙動を保っている（`RUN`）。

**`vw-update.ps1`（PowerShell 7）** — こちらは差し替えが 2 つで済み、より本物に近い形で
動きます（`Get-InstalledCommit` は macOS ツールではなく `<name>.commit` テキストを読む
だけなので実物、`Expand-Archive` / `Copy-Item` も pwsh の標準機能でそのまま動く）。

| 差し替える関数 | 本番 | テスト |
|----------------|------|--------|
| `Invoke-GH` | `Invoke-RestMethod` で GitHub REST API | フィクスチャ JSON を `ConvertFrom-Json` して返す（オフラインは throw） |
| `Invoke-WebRequest` | アセットをダウンロード | ローカルの zip を `-OutFile` にコピー（失敗は throw） |

これで両スクリプトとも、`q-stable` の `installed` / `latest`（7 桁化）/ `url`、`q-dev` の
`dev-*` フィルタとアセットのあるビルドだけの列挙、`do-install` の成功・各失敗経路
（ダウンロード失敗／想定外の zip／引数不足）まで、**素の Linux ランナーで**検証できます。

**インストーラ（`vw-install.*`）のテストはさらに実物寄り**です。差し替えるのは JSON 抽出
（`jval`）と Gatekeeper のツール（`codesign` / `xattr`）と `PlistBuddy` だけで、配置そのもの
——展開済みディレクトリの走査・差し替え・インストーラ自身の除外——は本物が temp ディレクトリ
に対して動きます。要となる検査は次の 3 つです。

* **列挙されていないファイルも入ること**（フィクスチャに `<name>.brand-new` を混ぜてある。
  これが落ちたら、次にファイルが増えたとき利用者が手で入れ直す羽目になる）。
* **インストーラ自身は `Plug-Ins` へ置かないこと**。
* **知らないオプションで落ちないこと**（新しいアップデータが古い zip の中のインストーラを
  呼ぶ向きが起こりうるため）。
* **プラグインのフォルダに入り、入れ子にならないこと**（アップデータは「いま自分が
  読み込まれたフォルダ」を渡してくるので、無条件に足すと更新のたびに深くなる）。
* **入れる前に前の版が取り除かれること**（前の版にしか無かったファイルが残らない）。
更新後の**再起動**はスクリプトの仕事ではありません。Vectorworks 自身に頼むので
（SDK の `CloseAllFilesAndQuitVectorworks`。`src/Updater.cpp` の `Restart`）、テストで
押さえるのは**いつ再起動を尋ねるか**——殻まで変わったときだけで、本体だけなら尋ねずに
降ろす——という判断のほうです（`UpdaterFlowTests`）。SDK 呼び出しそのものは
`IUpdaterHost` の向こう側なので、実機での目視確認に委ねます。

`q-dev` の出力に足した**5 列目（ブランチ名）**は両側で押さえてあります——スクリプトが
リリース本文の `branch=` から拾えること（`tests/vw-update.test.sh` /
`tests/vw-update.Tests.ps1`）と、**列が無い古い出力も読めること**
（`tests/UpdaterParseTests.cpp` の `ParseDevBuilds`）。

各スクリプトに残る OS 固有の面（`.sh` の osascript ダイアログ・`codesign` / `xattr` の
再署名・`PlistBuddy`、`.ps1` の `%APPDATA%` 既定パス）は、その OS でしか動かないため、
C++ 側が `dladdr` / `gSDK` のグルーを対象外にしているのと同様、手動／e2e に委ねます。
必要なツール（`.sh`: python3 / unzip / zip、`.ps1`: `pwsh`）が無い**ローカル**環境では、
ハーネス自体が自動で SKIP、あるいは CMake がそのテストを登録しません（`scripts/lint.sh` の
`skip` と同じ方針）。

ただし **CI では黙ってスキップさせません**。ツールが欠けたまま「テストが 1 件も走らずに緑」
になるのを防ぐため、`-DVW_REQUIRE_SCRIPT_TESTS=ON`（Tests ワークフローが指定）を付けると
挙動が逆転します。インタプリタ（`bash` / `pwsh`）が無ければ **configure が FATAL_ERROR** で
失敗し、ハーネスに渡す補助ツール（python3 / unzip / zip）が無ければ **ハーネスが SKIP では
なく exit 1** で失敗します。**PowerShell のハーネスはあわせて `$ErrorActionPreference` も
`Stop` へ切り替え**、想定外のエラーを見逃さないようにします（上記の注意書き）。この値は各ハーネスの環境変数 `VW_REQUIRE_SCRIPT_TESTS` として
渡され、ローカル既定（OFF）では従来どおり穏やかに SKIP します。
