# 10/30 M5Dial ambient RX 専用手順

対象: `m5dial_signal_v2_can_29bit_passive`、M5Dial + U179/TJA1051T/3、500k Classical CAN。
[設計根拠・検証限界](m5dial-passive-design-review.md)。このenvはUSB STATS専用で、Ring/BLE表示・PID取得・ARM・leaseはない。

1. PMが車両/CANから完全に外したUSB机上で、配布KITの `flash-plan.md` に従い本人機器・8MiB容量・full backup二重読み・SHA・既知bootloader/partitionを確認。app-only write/verify。**whole-chip erase、partition/bootloader書換え、PlatformIO uploadは禁止**。異なる構成なら停止。
2. 実機がU179であること、Grove黄=GPIO13→TXD、白=GPIO15←RXD、5V/GNDを確認。P4.10/GPIO39に何も接続しない（JTAG/拡張基板も不可）。OBD16/HPWR+を使用しない。U179の120Ωが余分な終端にならないことを確認。不明ならNO-GO。
3. **車に挿す前にM5Dialが `PASSIVE (TX ISOLATED)` と表示することが必須**。USB115200で自動STATSを60秒保存。`self_check=OK`、`checks`増加、`age_ms<=200`、机上未接続でRX=0を確認。`SIGV2 CAN ARM`、`ARM`、`LEASE 1000`、`ATM5TX1`は全て `#REJECT,PASSIVE_IMMUTABLE`。再起動でも同じ表示/状態を確認。RX=0はno-TX証明ではない。
4. exact SHAの全CI成功・ELF除外検査・配布hashと上記実機確認をowner/PMがレビューして初めてpassive接続可。未確認が一つでもあれば**週末NO-GO**。この変更は旧5Hz/10Hz envやactive運転の許可ではない。
5. 最初は停車・P・駐車ブレーキ状態で受信。USB STATSのみ保存（VIN・位置・BLE識別子・CAN payloadを出力しない）。`rx/rx_s/std/ext`はambient RX、`bus_error/arb_lost/missed/overrun`はdriver counter。`REC/TEC`は診断情報で安全根拠に使わない。外部アナライザーなしでon-wire負荷や電気波形を実測したとは扱わない。
6. `PASSIVE FAULT`、self_check異常/停止/古いSTATS、リセット、連続するbus_error/arb_lost増加、RX欠落増加、車両警告/異常があれば直ちに停車状態を確保しCANを外す。FAULTは再開不可。CAN接続中にreset/flash/ARM操作をせず、机上へ戻して原因レビューする。

実装担当はflash・reset・車両接続を実施していない。ホスト試験とbuildは実機UI/配線/受信試験の代わりではない。
