# V2 フライトレコーダーの証拠（2026-11-15）

比較の基準はmainの `c3843a6`、比較対象はレコーダーのコードコミット `edf96b0` です。デバイスへの書き込み、シリアルポートの操作、実車でのCAN送信は一切していません。

| ファイル | 内容 |
| --- | --- |
| `run_all_pass_lines.txt` | `tests/run_all.sh` のPASS行と終了コード。Python 3.11で実行（ローカルのPython 3.9は既存テストの `zip(strict=)` に対応していない） |
| `build_matrix.txt` | owner_liveを含む10 envのpio buildの結果とRAM/Flash使用量 |
| `owner_live_function_diff.txt` | owner_liveのELFを関数ごとに比べた結果（`elf_function_diff.py`、正規化した逆アセンブル） |
| `owner_live_gatt_tu_asm_diff.txt` | owner_liveのフラグで `src/signal_v2_gatt.cpp` をGCC `-S` した結果の関数単位の差分（行番号とラベルは正規化） |
| `owner_tick_asm_diff.txt` | 上記のうち `CanOwner::tick` の差分本体 |
| `other_envs_function_diff.txt` | m5dial / 5hz_lab / can_disabled_lab の関数差分。違いはbuild SHA文字列のliteralだけ |

## 要点

- **CanMonitor の関数は1つも変わっていません**（owner_liveの関数差分に `m5can::CanMonitor::` は出てこない）。
- CanOwnerのGCCアセンブリで変わったのは `CanOwner::tick` だけです。変化は2点です。
  - result処理のあとに `recorder::tapResult(incoming, ecu_)`（const参照の読み取り）を1回呼ぶ。
  - レジスタ割り当てが少し変わる。
  - stop/result/s1Tick/pin/probeResult などの命令列は同一です。ELF上で「CHANGED」と出る他のCanOwner関数は、命令数が同じで、ブランチのoffsetとリテラルプール位置（relaxationとalignment）が違うだけです。
- 同じTUでほかに変わったのは、hookを入れた `signal_v2::poll` / `serviceAutoArm` / `manage` と、新設の `tapResult` / `resultTap` だけです。`links` と `golden_G01` が差分に出るのは、直後のsectionに新しいstatic（tap）が入ったことによる解析上の境界の違いで、コードの変化ではありません。
- LOADサイズの変化（owner_live）は次のとおりです。`.iram0.*` は±0で、IRAM/ISRには変化がありません。

  | section | 変化 |
  | --- | --- |
  | `.flash.text` | +15,496 |
  | `.flash.rodata` | +4,124 |
  | `.dram0.data` | +1,952 |
  | `.dram0.bss` | +20,544（RAMページ5枚、ring 2KiB、EvidenceStoreの拡張index、出力バッファ） |

- ほかのenv（m5dial、5hz_lab、10hz_lab、can_disabled_lab ほか）は、build SHA文字列を除いて関数単位で同一です。
- `tools/test_pid_s1_load.py`（#76）は、この差分の中にある命令 `0x1525b6` のデコードに対応しておらず実行できませんでした。代わりに上の関数単位の比較とGCCアセンブリの比較を使っています。

## TXの同一性（`tools/test_v2_flight_recorder.py`）

PID S1の既存の11 owner scenarioすべてで、次の6通りのTX/RXトレースがbyte単位で一致しました。

- base
- head
- head + tapマクロ
- head + 毎tick producer/consumer（storeが正常）
- 同上（storeが失敗し、遅い）
- 同上（カウンタをすべてUINT32_MAXにし、ringを一度も吸い出さない）

このとき、ARM中かつ未latchedでのflash書き込みは0回でした（latched stop後の保存を含め、合計36回のputのうち）。owner本体を書き換えるhookを入れた陽性対照では、トレースが変わることを確認しています。
