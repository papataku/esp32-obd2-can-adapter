# 10/30 M5Dial ambient RX 専用手順

対象: `m5dial_signal_v2_can_29bit_passive`、M5Dial + U179/TJA1051T/3、500k Classical CAN。
[設計根拠・検証限界](m5dial-passive-design-review.md)。このenvはUSB STATS専用で、Ring/BLE表示・PID取得・ARM・leaseはない。

## オーナー決定 2026-10-31 — M-01残存リスクと車両接続前のLCD目視ゲート

**残存リスクを受容する範囲**：M5GFX 0.2.35 / ESP-IDF 4.4.7のSPI3デバイス登録では、内部処理が途中失敗して登録スロットだけ残留する場合、アプリ側の空きデバイスID差分プローブ（5→4）が「登録成功」と誤判定し得る。これはLCDが動作することを証明しない。DMA確認・`display.init()` の成功扱い・`#PASSIVE_BOOT`正常・`self_check=OK`・CI greenだけでも**LCD動作は証明できない**。現行のID差分プローブは異常を追加で拒否する**fail-closed補助検査**として残すが、完全検出・電気的安全の保証とは扱わない。

**車両/CAN接続前に必ず実施する操作ゲート（オーナー決定）**：

1. 車両/CANから物理的に外し、USB給電の実機M5Dial LCDを人間（オーナー）が直接目視する。表示文字列 **`PASSIVE (TX ISOLATED)`** が実際に読めることを確認し、記録する。シリアルログの表示状態・画面推定値・ビルド成功を代用しない。**表示されない、読めない、`PASSIVE FAULT`、表示不安定・判定不能なら即NO-GO**。LCD動作を推測して車両接続しない。
2. 本runbookのapp-only書込み元イメージSHA、backup/flash gate、同じイメージ・同じ実機のUSB `#PASSIVE_BOOT`全項1・`reassert=0`、`#STATS self_check=OK`の連続性、`#PASSIVE_FAULT`ゼロ、ARM/lease/TX拒否、電源再起動後も同表示・安全状態を満たすことを別途確認する。これらは**目視ゲートに加えて**必要であり代替ではない。
3. 目視ゲートと安全ゲートの両方を満たしても、それだけで実車GOを自動発行しない。実機配線・終端・停車状態等の全条件をPM/オーナーが審査し、対象イメージを明示して車両接続を別途承認するまでは**車両NO-GO**。

**固定条件・再レビュー**：この検証手順とID差分プローブの解釈は**ESP-IDF 4.4.7固定**のもの。ESP-IDFのバージョン（パッチ版を含む）、M5GFX、SPI3初期化/デバイス登録/ロックAPI、LCD配線・表示実装、対象ファームを変更した場合は、その変更に対応するソース・安全検査・ハード検証の**再レビューが必須**。目視による表示確認は上記の残存リスクに対する運用対策であって、GPIO13の瞬間的な電圧やCANバス上の負荷を測定した証拠ではない。

## 机上試験証拠 — 2026-10-31 06:3x JST（オーナー申告、対象head 8ba1243のみ）

- **試験対象**：commit `8ba12439163d7bedb2eef3ec6834b74ae79b52de`（`8ba1243`）。app-only `firmware.bin` **SHA-256 `97c18447915a6f456e9079356d59d591298d44260ae8d0713f50578405b942aa`**。
- **実施条件・書込み**：2026-10-31 **06:3x JST**（分単位の正確な時刻は未提供）、**USB単独・CAN/車両未接続**でオーナーがapp-only書込みを実施。バックアップのSHA照合 `dfe4b932…` 一致（バックアップの完全なハッシュ値はここでは未提供）、`passive_flash_check` **PASS**。whole-chip eraseや車両CAN接続を行ったという証拠ではない。
- **起動時**：`#PASSIVE_BOOT` の全安全項 **1**、`reassert=0` を確認。
- **連続観測**：75秒間、`#STATS` **75行すべて `self_check=OK`**、`checks` **167→907**、記録された `age_ms=34`、`#PASSIVE_FAULT` **0件**。
- **コマンド拒否**：`ARM`、`LEASE`、`ATM5TX1`、`SIGV2 CAN ARM` は**すべて `#REJECT,PASSIVE_IMMUTABLE`**。
- **LCD目視**：オーナーが実機LCDの **`PASSIVE (TX ISOLATED)`** を目視確認。USBを抜き差しして再起動した後も**同じ表示の再出現**を目視確認。
- **証拠の適用範囲**：以上は**8ba1243の実機結果**であり、後続の **`8fec38525bf67e497c6db874b98eb2e13662fd46`（8fec385）や本runbookの文書更新後headの実機試験結果ではない**。その後のheadに同じ書込み・画面目視・連続観測の実機PASSを転用しない。新headのCI greenも8ba1243の机上証拠を置き換えない。**最新headの実車接続は別途同一イメージの実機確認・PM/オーナー承認があるまでNO-GO**。



1. PMが車両/CANから完全に外したUSB机上で、配布KITの `flash-plan.md` に従い本人機器・8MiB容量・full backup二重読み・SHA・既知bootloader/partitionを確認。app-only write/verify。**whole-chip erase、partition/bootloader書換え、PlatformIO uploadは禁止**。異なる構成なら停止。
2. 実機がU179であること、Grove黄=GPIO13→TXD、白=GPIO15←RXD、5V/GNDを確認。P4.10/GPIO39に何も接続しない（JTAG/拡張基板も不可）。OBD16/HPWR+を使用しない。U179の120Ωが余分な終端にならないことを確認。不明ならNO-GO。
3. **車に挿す前にオーナーがM5Dial実機LCDの `PASSIVE (TX ISOLATED)` を毎回目視確認することが必須**。表示なし・視認不能なら他の試験がPASSでも**NO-GO**（2026-10-31オーナー決定、上記参照）。USB115200で自動STATSを60秒保存。`self_check=OK`、`checks`増加、`age_ms<=200`、机上未接続でRX=0を確認。`SIGV2 CAN ARM`、`ARM`、`LEASE 1000`、`ATM5TX1`は全て `#REJECT,PASSIVE_IMMUTABLE`。再起動でも同じ表示/状態を確認。RX=0はno-TX証明ではない。
4. exact SHAの全CI成功・ELF除外検査・配布hashと上記実機確認をowner/PMがレビューして初めてpassive接続可。未確認が一つでもあれば**週末NO-GO**。この変更は旧5Hz/10Hz envやactive運転の許可ではない。
5. 最初は停車・P・駐車ブレーキ状態で受信。USB STATSのみ保存（VIN・位置・BLE識別子・CAN payloadを出力しない）。`rx/rx_s/std/ext`はambient RX、`bus_error/arb_lost/missed/overrun`はdriver counter。`REC/TEC`は診断情報で安全根拠に使わない。外部アナライザーなしでon-wire負荷や電気波形を実測したとは扱わない。
6. `PASSIVE FAULT`、self_check異常/停止/古いSTATS、リセット、連続するbus_error/arb_lost増加、RX欠落増加、車両警告/異常があれば直ちに停車状態を確保しCANを外す。FAULTは再開不可。CAN接続中にreset/flash/ARM操作をせず、机上へ戻して原因レビューする。

実装担当はflash・reset・車両接続を実施していない（2026-10-31の8ba1243 USB机上書込みはオーナー実施）。ホスト試験とbuildは実機UI/配線/受信試験の代わりではない。


## 2026-10-30 startup FAULT diagnostic addendum

On a **USB-only, vehicle/CAN-disconnected bench** record the first 60–75 seconds of 115200 serial. A terminal may attach after reset; unlike the one-shot `#PASSIVE,git` greeting, an initial fault is now repeated each second as `#PASSIVE_FAULT,stage=...,field=...,code=...,` followed by the 12 snapshot booleans. `STATS` also prints the latched first fault. Copy the complete line and corresponding `#STATS` and note whether the U179 was actually present/powered; no CAN ID, payload, VIN or identity is printed. `stage=before_snapshot,field=txd_pad_high` means observed GPIO13 pad LOW; `field=txd_awake` may mean sleep or RTC mux. `stage=display_init` is an LCD init return failure. `stage=initial_check` identifies the first post-start controller/status/route check. Never infer a root cause from `checks=0` alone.

A `#PASSIVE_FAULT` of any stage, or no serial/no screen, is a **NO-GO** and must not be cleared by disabling checks, retrying TX, or attaching a vehicle. Follow the original app-only/backup/hash gates for any owner-led retry. The implementation agent has not flashed or reset hardware.
