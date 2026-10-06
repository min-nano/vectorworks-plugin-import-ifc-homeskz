# 継続的インテグレーション（CI）

（このフォルダのページの一覧は[開発ガイドの目次「CI とリリース」](../../DEVELOPMENT.md#ci-とリリース)）

`.github/workflows/build.yml` がプラグインをビルドします。`main` は保護された
デフォルトブランチで、機能開発は必ず PR 上で行うため、ブランチを二重にビルドしない
ようトリガを分けています。

- **`main` への push**（マージ）は **stable** リリースをビルドして公開します。
- **PR** はそのブランチをビルドして **dev** プレリリースを公開します。

ワークフローの内容:

- **4 つの並行ジョブ**を持ちます。ビルドの `build-mac`（`macos-15`・Apple Silicon、
  Xcode 16.2）と `build-windows`（`windows-latest`・Visual Studio 2022）、および
  静的解析の `tidy-mac` / `tidy-windows` です。4 つとも**同時**に走り、互いを待ちません
  （clang-tidy はビルドより時間がかかるので、ビルドの中に置かず並走させています。
  詳細は[「SDK 依存コードの静的解析」](../lint/clang-tidy-sdk.md)）。
- SDK は一度だけダウンロードし、（トリミングした）SDK を**キャッシュ**するので、大きな
  zip は以降の実行で再ダウンロードされません。強制的に再ダウンロードするにはワーク
  フロー内の `VW_SDK_CACHE_KEY`（プラットフォームごとに 1 つ）を変更します。SDK を
  用意する手順そのものは `scripts/fetch-vw-sdk.sh` に 1 つだけあり、4 ジョブと
  `ci-debug.yml` が共有します（キャッシュがヒットしていれば検証だけして抜けます）。
- 各ジョブは**その実行が公開するチャンネルだけ**をビルドします（`-DVW_BUILD_CHANNEL`。
  `main` は `min-nano_structure`、PR は `min-nano_structureDev`）。コミットで刻印
  （`-DVW_BUILD_VERSION`）して成果物を確認・アップロードします（macOS はさらにアドホック
  署名）。PR ではエフェメラルなマージコミットではなく、PR の **head** コミット（あなたが
  push したもの）をビルドします。
- **ダウンロード可能なリリースを公開**し、アップデータが取得できる安定した URL を用意
  します。1 つのリリースに **macOS と Windows 両方のアセット**が入ります:
  - `main` はローリングな **`stable`** リリースを更新します
    （`min-nano_structure.vwlibrary.zip` + `min-nano_structure.vlb.zip`）。
  - PR はブランチごとの **`dev-<branch>`** プレリリースを更新します
    （`min-nano_structureDev.vwlibrary.zip` + `min-nano_structureDev.vlb.zip`。トークンで公開でき
    ないフォーク PR では `release` ジョブごとスキップされます）。

  リリースの公開は独立した **`release` ジョブ**が担当します。このジョブは 4 つのジョブ
  （`build-mac` / `build-windows` / `tidy-mac` / `tidy-windows`）が**すべて**完了してから
  走り（`needs: [build-mac, build-windows, tidy-mac, tidy-windows]`）、両ビルドジョブが
  アップロードした成果物をまとめてダウンロード
  し、**macOS と Windows 両方のアセットを 1 つのリリースに**添付して公開します。公開を
  ビルドから切り出したことで、どちらのプラットフォームも単独でリリースを作らなくなり、
  作成とアタッチが競合することがありません。静的解析ジョブも `needs` に入っているので、
  **ビルドが通っていても clang-tidy が通らなければリリースは公開されません**（解析は
  ビルドと並走しているため、このゲートを保っても所要時間は増えません）。どちらもローリング方式で、毎回タグを最新
  ビルドに貼り直します。**stable** の公開は GitHub API の長時間障害があってもリトライ
  します（stable リリースの取りこぼしは気づかれにくいため）。**dev** の公開はリトライ
  しません — dev ビルドはブランチ作業中にしか使わないので、一時的なエラーが出たら
  ジョブを再実行すれば十分です。

  公開に至らない実行 — トークンでリリースを作れない**フォーク PR** や、`main` 以外の
  ref での `workflow_dispatch` — では、**`release` ジョブ自体が起動しません**（ジョブの
  `if` で振り分けているので、成果物のダウンロードも走りません）。スキップされたジョブの
  チェック結論は `skipped` で失敗ではないため、ブランチ保護や `ci-wait` の判定には
  影響しません。

`.github/workflows/cleanup-dev-release.yml` は、**PR がクローズされたとき**（マージの
有無を問わない）にその `dev-<branch>` プレリリース（とタグ）を削除し、dev ビルドが
溜まらないようにします。プレリリースは**PR が開いているあいだの成果物**（公開するのは
`build.yml` の `pull_request` 実行だけ）なので、PR が閉じた時点が役目の終わりです。
以前はブランチ削除（`delete` イベント）を合図にしていましたが、それだと**ブランチを
残したままマージした PR**や**マージせず閉じた PR**のプレリリースが残り続け、逆に PR を
持たないブランチの削除でも起動していました。

`pull_request` の `closed` で起動するため、ワークフローの実体は PR のマージ ref
（head を base にマージしたもの）側のコピーから実行されます。したがってこの変更が
`main` に入って初めて有効になり、それ以前から開いている PR も、マージ ref が新しい
`main` に対して作り直された時点でこの版を拾います。フォークからの PR は
（そもそも `build.yml` がプレリリースを公開しないので）ジョブの `if` で除外します。

これと対になる取り決めが `build.yml` 側にもあります。ビルドの実行中に PR が閉じられると、
片付けの側は「まだ公開されていないプレリリース」を探して空振りするので、その後にビルドが
公開すると**誰も消さないプレリリース**が残ります。これを避けるため、dev の公開ステップは
**PR がまだ open か**を公開の前後で確かめます（以前はブランチの存在を見ていました）。

`.github/workflows/stable-release-healthcheck.yml` はスケジュール（6 時間ごと）で
安全網として実行されます。公開済みの `stable` リリースが `main` の先頭からずれている
場合 — つまり stable の公開を取りこぼした場合 — `main` で `build.yml` を再ディスパッチ
して再ビルド・再公開します。スケジュール起動のワークフローはデフォルトブランチから
実行されるため、`main` にマージされて初めて有効になります。

## 手動ディスパッチ（`workflow_dispatch`）を持つワークフロー

「Run workflow」ボタンは**必要なものにだけ**付けています。PR とマージで自動的に走る
チェック系（`lint.yml` / `test.yml` / `codeql.yml`）は手動起動する用途が無く、失敗した
実行を回し直したいだけなら Actions 画面の **Re-run** で足りるためディスパッチを持ちません。

| ワークフロー | 手動ディスパッチ | 理由 |
| --- | :---: | --- |
| `build.yml` | あり | `stable-release-healthcheck.yml` が stable の取りこぼしを検知して再ディスパッチする（リリース経路） |
| `stable-release-healthcheck.yml` | あり | 6 時間の次回スケジュールを待たずに stable のずれを直したいとき |
| `ci-debug.yml` | あり（専用） | 手動ディスパッチ**のみ**で起動する調査用ワークフロー |
| `lint.yml` / `test.yml` / `codeql.yml` | なし | push / PR（+ CodeQL は週次スケジュール）で自動的に走る |
| `cleanup-dev-release.yml` | なし | PR のクローズ（`pull_request` の `closed`）専用 |
| `pr-review.yml` | なし | 作成・昇格・インラインの返信で自動的に走り、ほかは `@claude review` のコメントで起こす |
