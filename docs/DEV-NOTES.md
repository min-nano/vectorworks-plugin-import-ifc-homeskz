# 開発メモ — 設計の記録とホームズ君 IFC の癖

このプラグインを作る過程での**設計上の判断**と、**ホームズ君 IFC（入力データ）に固有の
癖**、実機確認の作法をまとめたものです。トピックごとに [`docs/dev-notes/`](dev-notes/) の
下へ分けてあり、**このページはその目次**です。

**Vectorworks SDK の実測知見（実機でしか判明しなかった落とし穴・SDK に無い／機能しない
API・SDK 側の打ち切った調査）は、SDK リファレンスリポジトリ
[vectorworks-developer-sdk-reference](https://github.com/min-nano/vectorworks-developer-sdk-reference)
の [`Findings/`](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/README.md)
へ移動しました。** SDK の挙動について新しく分かったことも、開発メモではなくあちらへ
追加します（[`CLAUDE.md`](../CLAUDE.md)「ドキュメントの分担」）。

- 利用者向けの説明（何をするプラグインか・使い方）は [`README.md`](../README.md)。
- ビルド・テスト・lint・CI の手順は [`DEVELOPMENT.md`](DEVELOPMENT.md)。
- 作業時の規約（ディレクトリ・命名・依存の向き・PR とマージの規則）は
  [`CLAUDE.md`](../CLAUDE.md)。

**ソースコードのコメントにある `docs/DEV-NOTES.md M<数字>`** は、機能を積み上げていった
当時のマイルストーン番号です。[実装の経緯（M0〜M40）](dev-notes/milestones/README.md)に
対応表があり、ページのあるものは下の「実装の経緯」から番号で参照できます。番号は**振り直さない**
（既存のコメント・PR 本文が参照しているため）。`docs/DEV-NOTES.md「<節の名前>」` も、
下の一覧の同じ名前のページ（小見出しはそのページの中）で参照できます。

## 設計の考え方

| ページ | 中身 |
| --- | --- |
| [アーキテクチャの土台](dev-notes/design/architecture.md) | 2 フェーズ分離が全ての土台／命令セット（Document）の設計則／レイヤ・クラス・レベルの規約は 1 か所に置く |
| [描いた結果に対する 5 つの方針](dev-notes/design/drawing-policies.md) | 空のものを先に作らない・既存リソースを書き換えない・決定性・1 要素の欠損で止めない・診断は異常だけ |
| [部材の位置決め](dev-notes/design/member-placement.md) | 形状を先に確定し、支持部材を合わせる／部材の端点は接合点（芯線の交点）に置く |
| [構造材のスタイルをやめた](dev-notes/design/structural-member-style.md) | スタイルを当てず、パーツごとの 2D 属性をクラスへ従わせる |
| [用紙の割り付けの決め事（M18）](dev-notes/design/page-layout.md) | 縮尺の階梯・伏図の位置・凡例の幅・軸組図の上下 2 段 |
| [診断・完了報告の方針（M15/M19）](dev-notes/design/diagnostics.md) | 進捗の重み・完了ダイアログ・ログ・検算は開発ビルドだけ |
| [取り込み設定の決め事（M20）](dev-notes/design/import-options.md) | 設定ダイアログ・役割の表・既定・候補の絞り方 |

## 入力データ・調査・確認

| ページ | 中身 |
| --- | --- |
| [IFC / STEP 側の癖](dev-notes/ifc-quirks.md) | ホームズ君の出力仕様（公式の説明）／読んでみて分かった癖 |
| [打ち切った調査（もう一度やらないこと）](dev-notes/abandoned-investigations.md) | 本プラグインの方針・実装として打ち切ったもの |
| [Vectorworks SDK の実測知見（SDK リファレンスへ移動）](dev-notes/sdk-findings-moved.md) | 旧トピックと `Findings/` の移動先の対応 |
| [実機確認の作法](dev-notes/live-verification.md) | 診断行と OIP の読み合わせ・API の無い設定の特定の仕方・往復の回し方 |
| [描画の高速化（測って初めて場所が分かった）](dev-notes/draw-performance.md) | 予想と実測・何が重いか・`doRegen=false`・打ち切った候補 |
| [残っている宿題](dev-notes/open-issues.md) | 未対応・未確認のもの |

## 実装の経緯（M0〜M40）

[対応表（M0〜M40 の要約）](dev-notes/milestones/README.md)と、ページのあるマイルストーン
（番号順）。いくつかは長いのでフォルダに分けてあり、`README.md` から読み始めます。

| # | ページ |
| --- | --- |
| M10 | [地中梁の可視ソリッドが幅方向にずれる（M10 の続き）](dev-notes/milestones/m10-ground-beam-shift.md) |
| M17 | [基礎の床付け（捨てコン・砕石）は 3 通りに分かれる（M17）](dev-notes/milestones/m17-footing-bedding.md) |
| M19 | [耐力壁は「柱に紐付く線分 PIO」で描く（M19）](dev-notes/milestones/m19-shear-wall.md) |
| M19.5 | [共通化で寄せたもの（M19.5）](dev-notes/milestones/m19-5-consolidation.md) |
| M21 | [殻と本体に割ってホットリロードする（M21）](dev-notes/milestones/m21-shell-and-payload.md) |
| M22 | [配置の手順をリリース側へ移す（M22）](dev-notes/milestones/m22-installer-in-release.md) |
| M23 | [実機確認の往復をプラグインに回させる（M23）](dev-notes/milestones/m23-feedback-loop/README.md)——[人に尋ねること・伝えること](dev-notes/milestones/m23-feedback-loop/asking.md)（所見はプラグインの仕事ではなかった ほか）・[周の回し方](dev-notes/milestones/m23-feedback-loop/loop-flow.md)（ビルドを待つのをやめた ほか）・[同梱スクリプトの落とし穴](dev-notes/milestones/m23-feedback-loop/bundled-scripts.md)（bash 3.2・殻の ID から外した）・[図面が戻っているかを測る](dev-notes/milestones/m23-feedback-loop/baseline.md)（基準は 1 周目に採る ほか） |
| M23 | [プラグインの位置付けと、更新の起こし方（M23）](dev-notes/milestones/m23-update-trigger.md) |
| M24 | [往復をモードレスのパレットで回す（M24）](dev-notes/milestones/m24-feedback-palette.md) |
| M24 | [Claude と図面をつなぐ MCP ブリッジ（M24）](dev-notes/milestones/m24-mcp-bridge.md) |
| M25 | [実機テストを本番の取り込みから分ける（M25）](dev-notes/milestones/m25-test-separate-from-import/README.md)——[前の周の図を取り除く・取り消す](dev-notes/milestones/m25-test-separate-from-import/layer-removal-and-undo.md)・[作業ファイルを開き直す](dev-notes/milestones/m25-test-separate-from-import/work-file.md)（汚れた基準は焼き付く ほか）・[無人の周を勝手に回さない・止めない](dev-notes/milestones/m25-test-separate-from-import/unattended-rounds.md)（隠れたパレットは止まらない・図番が重なると訊いてくる） |
| M26 | [「いま動いているビルド」はディスクに聞く（M26）](dev-notes/milestones/m26-current-build-from-disk.md) |
| M27 | [往復が 1 時間 60 回の壁に当たる（M27）](dev-notes/milestones/m27-api-rate-limit.md) |
| M27 | [柱が長さ 0 で描かれる（M27）](dev-notes/milestones/m27-zero-length-column/README.md)——[自己修復をやめた](dev-notes/milestones/m27-zero-length-column/self-repair.md)（水平材の実体は「スパン」では測れない ほか）・[原因を突き止めた](dev-notes/milestones/m27-zero-length-column/root-cause.md)（実機 round 1 / round 2 の測定・両端に同じ Z を渡す）・[直ったあとに外したもの](dev-notes/milestones/m27-zero-length-column/after-fix.md)・[パスは 2D で渡す](dev-notes/milestones/m27-zero-length-column/path-2d.md) |
| M28 | [図面枠をシートレイヤへ置く（M28）](dev-notes/milestones/m28-title-block.md) |
| M29 | [「用紙に収まらなかった」が周ごとにぶれる（M29）](dev-notes/milestones/m29-fit-overflow-jitter.md) |
| M30 | [MCP ブリッジを常駐させる（M30）](dev-notes/milestones/m30-mcp-resident.md) |
| M31 | [伏図と軸組図へ寸法を自動で入れる（M31）](dev-notes/milestones/m31-dimensions/README.md)——[描画側の作法](dev-notes/milestones/m31-dimensions/drawing.md)・[実機で分かったこと](dev-notes/milestones/m31-dimensions/live-findings.md)・[レベル基準線の描き方の調整](dev-notes/milestones/m31-dimensions/level-lines.md) |
| M32 | [軸組図の図面ラベル（M32）](dev-notes/milestones/m32-drawing-label.md) |
| M33 | [横架材の継手（M33）](dev-notes/milestones/m33-splice.md) |
| M34 | [軸組図にする通りを選ぶ（M34）](dev-notes/milestones/m34-section-pick.md) |
| M35 | [軸組図の割り付けを詰め、図面枠の内側へ並べる（M35）](dev-notes/milestones/m35-section-layout.md) |
| M36 | [横架材の高さごとに伏図を作る（M36）](dev-notes/milestones/m36-plan-per-beam-level.md) |
| M37 | [垂木の断面を一律に指定する（M37）](dev-notes/milestones/m37-rafter-section.md) |
| M38 | [実機確認をローカルの Claude Code から MCP で回す（M38）](dev-notes/milestones/m38-local-mcp-verification.md) |
| M39 | [実機テストは毎周テンプレートから描き、自分の図面は保存せずに閉じる（M39）](dev-notes/milestones/m39-test-from-template.md) |
| M40 | [実機テストの 1 周目を MCP から尋ねずに始める（M40）](dev-notes/milestones/m40-auto-first-round.md) |

表に無い番号（M10 を除く M0〜M16・M18・M20）は[対応表](dev-notes/milestones/README.md)の
要約だけで、決め事として残したもの（M15 / M19 の診断・M18 の用紙の割り付け・M20 の
取り込み設定と部材の端点）は上の「設計の考え方」にあります。

## 書き足すとき

- **新しいマイルストーン**は [対応表](dev-notes/milestones/README.md)に 1 行追加し、経緯を残す
  なら `dev-notes/milestones/m<番号>-<内容>.md` を作って上の表にも 1 行追加する。長くなったら
  フォルダにして `README.md` から読み始められるようにする。
- **設計の決め事**は `dev-notes/design/` に、**入力データの癖**は
  [IFC / STEP 側の癖](dev-notes/ifc-quirks.md)に追加する。
- **SDK の挙動**はここではなく SDK リファレンスの `Findings/` へ（上記）。
