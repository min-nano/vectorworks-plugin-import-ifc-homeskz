# アップデータのテスト（テンプレート由来）

（テストの一覧は [`tests/README.md`](../../../tests/README.md#何をテストしているか)）

1. **`UpdaterParseTests`** … `src/UpdaterParse.h` の純粋ロジック（`std::string` /
   `std::vector` だけに依存し、`gSDK`・`dladdr`・Win32・VWFC ダイアログに一切触れない
   関数）を関数単位でテストします。
2. **`UpdaterFlowTests`** … 更新フロー本体（`RunStableUpdateCheckWith` /
   `RunDevUpdateCheckWith`、`src/UpdaterFlow.cpp`）を、**フェイクの `IUpdaterHost`**
   越しに丸ごと動かして、分岐とダイアログ文言まで検証します（後述）。**3 つの入口の
   違い**（`UpdateCheckKind`）もここで押さえます——手で押した確認（`Manual`）は最新・
   オフラインを必ず伝えてブランチ選択を出す、取り込みのついで（`Silent`）は黙って同じ
   ブランチのビルドだけ拾って尋ねる。MCP の `vw_update` が通る `RemoteDevUpdateWith`（M38）は
   **ダイアログを 1 枚も出さず結末を値で返す**こと（名指ししたブランチの最新を入れること・
   殻まで変わったら降ろさず `NeedsRestart` を返すこと）も押さえます。加えて **どの入口も
   「いま」をディスクに入っているビルドで決める**こと（`q-dev` の `installed=` /
   `installed-branch=`。M26）——殻にコンパイルされた sha を基準にすると本体だけを入れ替えた
   あと同じビルドを毎周入れ直し、**殻のブランチを基準にすると、手で別のブランチのビルドへ
   乗り換えても前のブランチへ戻してしまう**（[M26](../../dev-notes/milestones/m26-current-build-from-disk.md)）。
3. **`UpdaterRobustnessTests`** … `src/UpdaterParse.h` のパーサに **予期しない外部入力**
   （壊れたスクリプト出力・埋め込み NUL・巨大／退化した行・ランダムなバイト列）を
   食わせ、境界外アクセスや未定義動作を起こさないこと、そして「戻り値の `url` は必ず
   非空」「`EvaluateStable` が更新を提示するのは整形式のときだけ」「`ResolveDevSelection`
   は範囲外を返さない」といった**契約**が保たれることを検証します。とりわけ
   **ASan / UBSan 有効時**（[テストの実行](../../../tests/README.md#テストの実行)）に真価を発揮し、リファクタが招くメモリ不正や、GitHub 側
   仕様変更で崩れた入力への耐性を守ります（`tests/UpdaterRobustnessTests.cpp`）。
4. **`UpdaterScriptTests`** … 同梱スクリプト `scripts/vw-update.sh`（macOS）の
   **機械可読バックエンド**（`q-stable` / `q-dev` / `do-install` と、その土台の
   `asset_url` / `installed_commit` / `installed_branch`）を、`curl` / `plutil` を差し替えて
   検証します（`tests/vw-update.test.sh`。[スクリプトのテスト](updater-scripts.md)）。`q-dev` が `installed=` に加えて
   **`installed-branch=`**（入っているビルドのブランチ。M26）を出すこともここで押さえます。
   あわせて**失敗したときに理由を言えること**（`curl_reason` / `header_value` /
   `http_reason` と、それを `error=` の 1 行へ添える経路。M27）——GitHub の API 制限を
   ほかの HTTP エラーと区別し、いつ戻るかまで言うこと・403 でも残り回数があれば制限と
   言い切らないこと・応答ヘッダを大文字小文字を問わず（リダイレクトで塊が複数並んでも）
   読めることを押さえます。**`q-pr-state`**（ブランチごとに PR が開いているか。実機テストの
   一時ファイルの片付けが使う）は、開いている PR が 1 つでもあれば `open`・PR が無ければ
   `none`・尋ねられなければ `error` で、**閉じたと言い切れないものを `closed` にしない**ことを
   押さえます。
5. **`UpdaterScriptTestsPs`** … その Windows 版 `scripts/vw-update.ps1` を、同じ発想で
   `Invoke-GH` / `Invoke-WebRequest` を差し替えて検証します（`tests/vw-update.Tests.ps1`。
   [スクリプトのテスト](updater-scripts.md)）。理由づくり（`Get-ApiFailureReason`。M27）も mac 側と同じ観点で押さえます
   ——応答は形だけ揃えたもので作るので、ネットワークも Windows も要りません。
   PowerShell 7（`pwsh`）は Linux でも動くので、**同じ Linux ランナー**で回せます。
6. **`InstallerScriptTests` / `InstallerScriptTestsPs`** … **配置を担うインストーラ**
   `scripts/vw-install.sh` / `scripts/vw-install.ps1`（配布 zip の直下とリリースのアセット
   として配られ、アップデータが配置を委ねる先）を検証します。中心の検査はひとつ——
   **zip の直下にあるものが、列挙されていなくても全部入ること**。ここが取りこぼすと利用者の
   `Plug-Ins` に半端なプラグインが残る、というのが実際に起きた事故で、この仕組みはその再発を
   止めるためにあります（`tests/vw-install.test.sh` / `tests/vw-install.Tests.ps1`）。
   アップデータ側には**委譲そのもの**のテストもあります（zip に入っていたインストーラが
   走ったか・その出力が素通しされるか・黙っているインストーラを成功と取り違えないか）。
7. **`UninstallerScriptTests` / `UninstallerScriptTestsPs`** … **取り除く**
   `scripts/vw-uninstall.sh` / `scripts/vw-uninstall.ps1`。ここは本リポジトリで唯一
   「利用者のディスク上のものを消す」コードなので、中心の検査は**削除の安全弁**です——
   フォルダ名が一致し、かつ中に殻があるときだけ消し、`Plug-Ins` そのものや無関係な
   フォルダを名指しされても消さないこと。あわせて「入っていなければ成功」（アップデートの
   入口で無条件に叩ける）も押さえます（`tests/vw-uninstall.test.sh` /
   `tests/vw-uninstall.Tests.ps1`）。**スタブはありません**——削除そのものが対象なので、
   本物が temp ディレクトリに対して走ります。

以降の節（`IUpdaterHost` によるフロー全体のテスト・残る部分）と
[スクリプトのテスト](updater-scripts.md)は、すべてこのアップデータ系統の話です。

## フロー全体のテスト（インターフェイス／フェイク方式）

判断だけでなく **フロー全体**（スクリプトに問い合わせ→判断→ダイアログ→インストール→
結果表示）も SDK 抜きでテストしています。フローが実行する副作用を 6 つに絞って
`IUpdaterHost`（`src/UpdaterHost.h`）というインターフェイスにまとめました。

| メソッド | 本番（`Updater.cpp`） | テスト（`UpdaterFlowTests.cpp`） |
|----------|----------------------|--------------------------------|
| `RunScript` | 同梱スクリプトを `popen` で実行 | 固定の stdout を返す |
| `Inform` / `Ask` | `gSDK->AlertInform` / `AlertQuestion` | 呼び出しを記録／既定の回答を返す |
| `PickBuild` | VWFC のプルダウンダイアログ | 選択インデックスを返す |
| `Restart` | SDK の `CloseAllFilesAndQuitVectorworks(true, true)` を呼ぶ | 呼び出し回数を数える／成否を返す |
| `DropLoadedPayload` | 載っている本体を降ろす（次の操作で新しいものが読み直される） | 呼び出し回数を数える／成否を返す |

フロー本体（`RunStableUpdateCheckWith` / `RunDevUpdateCheckWith`、`src/UpdaterFlow.cpp`）
は `IUpdaterHost&` だけに依存し、SDK ヘッダを一切 include しません。よって

- **本番** は `Updater.cpp` が `gSDK` / `popen` / VWFC で実装した本物の host を渡し、
- **テスト** は呼び出しを記録して canned な回答を返すフェイク host を渡す

だけで、「更新あり→肯定→インストール成功→完了ダイアログ」「ユーザーが拒否→何もしない」
「インストール失敗→エラー文言」といった経路を、**実際のダイアログ文言と `do-install` の
引数まで含めて**検証できます。

> これは **ユニットテスト（コンポーネントテスト）** です。テスト対象の「ユニット」は
> フロー関数で、host はそれを差し替えるテストダブル（フェイク）です。**e2e ではありません**
> — e2e なら実際に Vectorworks 上でプラグインを起動し、本物の GitHub API を叩き、本物の
> ダイアログを出して確認することになります。ここではプロセス内で SDK ゼロで完結します。

## それでも残る部分（アップデータ）

判断・フロー・スクリプトのバックエンドまで SDK 抜きでカバーできたので、テストが届いて
いないのは **プラットフォーム固有のグルーと外部 API 呼び出しそのもの** だけになりました。

### 1. `Updater.cpp` の薄いグルー

`BundledScriptPath` / `BundlePluginsDir` / `RunBundledScript`（`popen`）と、
`CVectorworksUpdaterHost` の各メソッド（`gSDK->AlertInform` / `AlertQuestion` の呼び出し、
VWFC ダイアログの生成）。ロジックは全て他へ委譲済みで、ここは「どの SDK 関数を呼ぶか」
という配線だけです。パスの導出は `*FromBinary`（テスト済み）に、選択→ビルドの写像は
`ResolveDevSelection`（テスト済み）に寄せてあるため、この層をさらにテストするための
SDK ヘッダ全体のスタブ化は、保守コストが高い割にリターンが小さいので推奨しません。
`CBuildPickerDialog` も同様に、責務を `PickBuild` の外（`ResolveDevSelection`）へ
出してあるので薄いままにしています。

### 2. スクリプトの OS 固有部分

`.sh` / `.ps1` のバックエンドは上記 `UpdaterScriptTests` / `UpdaterScriptTestsPs` で
ctest／CI に組み込み済みです。残るのは各 OS でしか動かない部分だけ、

- **macOS**（`.sh`）… `codesign` / `xattr` の再署名、`PlistBuddy`、osascript ダイアログ。
- **Windows**（`.ps1`）… `%APPDATA%` 既定パス、ロード中 `.vlb` の rename 退避など。

いずれもその OS の実機（＝手動／e2e）でしか意味を持たないため、C++ 側が
`dladdr` / `gSDK` のグルーを対象外にしているのと同じ理由で対象外とします。
