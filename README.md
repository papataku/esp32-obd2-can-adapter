# ESP32 OBD-II CAN Adapter

ESP32を使って、**ELM327互換としても使える高機能CAN/OBD-IIアダプタ**を作るプロジェクトです。

初期ターゲットは **M5Dial (ESP32-S3) + M5Stack Mini CAN Unit U179** です。  
単なるELM327エミュレータではなく、RAW CAN、ISO-TP、OBD-II、UDS、ECU discovery、USB Native protocolを一つのデバイスにまとめます。

> **Current implementation:** `main` is currently the Phase 1 receive-only baseline. See [docs/STATUS.md](docs/STATUS.md) for verified and pending gates.

## 目的

市販のKW905/ELM327系アダプタは手軽ですが、ELM内部処理やBLE/Serial経由で情報が抽象化されるため、以下の用途では制約があります。

- CANフレームを受信時刻に近い形で記録したい
- 複数ECUの通信を同時に観察したい
- ISO-TPの途中状態や失敗原因まで追跡したい
- OBD pollingだけでなく、車両内で周期送信されているRAW CAN信号を利用したい
- Mac上の解析ツールと高速・再現可能な形式で連携したい

本プロジェクトでは、**RAW CANを基準データ**として保持し、その上にISO-TP/OBD-II/UDS/ELM327互換レイヤを載せます。

## 初期ハードウェア

- M5Dial / ESP32-S3
- M5Stack Mini CAN Unit U179
  - CAN transceiver: TJA1051T/3
  - 非絶縁
- M5Dial PORT.A
  - GPIO13: CAN TX
  - GPIO15: CAN RX
  - 5V / GND
- 車両接続
  - OBD-II Pin 6: CAN-H
  - OBD-II Pin 14: CAN-L
  - OBD-II Pin 4/5: GND
  - OBD-II Pin 16: **使用しない**
- 電源
  - M5Dialは車両のACC/IG連動USB 5Vから給電
  - U179もM5Dial側5Vから給電
  - U179のHPWR+は使用しない

> U179にはCAN-H/CAN-L間の120Ω終端抵抗が実装されているため、既存の車載CANへ常設接続する場合は終端構成を実測確認し、不要な終端を残さないこと。詳細は [docs/SAFETY.md](docs/SAFETY.md)。

## アーキテクチャ

```text
Vehicle CAN
    |
    v
U179 / CAN transceiver
    |
    v
ESP32-S3 TWAI
    |
    +--> RAW CAN core --------> USB Native stream / logger
    |
    +--> ISO-TP
           |
           +--> OBD-II
           |
           +--> UDS
           |
           +--> ELM327-compatible interface
```

重要なのは、ELM327互換処理を基準にしないことです。

```text
RAW CAN = source of truth
```

ELM互換処理やデコーダに不具合があっても、元のCANログから後で再解析できる構造を維持します。

詳細: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

## 動作モード

### MONITOR

- 起動時のデフォルト
- TWAI Listen-Only
- 車両CANへ能動送信しない
- RAW CAN観察・記録用

### DIAGNOSTIC

- 明示的な期限付きTX許可(TX Lease)がある場合のみ使用
- 最初は読み取り系OBD-IIのみ
- ISO-TP/UDSは段階的に追加
- Lease失効、ホスト切断、異常時は送信停止

### ELM COMPAT

- KW905/ELM327を利用する既存ソフトとの互換用
- プロジェクト内部ではRAW CAN/ISO-TPレイヤの上に実装
- ELM互換性を優先して内部設計を歪めない

## 安全設計の原則

1. **起動時は必ずListen-Only**
2. **CAN送信はデフォルト禁止**
3. **TXは期限付きLeaseがある間だけ許可**
4. **初期実装では読み取り系診断のみ許可**
5. **任意CAN ID/任意payloadを送れる公開APIを安易に作らない**
6. **BUS-OFFや異常後に診断送信を自動再開しない**
7. **Mac/USBが落ちてもデバイス単体で安全側へ戻る**
8. **各開発Phaseをテストで通してから次へ進む**

詳細: [docs/SAFETY.md](docs/SAFETY.md)

## KW905との位置づけ

KW905は捨てず、**Golden Reference**として利用します。

同一車両・同一要求について、

- response payload
- response latency
- P50 / P95 / P99
- requests/sec
- timeout率
- jitter

を比較し、M5CAN側の互換性と性能を数値で確認します。

最終的な狙いは「KW905より速いELM327」だけではありません。RAW CAN上で必要な信号を発見できた場合は、RPMや車速などの高速データをOBD pollingから外し、CAN周期送信を直接利用します。

## 想定する連携

- Mac: Honda e:HEV Analyzer
  - KW905または本アダプタを選択可能にする
  - RAW CAN / ECU discovery / payload analysis / replay
- 車載メーター
  - 解析で確定したCAN信号定義を利用
  - 高速信号はRAW CAN、低速診断値はOBD/UDSというハイブリッド方式を想定

初期検証車両は Honda STEP WGN e:HEV (RP8) を想定していますが、リポジトリ自体は特定車種専用にしません。

## 開発ロードマップ

[docs/ROADMAP.md](docs/ROADMAP.md) を参照してください。

大きな流れは以下です。

```text
Listen-Only RAW CAN
        ->
USB Native Protocol
        ->
Safe TX Lease
        ->
Read-only OBD-II
        ->
ISO-TP
        ->
UDS read
        ->
ELM327 compatibility
        ->
KW905 Golden Test
        ->
Analyzer / meter integration
```

## 開発者・Coding Agent向け

このリポジトリで作業する前に **[AGENTS.md](AGENTS.md)** を必ず読んでください。

設計上の決定事項は [docs/DECISIONS.md](docs/DECISIONS.md) に残します。  
重要な方針を変更する場合は、コードだけ変更せず関連ドキュメントも更新してください。
