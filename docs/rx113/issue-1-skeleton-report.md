# Issue #1 Worker report — implementation stopped at ICU compatibility gate

Date: 2026-11-05. Target: ihsinoky/qemu, base `rx113/dev`.
Working branch: `rx113/issue-1-skeleton`.

## Outcome / Draft PR body

**差し戻し。Phase 1 は未実装・未完了。** 調査中に、RX113 の IPR
レジスタ体系を既存 `TYPE_RX_ICU` に表の差し替えと単純なベースアドレス・
ストライドの変更だけでは収容できないことを確認した。
指示書の「モデル側は変更せず」「無理に進めず、差し戻し前提で報告」および
ユーザーの「深追いせず報告書に上げて切り上げ」に従って停止した。
このコミットは調査報告のみであり、新マシンを提供しない。

- 実装済み: なし。MCU・machine・Kconfig・Meson の変更は行っていない。
- スタブ化済み: なし。SCI1/2/5/6/12 を含む全周辺は未実体化。
- 見送り: `rx113.c`、`rx113.h`、machine、ELF ローダ、ICU/CMT/TMR/SCI の
  実体化と配線、未モデル化モジュールのスタブ、起動 smoke。
- RX62N、既存周辺モデル、CPU モデルは無変更。
- push・マージ・GitHub PR 作成は行っていない。push 不可の指示に従い、
  PMO がこの本文を使って `rx113/dev` 向け Draft PR を作成できる形で残す。
  完了扱いの PR にはしないこと。

## 一次資料と設計入力の不整合

指示書・AGENTS.md・project.md・decisions.md・差分表を全文読んだ。
次の不整合は元文書を変更せず、ここに記録する。

1. RX113 の HW マニュアルは **R01UH0448EJ0120 Rev.1.20,
   Sep 07, 2020**。指定された R01UH0365EJ は RX111 用である。
   [ルネサス公式 RX113 HW マニュアル](https://www.renesas.com/en/document/man/rx113-group-user-s-manual-hardware)
   を PDF として取得し、表紙・ICU 章を確認した。RX111 の表は使っていない。
2. 512 KiB のコードフラッシュを 32-bit 空間末尾に整列すると
   `0xFFF80000–0xFFFFFFFF` となる。差分表・指示書の
   `0xFFE00000–0xFFFFFFFF` は 2 MiB であり、512 KiB と両立しない。
   将来の実装では容量から基底を算出する必要がある。
3. 配布された `docs/rx113/rx62n-rx113-diff.md` は77行で、§4.2 の途中に
   literal な `...[truncated 2178 chars]` があり終了している。
   指示書が参照する **§7 の要照合リストはファイルに存在しない**。
   その完全な消化状況は報告できない。下記は取得できた範囲からの照合結果。

取得 PDF の SHA-256:
`a41f74559bd8f8aa36749519da119bcc28dcb40f6a23e6f5d403b9113fd19356`
PDF と `pdftotext -layout` 出力は元ワークスペース直下の
`.issue1-runtime/rx113.pdf` / `rx113.txt` に保持。PDF はコミットしない。

## ICU の阻害要因

出典: 上記マニュアル §14.2.3, p.252（IPRn アドレス範囲）および
**§14.3.1, Table 14.3 (1/7–7/7), pp.266–272**
（vector → IPR、割り込み検出形式）。取得済みなので推測の仮表は不要。
ただし、モデルにそのまま設定すると安全でないためコードへの転記は見送った。

関連する確定値:

| 対象 | ベクタ | IPR番号（10進） | IPRアドレス | 検出形式 |
|---|---|---|---|---|
| SWINT | 27 | 3 | 0x00087303 | Edge |
| CMT0–3 | 28–31 | 4–7 | 0x00087304–0x00087307 | Edge |
| TMR0 | 174–176 | 174（3本で共有） | 0x000873AE | Edge |
| TMR1 | 177–179 | 177（同上） | 0x000873B1 | Edge |
| TMR2 | 180–182 | 180（同上） | 0x000873B4 | Edge |
| TMR3 | 183–185 | 183（同上） | 0x000873B7 | Edge |
| SCI2 | 186–189 | 186（4本で共有） | 0x000873BA | ERI/TEI=Level, RXI/TXI=Edge |
| SCI0 | 214–217 | 214（同上） | 0x000873D6 | 同上 |
| SCI1 | 218–221 | 218（同上） | 0x000873DA | 同上 |
| SCI5 | 222–225 | 222（同上） | 0x000873DE | 同上 |
| SCI6 | 226–229 | 226（同上） | 0x000873E2 | 同上 |
| SCI12 | 238–241 | 238（同上） | 0x000873EE | 同上 |
| SCI12 SCIX0–3 | 242–245 | 242–245 | 0x000873F2–0x000873F5 | Level |

既存実装の制限:

- `include/hw/intc/rx_icu.h:60`: `uint8_t ipr[142]`。
  有効な配列添字は0–141。
- `hw/intc/rx_icu.c:195,253`: IPR の read/write デコードは
  `A_IPR ... A_IPR + 0x8f`（添字0–143）のみ。
  RX113 の IPR174/214/238 等へのゲストアクセスは処理されない。
- 同ファイル `rxicu_level()` と `rxicu_ack_irq()` は
  `icu->ipr[icu->map[n]]` を直接参照する。RX113 の実 IPR 番号を
  `ipr-map` に渡すだけでは、TMR/SCI 割り込みで配列範囲外参照になる。
- 同ファイルの VMState も IPR を142要素として保存する。
  配列長だけ変更すればよい、と判断して共有モデルを改変してはいけない。

配置だけでの吸収を検討した結果、単一のベースアドレス／ストライド変更では
低位 IPR と高位 IPR を同時に保持できない。内部 IPR 番号を圧縮するなら、
各実 IPR の MMIO アドレスも圧縮先へ変換し、共有 IPR と独立 IPR を維持する
追加の MCU 側アダプタが必要になる。この方法の可能性は否定しないが、
単純な配置変更を超える設計・検証となるため、このタイムボックスでは実装しない。
割り込みを無効化したまま skeleton 完了とする回避も行っていない。

PMO/PO への差し戻し事項は、(a) RX113 専用 IPR アドレス変換を MCU 側に
設ける方針、または (b) RX62N 互換性を維持する ICU 拡張を別 Issue にする方針の
選択。既存 RX62N の動作を変えない条件はどちらでも継続する。

## 取得範囲の要照合結果

| 項目 | 調査結果・未了事項 |
|---|---|
| メモリ容量 | 公式 Table 1.3 で R5F51138 の ROM 512KB / RAM 64KB / E2 8KB を確認。コードフラッシュ基底の記載訂正が必要 |
| ICU | 一次資料取得済み。上記が阻害要因。完全な実装用 IPR/level 表は未追加 |
| CMT | 公式 §23.1、§23.2.1–2、Table 5.1 では2ch×2unit。CMSTR0=0x00088000、CMSTR1=0x00088010。既存モデルの0x10ストライドと各unitの+2/+8配置は一致する。実行動作の互換性は未検証 |
| TMR | 既存モデルは **1インスタンスで2ch、IRQは6本**。2インスタンスを0x00088200/0x00088210に置けば4chになる。`rtmr_init()` の出力順は ch0 CMIA/CMIB/OVI → ch1 CMIA/CMIB/OVI で、174–179 / 180–185へ配線可能。差分表のRX62N「unit当たり4本」という説明は現ソースと不一致 |
| SCI | 既存モデルのIRQ列は ERI/RXI/TXI/TEI で指定順と一致。基本レジスタは+0..+7。SCI0のアドレス変更は可能。拡張レジスタと各ch機能の適合、SCI12のSCIXは未検証・未実装 |
| SCI8/9 | 公式 Table 14.3 には割り込み行がある。差分表の実体化対象外という方針を変更していない。実装有無の追加調査は見送り |
| クロック | RX62NのXTAL×4は採用しない。実装再開時は32MHz以下の固定PCLKBを既存モデルのinput-freqへ渡す案。SYSTEMスタブでは動的分周・発振切替を再現できない。コードでの採用値は未設定 |
| CPU/FPU | 既存CPUの型はrx62n、FPU無効化プロパティは確認できなかった。FPSW/FPUを厳密に制限するには別途設計が必要。CPU・FIT初期化の実行検証は未実施 |
| machine/loader | 新マシン名の案は`rx113-r5f51138`。外部SDRAMを持つgdbsimと区別し、MCU型式を明示する。ELFの配置・リセットベクタ・再リセット挙動は未実装 |
| その他周辺 | SYSTEM/DTC/IWDT/DAC/MTU/S12AD/ELC/CAC/DOC/RTC/PORT/MPC/USB0/LCDC/CTSUのスタブは未追加。LCDCの状態モデルは本Issueで実装していない |
| §7 | 元ファイル欠落のため完全照合不能。復元された設計入力で再確認が必要 |

## 検証と引き渡し

- 静的確認: ソースから IPR 配列長と MMIO 上限を抽出し、公式表の
  CMT0=4 / TMR0=174 / SCI0=214 / SCI12=238 と比較。
  CMT0のみ配列・デコード範囲内で、後3者は両方とも範囲外と確認した。
  危険な設定で QEMU を実行する検証は行っていない。
- ビルド: **未実施**。実装を停止し、変更は報告書のみ。
- `-M help` への追加: **未達**。新マシンなし。
- 最小バイナリ起動 smoke / SCI0文字出力: **未実施**。
  再現バイナリも未作成。
- checkpatch: `git diff --cached | scripts/checkpatch.pl --no-signoff -`
  で **0 errors, 2 warnings**。警告は新規文書のSPDX表示とMAINTAINERS更新の
  確認。`git diff --cached --check` も成功。Cソースへの適合確認や
  機能検証の代用にはしない。
- 既存 RX62N/周辺/CPU の差分がないことを確認して引き渡す。
- `/tmp` は使用せず、サブプロジェクトのネットワーク再取得も行っていない。

元ワークスペースの `.git` は読み取り専用で、最初の `git switch -c` が
`index.lock: Read-only file system` で失敗した。このため、元ワークスペースの
`.issue1-worktree/` に `git clone --shared --no-checkout .` でローカル clone を
作り、`origin/rx113/dev` から指定ブランチを作成した。コミットとブランチは
**この clone 内**にある。元リポジトリのブランチは `rx113/dev` のまま。
ローカル clone の `origin` は元ローカルリポジトリを指し、GitHub ではない。
元オブジェクトストアを共有しているため、そのまま別マシンへコピーしないこと。
PMO はこの clone から fetch するか、別途保存した bundle を使用できる。
