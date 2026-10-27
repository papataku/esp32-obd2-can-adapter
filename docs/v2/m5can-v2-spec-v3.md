# M5CAN Signal V2 — V2-0 確定仕様 v3 ＋ V2-1 実装計画

**版：** v3.0  
**日付：** 2026-10-27  
**作成目的：** PMからCodexへの実装引き継ぎ  
**対象：** M5CAN（ESP32-S3 / M5Dial）、SmartRing、TacoTacoMeter iOS  
**現在の承認範囲：** 設計、V2-1の新ブランチ・Draft PRでの実装、MacにUSB接続されたM5DialおよびSmartRingへの実験FW書込み  
**今回の作業：** 設計書作成のみ。コード・commit・PR・FW書込みは行わない。

---

## 0. 結論・PM判断

方式C（購読型SUBSCRIBE＋オンデマンドBATCH）を正式採用する。

V2-0では**Signal V2のwire形式、端末登録、セッション、設定変更、通知、再同期、CAN送信許可の境界をv3.0として固定する**。

V2-1では、物理CAN送信をコンパイル時に禁止した専用実験FWで、M5DialをBLE Peripheral、SmartRingをBLE Centralとして通信プロトコルを検証する。

### 重要な区別

- **仕様固定：** 本書v3で実施する。
- **Golden検算：** G01〜G12の長さ・CRC・期待結果を確認した。
- **実装済み：** まだ該当しない。
- **実機合格：** まだ該当しない。
- **実車CAN送信：** 今回の承認範囲外。

V2-1で使用するサンプルはすべて合成データまたは固定Goldenとし、実車で観測した値と混同しない。

## 1. v2からの変更点

| ID | v3での変更 | 根拠 |
|---|---|---|
| D-09 | V2を優先し、新ブランチ・Draft PR・指定USB実機へのFW書込みを後続作業として許可 | オーナー決定 |
| Q2 | 初回1回の端末登録と、登録済み端末の自動再接続を規定 | オーナー決定 |
| Q2 | 既存bond、V1 wire、NVS設定を保持。端末登録とCAN送信権限を分離 | 安全・互換性 |
| Q1 | SOC/POWERのV2対応を対象に含めるが、TTLと実車承認まではLIVE不可 | オーナー決定 |
| Q1 | 019Aは既存ESP32式に合わせ、負＝充電・正＝放電、A/B未使用とする | 表示互換 |
| Q3 | 実車送信開始条件の決定をV2-2以降へ移管 | オーナー決定 |
| D-04 | 優先度を速度→主→補助→情報パネルに変更 | オーナー決定 |
| D-05 | 性能評価を実測基準とし、iOSの250ms・4件/秒制限を維持 | オーナー決定 |
| Wire | v2のG01〜G12を検算し、フラグメントと負応答を明確化 | 実装前固定 |
| V2-1 | CAN無効FW、登録、購読、Golden再生、二重Central試験を具体化 | Codex引き継ぎ |

### 優先度と性能の原則

iOSの**250ms最小送信間隔・4コマンド/秒**は変更しない。接続世代を変更しても、このホスト側送信予算はリセットしない。

一方、V2のDATAはM5CANからのプッシュ通知であり、iOSが200msごとに新しい診断コマンドを発行する仕組みではない。

この違いにより、iOSからのコマンド数を増やさずに5Hzの通知を受け取る設計は可能である。ただし、**5Hzの新規CAN観測が成立するかは別途ベンチ・実車評価が必要**である。

---

# Part A — V2-0 確定仕様

## 2. 適用範囲と不変条件

### 2.1 対象機能

初期V2で設計・検証する機能：

1. 能力交渉とバージョン検証。
2. 端末の初回登録・再接続・失効・再登録。
3. SUBSCRIBE設定の原子的な適用。
4. ACCEPT / RELAXED / REJECTによるAdmission Control。
5. リースとRENEW。
6. DATAプッシュ通知。
7. Snapshotによる欠落回復。
8. BootId、SessionId、ProfileRevision、取得元世代の検証。
9. BLE二重Central時の独立セッションと公平性。
10. V1/NUS、既存RAWログV2との共存。

### 2.2 V2-1で実施しない機能

- 物理CANへの診断要求送信。
- ECU探索、物理ECUルーティングの実車検証。
- 実車RPM/速度の5Hz取得証明。
- SOC/POWERの実車LIVE承認。
- Service 22の周期購読。
- ディーラー診断との共存試験。
- OTA/flash partition変更。
- 既存NVS、bond、RAWログの消去。
- 実車装着状態でのFW検証。

### 2.3 Fail-closed不変条件

**I-01：** 未登録・未認証CentralによるCAN TXは常に拒否する。

**I-02：** V1・V2・USB Serialを含むすべてのCAN実行経路は共通の認可ゲートを通る。

**I-03：** V2-1ビルドでは、要求内容・登録状態に関係なく物理CAN TXが0件。

**I-04：** セッション・revision・ECU承認世代が異なるDATAはLIVE採用しない。

**I-05：** DEMO、合成Golden、REPLAYをCAN由来のLIVE値に昇格させない。

**I-06：** 接続切断、購読リース失効、認証失効によって、そのCentralの取得需要を無効化する。

**I-07：** 片方のCentralの切断や通信滞留が、もう片方の権限を拡大しない。

**I-08：** ACK喪失・Control再送で設定が二重適用されない。

**I-09：** V2失敗を理由に、V1の許可PID・コマンドレート・送信権限を緩和しない。

**I-10：** 現行FWのflash partition、bond、ログ証跡、設定を無断で変更しない。

---

## 3. V1・既存ログV2との共存

M5CANには既に、実験用RAWログ転送の独立GATTサービスが存在する。

そのため、本書では次の名称を厳密に分ける。

| 機能 | 名前 | 仕様 |
|---|---|---|
| 既存ELM通信 | V1 / NUS | `PROTO=1.1` |
| 既存RAWログ転送 | Log Stream V2 | 既存`b85964xx-...` UUID |
| 新しい購読通信 | Signal Stream V2 | 本書の新UUID |

既存の`PROTO=1.1`は変更しない。

V2対応機では`ATM5CAP`の末尾へ`SIGNALV2=1`を追加する。

ただし`SIGNALV2=1`だけで接続を開始せず、GATTサービス発見・登録済み端末の認証・HELLO ACKが揃って初めて使用する。

既存V1パーサは未知フィールドを許容することを確認しているが、機能追加後に再度Goldenで回帰確認する。

### 3.1 新Signal V2 UUID

以下をv3で固定する。

| 特性 | UUID |
|---|---|
| Service | `f9c17d00-2a3e-4e18-89a2-d7c1b539e000` |
| Control | `f9c17d01-2a3e-4e18-89a2-d7c1b539e000` |
| Data | `f9c17d02-2a3e-4e18-89a2-d7c1b539e000` |
| Status | `f9c17d03-2a3e-4e18-89a2-d7c1b539e000` |

これらは本v3用の提案UUIDであり、既存FWに実装済みという意味ではない。既存Log Stream V2 UUIDとは重複しない。

| 特性 | Properties | 用途 |
|---|---|---|
| Control | Write With Response | HELLO、SET_PROFILE、RENEW等 |
| Data | Notify | DATA、SNAPSHOT、HEARTBEAT |
| Status | Read + Indicate | ACK、設定変更、安全状態 |

Controlは暗号化済み接続からのみ受け付ける。SUBSCRIBE等の実行可能な操作には、さらに端末登録済みであることを要求する。

---

## 4. 端末登録・認証

### 4.1 登録の目的

初回接続時だけ、M5CANに利用を許可する端末を登録する。

登録後は、端末のbond情報と登録情報が保持される限り、毎回の登録操作を要求しない。

ただし、**BLEペアリング済み＝CAN送信許可済み**とは扱わない。

権限は次のように分ける。

| 権限 | 内容 |
|---|---|
| CONNECT | BLE接続・安全な能力照会 |
| SIGNAL_DEMO | 合成DATAの購読 |
| SIGNAL_CAN | 許可済みCAN由来信号の購読 |
| CAN_TX | 共通TXゲートを通じた診断要求の実行 |
| ADMIN | 登録・失効・全体STOP管理 |

V2-1で新規登録した端末に付与するのはCONNECTとSIGNAL_DEMOまでとする。

SIGNAL_CANとCAN_TXはV2-1では付与できない。V2-2以降、別の承認手順で初めて検討する。

### 4.2 初回登録フロー

```text id="707ogj"
未登録SmartRing
    │
    ▼
オーナーがM5Dialで登録モードを有効化
    │
    ▼
90秒間の登録ウィンドウ
    │
    ▼
SmartRingがBLE接続・セキュアペアリング
    │
    ▼
M5CANがbond・相手identityを確認
    │
    ▼
オーナーが登録対象をM5Dial側で承認
    │
    ▼
端末登録情報をNVSへatomic commit
    │
    ▼
REGISTERED
    │
    ▼
HELLO → SET_PROFILE → DEMO DATA
```

初回登録には、**物理的なオーナー操作**を必須とする。

登録モードの開始方法は、M5Dialの操作画面またはUSB接続したMacからのローカル管理操作とする。BLEから未登録端末が遠隔で登録ウィンドウを開くことはできない。

登録ウィンドウは90秒で自動終了する。最大登録端末数は初期4件、同時接続は最大2件とする。

登録時は、利用可能であればLE Secure ConnectionsのPasskeyまたはNumeric Comparisonを利用する。

**注意：** M5Dial・SmartRingの実際の入出力能力とNimBLE-Arduino 1.4.3で認証方式を確認すること。Just Worksによる暗号化bondだけではMITM耐性を証明できない。十分な相互認証を提供できない構成では、物理USBによる本人確認済み登録を追加の必須条件とし、CAN_TX権限は付与しない。

### 4.3 端末識別

登録キーは、BLEの一時的な接続ハンドルや広告アドレスだけで構成しない。

登録情報には次を保存する。

| フィールド | 内容 |
|---|---|
| RecordVersion | 登録形式の版 |
| RegistrationId | ローカル登録ID |
| PeerIdentity | bond解決後の安定したpeer identity |
| BondBinding | セキュリティ情報との対応 |
| AllowedRoles | 許可された役割 |
| PermissionMask | 許可ビット |
| RegistrationEpoch | 登録世代 |
| State | ACTIVE / REVOKED |
| Integrity | CRC・世代検査情報 |

BLE privacyでアドレスが変更されても、解決済みidentityと保存済みbond情報を照合する。

単純なMACアドレス一致だけで認可しない。

### 4.4 NVS保存

新しい専用namespaceを使用し、既存のNVSキーと衝突させない。

推奨namespace：`m5can_sigv2`

保存方式：

- 二重スロットA/B。
- 各スロットにRecordVersion、世代番号、登録テーブル、CRCを保持。
- 非アクティブスロットへ書き込み。
- 書込み・読戻し・整合性検証後、active世代を切り替える。
- 起動時は検証済みの最新世代を採用。
- 両スロット不正なら登録なしとしてfail-closed。

登録情報更新のためにNVS全体を消去しない。

既存bondテーブル、DEMO設定、ログ、証跡、OTA状態を自動削除・再初期化しない。

CRCはNVSデータ破損の検出であり、攻撃者に対する認証の代替ではない。FWが実行時に権限を判断する際は、必ずBLEのセキュリティ状態と登録テーブルを照合する。

### 4.5 通常の再接続

登録済み端末が再接続した場合：

1. BLEリンク確立。
2. bondと暗号化状態を確認。
3. peer identityを解決。
4. ACTIVEな登録情報を照合。
5. 新しいConnectionGenerationを割り当て。
6. HELLOで新しいSessionIdを割り当て。
7. V2購読は未設定から開始。
8. SET_PROFILE・ACK確認後にDEMO通知開始。

以前のSessionId、lease、SampleSeqの採用状態、Snapshot状態を自動復元しない。

**登録状態は保持するが、購読実行状態は保持しない。**

### 4.6 登録失効・再登録

失効には次の2種類を設ける。

**管理者による失効**

- 登録レコードをREVOKEDへ変更する。
- RegistrationEpochを更新。
- 対象Centralの購読を無効化する。
- 該当端末からの新しい実行可能な制御を拒否する。
- ほかの登録済み端末は継続使用できる。
- 失効操作をUSB診断ログに記録する。ただし鍵素材は出力しない。

**セキュリティ上の失効**

bond消失、identity不一致、暗号化喪失、登録情報破損、replayと判断した場合は権限を無効にする。

登録解除しても、既存のbond情報を無条件に消去しない。bondの選択的削除が必要な場合は、対象端末を特定した追加のオーナー操作とする。

再登録時は新しいRegistrationEpochを割り当て、旧世代のセッション・制御要求・購読設定を再利用しない。

### 4.7 登録試験

| テスト | 合格条件 |
|---|---|
| 初回登録 | 物理承認後だけ成功 |
| 登録済み再接続 | 登録操作なしで認証成功 |
| 未登録端末 | 実行可能なSUBSCRIBE拒否 |
| bondのみ存在 | 登録がなければ権限なし |
| 一時アドレス変更 | 正しい解決済みidentityで照合 |
| 登録窓タイムアウト | 90秒後に登録拒否 |
| NVS書込み途中電源断 | 旧または新の整合した状態のみ採用 |
| 1端末失効 | 他端末の登録維持 |
| 登録全件不正 | fail-closed |
| 既存V1設定 | 消去・破損なし |

---

## 5. Wire仕様 — 凍結対象

### 5.1 基本形式

整数はすべてlittle-endian。

論理PDU最大512B、固定ヘッダ26B。

| Offset | Size | フィールド |
|---:|---:|---|
| 0 | 2 | Magic=`4D 35` |
| 2 | 1 | Major=`02` |
| 3 | 1 | Minor=`00` |
| 4 | 1 | Type |
| 5 | 1 | Flags=0 |
| 6 | 1 | HeaderLength=`1A` |
| 7 | 1 | Reserved=0 |
| 8 | 4 | BootId |
| 12 | 4 | SessionId |
| 16 | 4 | Sequence |
| 20 | 2 | PayloadLength |
| 22 | 4 | CRC32 |
| 26 | 可変 | Payload |

PayloadLengthは0〜486。

未知Major、未知Type、予約bit非0、長さ不一致、整数オーバーフロー、CRC不一致は拒否する。

| Type | Value |
|---|---:|
| CTRL_REQ | `01` |
| DATA | `20` |
| SNAPSHOT | `21` |
| HEARTBEAT | `22` |
| CTRL_ACK | `80` |
| STATE_EVENT | `81` |

### 5.2 CRC32

CRC-32/ISO-HDLC。

- Polynomial：`0x04C11DB7`
- Reflected polynomial：`0xEDB88320`
- Init：`0xFFFFFFFF`
- RefIn/RefOut：true/true
- XorOut：`0xFFFFFFFF`
- 格納順：little-endian
- 対象：論理PDU全体
- 計算時：Offset22〜25を0として計算

### 5.3 Fragmentヘッダ

固定16B。

| Offset | Size | フィールド |
|---:|---:|---|
| 0 | 2 | Magic=`D7 52` |
| 2 | 1 | Version=`02` |
| 3 | 1 | Lane |
| 4 | 1 | Flags |
| 5 | 1 | Reserved |
| 6 | 4 | TransferId |
| 10 | 2 | TotalLength |
| 12 | 2 | Offset |
| 14 | 1 | FragmentLength |
| 15 | 1 | Reserved |
| 16 | 可変 | FragmentData |

Lane：Control=`01`、Data=`02`、Status=`03`。

Flags：FIRST=`01`、LAST=`02`。

同一TransferId内のFragmentを順番に再構築し、その後に論理PDU CRCを検証する。

ATT MTU23では、20B Value中16BがFragmentヘッダ、残り4Bがデータとなる。

### 5.4 制御opcode

| OpCode | Value | Body |
|---|---:|---|
| HELLO | `01` | 能力交渉12B |
| SET_PROFILE | `02` | 8+24N B |
| ACCEPT_PLAN | `03` | PlanIdとNewRevision、8B |
| RENEW | `04` | 0B |
| PAUSE | `05` | NewRevision、4B |
| RESUME | `06` | NewRevision、4B |
| SNAPSHOT_REQ | `07` | LastSeenSeq、ExpectedRevision、8B |
| GET_STATUS | `08` | 0B |
| TIME_SYNC | `09` | HostT1、8B |
| STOP_TX | `0A` | 0B |
| ONE_SHOT | `0B` | V2-1ではUNSUPPORTED |

すべてのCTRL_REQは、以下の12B接頭部を持つ。

`RequestId:u32, ExpectedRevision:u32, OpCode:u8, Flags:u8, BodyLength:u16`

SET_PROFILEは16件以下、Subscription Entryは24B固定とする。

### 5.5 ACK

CTRL_ACKはStatus Indicateで送る。

16B接頭部：

`RequestId:u32, OpCode:u8, Result:u8, Flags:u16, AppliedRevision:u32, BodyLength:u16, Reserved:u16`

Result：

| Value | 意味 |
|---|---|
| 00 | ACCEPT |
| 01 | RELAXED |
| 02 | REJECT |
| 03 | BAD_REVISION |
| 04 | BAD_SOURCE |
| 05 | RATE_LIMITED |
| 06 | NOT_AUTHORIZED |
| 07 | TX_LOCKED |
| 08 | BUSY |
| 09 | UNSUPPORTED |
| 0A | PROTOCOL_ERROR |
| 0B | RESULT_UNKNOWN |

GATT Write Responseだけで設定適用成功と判定しない。

### 5.6 RequestIdとrevision

制御要求の同一性：

`ConnectionGeneration + BootId + SessionId + RequestId`

- 同一RequestId・同一内容：保存済み結果を返し、副作用なし。
- 同一RequestId・異なる内容：PROTOCOL_ERROR。
- 古いIDで結果履歴なし：RESULT_UNKNOWN。再実行禁止。
- ExpectedRevision不一致：BAD_REVISION。
- 新接続：旧SessionIdを再利用しない。
- Profile変更はCAN ownerの状態境界で原子的に確定。
- 適用済みrevision以前のDATAは破棄。

1 Centralあたり直近8件のControl結果を保持する。

履歴を超えた古いRequestIdを新規要求として実行しない。

### 5.7 Sessionとlease

- 初期lease：3秒。
- 最大lease：10秒。
- 推奨RENEW：1秒間隔。
- leaseはNVS保存しない。
- 切断時、当該Centralの需要を即時失効。
- 他Centralの有効需要は独立して保持する。
- REGISTEREDであっても、lease期限切れなら新規CAN需要を発生させない。

V2-1ではCANが無効なので、leaseの期限切れを**内部要求予約数・模擬取得数・DATA停止**で検証する。

### 5.8 DATAと品質

DATA Payload：

`RecordCount:u8, Reserved:u8, SignalRecord[]`

SignalRecord：

| Offset | Size | 項目 |
|---:|---:|---|
| 0 | 2 | RecordLength |
| 2 | 1 | RecordType |
| 3 | 1 | OriginFlags |
| 4 | 2 | SubscriptionId |
| 6 | 1 | Service |
| 7 | 1 | Quality |
| 8 | 2 | ParameterId |
| 10 | 4 | ProfileRevision |
| 14 | 4 | SourceGeneration |
| 18 | 4 | ModeGeneration |
| 22 | 4 | ResponseCANID |
| 26 | 8 | ObservedAtUs |
| 34 | 4 | TimeUncertaintyUs |
| 38 | 4 | SampleSeq |
| 42 | 1 | DataLength |
| 43 | 1 | Reserved |
| 44 | 可変 | Raw診断応答 |

V2-1の模擬DATAではOriginFlagsをDEMOまたはREPLAYとする。

**前版のG05にあるCAN由来サンプルはwireデコーダー検証専用の固定fixtureであり、実機V2-1のLIVE出力として使ってはいけない。**

Qualityの主要値：

- `00` VALID
- `01` VALID_ZERO
- `02` NO_DATA
- `03` TIMEOUT
- `04` NEGATIVE_RESPONSE
- `05` PARSE_ERROR
- `06` SOURCE_MISMATCH
- `07` STALE
- `08` DEADLINE_MISSED
- `09` TIMESTAMP_UNCERTAIN
- `0A` TX_LOCKED

NO_DATAやTIMEOUTを0rpm/0km/hへ変換しない。

### 5.9 SNAPSHOT

SNAPSHOT_REQで通常DATAを停止し、旧未送信DATAを破棄する。

SNAPSHOTは現在のキャッシュ・状態を返すものであり、CAN取得を強制実行するものではない。

受信側は以下の順に復旧する。

`RUNNING → RESYNC_REQUIRED → WAIT_ACK → WAIT_SNAPSHOT → VALIDATE → RUNNING`

全体5秒以内・最大2試行で復旧できない場合はINVALID_STOP。

SnapshotのProfileRevision、SessionId、BootId、SourceGeneration、ModeGeneration、ResumeSequenceをすべて照合する。

512Bに収まらないSNAPSHOTを部分成功としない。V2-1では容量内に収まる購読だけAdmissionで受理する。

### 5.10 設定変更中の境界

SET_PROFILE適用時は：

1. 入力と権限をすべて検証。
2. 必要ならRELAXEDを返し、承認前は未適用。
3. コミット時に旧需要を除外。
4. ProfileRevisionを原子的に変更。
5. 旧revisionの未配信DATAを破棄。
6. 新revisionの初回SNAPSHOTへ移行。
7. Statusで適用完了ACKを通知。

ACKが失われた場合、同一RequestId再送で同じ結果を返す。

---

## 6. 優先度・Admission Control

オーナー指定の優先順位を固定する。

| 優先順位 | 分類 | Priority |
|---:|---|---:|
| 1 | 速度 | 0 |
| 2 | 主メーター | 1 |
| 3 | 補助メーター | 2 |
| 4 | 情報パネル | 3 |

同じPIDを複数メーターが要求する場合は、最も高い優先度と、Admissionで受理した最も厳しい周期要求を基準に統合する。

速度を主メーターとして使用する場合も、分類として速度を優先する。

Admission結果：

- ACCEPT：要求周期のまま受理。
- RELAXED：実行可能な周期を提示し、Centralの明示承認を待つ。
- REJECT：安全または資源条件を満たさず拒否。

V2-1はCANが無効なので、CAN実測による受理判定は行わない。**SIMULATED_ADMISSION**として、プロトコル状態機械と各応答を検証する。

模擬でACCEPTした周期を、実車で承認済みの取得周期として保存しない。

---

## 7. SOC・POWERの扱い

### 7.1 初期の扱い

SOC/POWERをV2対象信号に含める。

ただし、V2-1では合成値のデコード・通知のみとし、実車LIVEへの昇格は禁止する。

LIVE承認の必要条件：

1. PID別取得元ECUの承認。
2. 正常応答の形式・長さの検証。
3. ISO-TPの安全な取得。
4. 動的TTLの実機計測。
5. 信号解釈のESP32版との一致。
6. iOSの独立した承認記録。
7. V2側AdmissionとCAN予算の合格。

### 7.2 SOC `015B`

ESP32版に合わせ、表示候補を以下とする。

`SOC[%] = A × 100 / 255`

正常範囲は0〜100%。

欠測・タイムアウト・未承認・TTL切れは`--`表示。

0%は欠測ではなく、有効な観測なら0%として表示する。

### 7.3 POWER `019A`

ESP32 TacoTacoMeterの現行パーサで使用されている電圧・電流の取り出し方を基準とする。

- 電圧：符号なし16bit値÷64
- 電流：符号付き16bit値÷10
- 電力：電圧×電流÷1000（kW）
- 負：充電
- 正：放電

A/Bは現段階では使用しない。

ただし、A/Bの意味を未使用とすることと、異常な生応答を正常データとして受け入れることは別である。必要な応答長、電圧・電流の値域、ISO-TP完全性は引き続き検証する。

`019A`の機能アドレス周期取得は禁止。実車での物理ECU送信はV2-2以降の別承認とする。

### 7.4 TTL

固定値を根拠なくLIVE判定に使用しない。

ESP32側の動的TTL実装を参照し、次を実測する。

- 新規観測間隔のp50/p95/p99
- CAN完成時刻からBLE受信までの遅延
- 通知欠落・BLE再送・キャッシュ再配信
- 停車、EV走行、電源遷移の影響

TTL候補は、実測分布と信号の安全上の意味から決める。期限が確定していないSOC/POWERは、UIが存在してもLIVE値として表示しない。

---

## 8. Golden G01〜G12 確定

### 8.1 検算結果

前版のhexを独立に読み取り、CRC-32/ISO-HDLCを再計算した。

| Golden | 全長 | 宣言Payload | CRC結果 | 判定 |
|---|---:|---:|---|---|
| G01 HELLO | 50B | 24B | 一致 | 正常 |
| G02 HELLO ACK | 58B | 32B | 一致 | 正常 |
| G03 SET_PROFILE | 70B | 44B | 一致 | 正常 |
| G04 SET_PROFILE ACK | 50B | 24B | 一致 | 正常 |
| G05 DATA 0rpm | 76B | 50B | 一致 | 正常 |
| G06 SNAPSHOT_REQ | 46B | 20B | 一致 | 正常 |
| G07 SNAPSHOT | 90B | 64B | 一致 | 正常 |
| G08 HEARTBEAT | 38B | 12B | 一致 | 正常 |
| G09 RENEW | 38B/42B | 12B/16B | 両方一致 | 正常 |
| G10 MTU23 Fragment | 13 Fragment | 50B再構築 | G01 CRC一致 | 正常 |
| G11 BAD_REVISION要求 | 46B | 20B | 一致 | 応答はBAD_REVISION |
| G12 CRC破損DATA | 76B | 50B | **不一致** | 破損を拒否 |

G12の格納CRCは`0x655317C7`、破損後の内容から算出されるCRCは`0x9489126D`である。

G12は**失敗することが正しいGolden**である。

### 8.2 G01 — HELLO要求

```text id="yykulv"
4D 35 02 00 01 00 1A 00 00 00 00 00 00 00 00 00
01 00 00 00 18 00 D8 BB 84 98 01 00 00 00 00 00
00 00 01 00 0C 00 01 02 00 00 00 02 F4 00 40 30
20 10
```

期待：現在セッション未設定状態で受理。端末認証は別途必要。CAN TXなし。

### 8.3 G02 — HELLO ACK

```text id="serd67"
4D 35 02 00 80 00 1A 00 44 33 22 11 D4 C3 B2 A1
01 00 00 00 20 00 90 C1 DD 23 01 00 00 00 01 00
00 00 00 00 00 00 10 00 00 00 00 02 00 02 F4 00
B8 0B 10 00 01 00 01 00 00 00
```

期待：BootId=`0x11223344`、SessionId=`0xA1B2C3D4`。Status Indicate上のACKとして受信。

### 8.4 G03 — SET_PROFILE

```text id="4v2q1t"
4D 35 02 00 01 00 1A 00 44 33 22 11 D4 C3 B2 A1
02 00 00 00 2C 00 09 11 02 F8 02 00 00 00 00 00
00 00 02 00 20 00 01 00 00 00 01 00 00 00 01 00
01 00 0C 00 01 00 01 F1 DA 18 01 00 00 00 C8 00
90 01 58 02 00 00
```

期待：模擬Admissionが許可している場合に限り、Revision=1、RPM200msの購読を原子的に設定。

### 8.5 G04 — SET_PROFILE ACK

```text id="zjch43"
4D 35 02 00 80 00 1A 00 44 33 22 11 D4 C3 B2 A1
02 00 00 00 18 00 8E 2D 3C 72 02 00 00 00 02 00
00 00 01 00 00 00 08 00 00 00 01 00 00 00 C8 00
58 02
```

期待：RequestId=2、ACCEPT、AppliedRevision=1。

### 8.6 G05 — DATA、正常0rpm

```text id="pw3s2v"
4D 35 02 00 20 00 1A 00 44 33 22 11 D4 C3 B2 A1
01 00 00 00 32 00 C7 17 53 65 01 00 30 00 01 01
01 00 01 01 0C 00 01 00 00 00 01 00 00 00 01 00
00 00 01 F1 DA 18 40 42 0F 00 00 00 00 00 E8 03
00 00 01 00 00 00 04 00 41 0C 00 00
```

期待：RAW値として0rpmをデコードできる。

ただし本GoldenのOriginFlagsはCAN RAW由来であるため、V2-1の実機DEMO通知ではそのまま使用しない。V2-1で送信する際はDEMO由来の別fixtureを使用し、CRCも再計算する。

### 8.7 G06 — SNAPSHOT_REQ

```text id="4stv6z"
4D 35 02 00 01 00 1A 00 44 33 22 11 D4 C3 B2 A1
03 00 00 00 14 00 F2 BA 14 65 03 00 00 00 01 00
00 00 07 00 08 00 00 00 00 00 01 00 00 00
```

期待：SNAPSHOT要求を受け付け、現在のrevisionを保持。

不足していたStatus ACKの確定ベクトルを追加する。

```text id="02faov"
4D 35 02 00 80 00 1A 00 44 33 22 11 D4 C3 B2 A1
03 00 00 00 10 00 BB 23 1E 58 03 00 00 00 07 00
00 00 01 00 00 00 00 00 00 00
```

期待：RequestId=3、OpCode=07、ACCEPT、Revision=1。

### 8.8 G07 — SNAPSHOT

```text id="3blzf6"
4D 35 02 00 21 00 1A 00 44 33 22 11 D4 C3 B2 A1
02 00 00 00 40 00 5A 1A 75 C1 03 00 00 00 03 00
00 00 01 00 00 00 01 00 00 00 30 00 01 01 01 00
01 01 0C 00 01 00 00 00 01 00 00 00 01 00 00 00
01 F1 DA 18 40 42 0F 00 00 00 00 00 E8 03 00 00
01 00 00 00 04 00 41 0C 00 00
```

期待：FrameSequence=2、ResumeSequence=3。Snapshot取得だけでSampleSeqや観測時刻を新しくしない。

### 8.9 G08 — HEARTBEAT

```text id="x59t3v"
4D 35 02 00 22 00 1A 00 44 33 22 11 D4 C3 B2 A1
03 00 00 00 0C 00 65 EF B7 92 01 00 00 00 00 00
00 00 01 00 00 00
```

期待：通信状態更新のみ。新しいCAN観測として扱わない。

### 8.10 G09 — RENEWとACK

RENEW：

```text id="4f8t9x"
4D 35 02 00 01 00 1A 00 44 33 22 11 D4 C3 B2 A1
04 00 00 00 0C 00 85 96 53 B2 04 00 00 00 01 00
00 00 04 00 00 00
```

ACK：

```text id="mrfwcv"
4D 35 02 00 80 00 1A 00 44 33 22 11 D4 C3 B2 A1
04 00 00 00 10 00 18 72 BA 0A 04 00 00 00 04 00
00 00 01 00 00 00 00 00 00 00
```

期待：リース延長のみ。ProfileRevisionは増加しない。

### 8.11 G10 — MTU23 Fragment

G01の50B PDUを13個へ分割する。

各Fragmentは16Bのヘッダと最大4Bのデータを含む。

| Fragment | Offset | Length | FIRST | LAST |
|---|---:|---:|---|---|
| 1 | 0 | 4 | Yes | No |
| 2 | 4 | 4 | No | No |
| 3 | 8 | 4 | No | No |
| 4 | 12 | 4 | No | No |
| 5 | 16 | 4 | No | No |
| 6 | 20 | 4 | No | No |
| 7 | 24 | 4 | No | No |
| 8 | 28 | 4 | No | No |
| 9 | 32 | 4 | No | No |
| 10 | 36 | 4 | No | No |
| 11 | 40 | 4 | No | No |
| 12 | 44 | 4 | No | No |
| 13 | 48 | 2 | No | Yes |

Fragment 1：

```text id="gsca4r"
D7 52 02 01 01 00 01 00 00 00 32 00 00 00 04 00
4D 35 02 00
```

Fragment 13：

```text id="6wu0vw"
D7 52 02 01 02 00 01 00 00 00 32 00 30 00 02 00
20 10
```

中間11個は、上表のOffsetからG01の対応する4Bを順番に格納する。Codexは13Fragmentをfixture化し、50Bの完全なG01に一致することを検証する。

FragmentごとにCRC32を検証するのではなく、再構築後の論理PDUでCRCを確認する。

### 8.12 G11 — BAD_REVISION

```text id="tqmdjb"
4D 35 02 00 01 00 1A 00 44 33 22 11 D4 C3 B2 A1
05 00 00 00 14 00 8E EB 27 A4 05 00 00 00 00 00
00 00 02 00 08 00 02 00 00 00 00 00 00 00
```

期待：現在revision=1なのにExpectedRevision=0のため、BAD_REVISION。

対応するACKの確定ベクトル：

```text id="hwd05e"
4D 35 02 00 80 00 1A 00 44 33 22 11 D4 C3 B2 A1
05 00 00 00 10 00 FF 86 10 35 05 00 00 00 02 03
00 00 01 00 00 00 00 00 00 00
```

期待：RequestId=5、OpCode=02、Result=03、AppliedRevision=1、CAN取得予約の増加0。

### 8.13 G12 — CRC破損

```text id="77qfb3"
4D 35 02 00 20 00 1A 00 44 33 22 11 D4 C3 B2 A1
01 00 00 00 32 00 C7 17 53 65 01 00 30 00 01 01
01 00 01 01 0C 00 01 00 00 00 01 00 01 00 01 00
00 00 01 F1 DA 18 40 42 0F 00 00 00 00 00 E8 03
00 00 01 00 00 00 04 00 41 0C 00 00
```

期待：CRC不一致によりPDU全体棄却。

正常値への変換、Fresh採用、キャッシュ更新は0件。

### 8.14 Golden運用規則

- G01〜G12はv3.0の互換性基準として保存。
- 実機Goldenとは別に、ホスト上の純粋なエンコード・デコードテストを持つ。
- テストと実装が同じエンコーダーを呼び出して期待hexを生成してはならない。
- 既知hexを固定fixtureとして保持し、双方を独立に検証する。
- 正常系はエンコード結果と固定hexが完全一致すること。
- 異常系は正しい拒否理由に到達すること。
- 全ケースでCAN TX件数も検査する。
- 仕様変更が必要ならwire minor/majorとGolden更新を正式レビューする。

---

# Part B — V2-1 Codex実装計画

## 9. リポジトリ調査結果

### 9.1 M5CANの基準

対象：

`papataku/esp32-obd2-can-adapter`

固定基準SHA：

`df468a084307fc84bff0a3151b7472e28b674442`

2026-10-27の読み取り確認で、このSHAは取得できた最新コミット一覧の先頭だった。`platformio.ini`の既定環境は引き続き`m5dial`である。

主な既存構成：

- `m5dial_dual_ble_bench`：NimBLE-Arduino 1.4.3、2 Central実験構成。
- `m5dial_dual_ble_canmode_dry_bench`：CAN mode変更のdry-run評価。
- `m5dial_dual_ble_multipid_bench`：複数PID実験。
- `m5dial_v2_authenticated_lab`：既存RAWログV2用の認証実験。
- `m5dial_v2_fresh_flash_lab`：全消去を伴う既存RAWログV2実験。

最後の2つは、今回のSignal V2用ではない。

**V2-1では、`m5dial_v2_fresh_flash_lab`を使用しない。**

同環境は全chip消去・partition変更を前提とするため、オーナーの「既存ペアリング・設定を消さない」という決定と適合しない。

### 9.2 関係する既存ファイル

| ファイル | 現状・用途 |
|---|---|
| `platformio.ini` | build envとcompile flags |
| `src/main.cpp` | 起動、CAN owner、BLE worker |
| `src/ble_elm_transport.cpp` | NimBLE GATT、接続、通知 |
| `src/ble_elm_transport.h` | Central別状態・送信バッファ |
| `src/dual_ble_elm.cpp` | V1セッション・ELM要求 |
| `src/dual_ble_elm.h` | V1セッション構造 |
| `src/can_monitor.cpp` | TWAI初期化、単一CAN owner |
| `src/serial_protocol.cpp` | USB Serialコマンド |
| `src/m5can_log_stream_v2_gatt.h` | **既存ログV2**。改変せず分離する |
| `src/diagnostic_policy.cpp` | CANサービス・ECU許可 |
| `src/diagnostic_request_pacer.h` | 既存50msペーシング |
| `src/app_config.h` | UUID、timeout等の既存設定 |

既存RAWログV2に関するGATTと認可コードは再利用可能な設計上の参考とするが、新Signal V2をログ転送サービスに混ぜない。

### 9.3 GitHub上の関連PR

- [M5CAN PR #56](https://github.com/papataku/esp32-obd2-can-adapter/pull/56)：物理ECUルーティング。
- [M5CAN PR #57](https://github.com/papataku/esp32-obd2-can-adapter/pull/57)：ECU hintと再検証。
- [iOS PR #12](https://github.com/papataku/tacotaco-meter-ios/pull/12)：Phase 10c-1取得スケジューラ。

PR #12は読み取り時点でDraft・openだった。

10c-2は後続統合タスクとして扱う。今回の読み取りでは、10c-2自体の確定済みAPI・PRを確認できなかったため、以下に示すのは**接続すべき設計契約**である。

---

## 10. ブランチ・実装単位

### 10.1 推奨ブランチ

新ブランチ：

`feat/signal-v2-1-ble-prototype`

Draft PRタイトル：

`[Draft] Signal V2-1: authenticated BLE subscription prototype (CAN TX disabled)`

ベースは作業開始直前に再取得したmainとする。

作業開始時に以下を記録する。

- 固定基準SHAとの比較
- 実際のmain SHA
- 変更差分
- PlatformIO環境
- Mac接続実機の識別
- FW書込み前のpartition・NVS保護確認

固定基準と最新mainが異なる場合は、差分を最初にレビューする。

### 10.2 実装順序

| 順序 | 機能 | 完了条件 |
|---:|---|---|
| 1 | Pure wire codec | G01〜G12合格 |
| 2 | Fragment codec | MTU23/247合格 |
| 3 | Session・requestId・revision | 冪等・再接続合格 |
| 4 | 端末登録・NVS | 初回登録・再接続・失効合格 |
| 5 | GATT追加 | Control/Data/Status分離 |
| 6 | SET_PROFILE・Admission | ACCEPT/RELAXED/REJECT |
| 7 | 模擬Signal Store | DEMO/REPLAY専用 |
| 8 | DATA/HEARTBEAT/SNAPSHOT | 欠落回復合格 |
| 9 | 2 Central・流量制御 | 公平性・資源合格 |
| 10 | USB実機・長時間試験 | V2-1 Gate合格 |

CANに関係する実行許可・ルートの構造は安全インターフェースとして準備するが、V2-1で送信有効化しない。

---

## 11. 提案する新規・変更ファイル

### 11.1 M5CAN

| ファイル | 種別 | 責務 |
|---|---|---|
| `include/signal_v2_wire.h` | 新規 | wire定数、型、UUID |
| `src/signal_v2_codec.cpp` | 新規 | PDU、CRC、Fragment |
| `include/signal_v2_codec.h` | 新規 | codec API |
| `src/signal_v2_session.cpp` | 新規 | Session、revision、ACK履歴 |
| `include/signal_v2_session.h` | 新規 | セッション型 |
| `src/signal_v2_registry.cpp` | 新規 | 端末登録・NVS |
| `include/signal_v2_registry.h` | 新規 | 認可・登録API |
| `src/signal_v2_subscription.cpp` | 新規 | SET_PROFILE・lease・Admission |
| `include/signal_v2_subscription.h` | 新規 | 購読管理 |
| `src/signal_v2_simulator.cpp` | 新規 | CANなしの合成サンプル |
| `include/signal_v2_simulator.h` | 新規 | 模擬Signal Source |
| `src/signal_v2_gatt.cpp` | 新規 | Control/Data/Status |
| `include/signal_v2_gatt.h` | 新規 | GATT API |
| `src/signal_v2_flow_control.cpp` | 新規 | Central別通知・mbuf制御 |
| `include/signal_v2_flow_control.h` | 新規 | credit管理 |
| `src/ble_elm_transport.cpp` | 変更 | 新サービス登録・接続イベント |
| `src/ble_elm_transport.h` | 変更 | V2セッション参照 |
| `src/main.cpp` | 変更 | V2初期化・タスク連携 |
| `src/serial_protocol.cpp` | 変更 | 物理オーナー登録・状態照会 |
| `platformio.ini` | 変更 | Signal V2専用env |
| `tests/test_signal_v2_wire.py` | 新規 | 固定hex検証 |
| `tests/test_signal_v2_security.py` | 新規 | 登録・失効 |
| `tests/test_signal_v2_session.py` | 新規 | ACK・revision・lease |
| `tests/test_signal_v2_faults.py` | 新規 | CRC・MTU・欠落・世代 |
| `docs/protocol/signal-v2-wire-v3.md` | 新規 | 本仕様の正本 |
| `docs/qa/signal-v2-1-bench-runbook.md` | 新規 | USB実機検証手順 |

実際のファイル名は既存命名規則と合わせて調整可能だが、責務の統合によって単一CAN owner・セッション境界を曖昧にしてはならない。

### 11.2 CAN TXハード無効化

V2-1専用ビルドフラグを提案する。

```text id="jxlciq"
M5CAN_SIGNAL_V2_EXPERIMENTAL=1
M5CAN_CAN_TX_HARD_DISABLED=1
```

`M5CAN_CAN_MODE_DRY_RUN=1`は既存のdry-run手段として活用してよいが、それだけで完全な送信禁止を証明済みとは扱わない。

V2-1では以下を必須にする。

1. CAN送信入口すべてのリンク・呼出経路を監査。
2. V1、V2、USB Serial、内部診断からの送信を禁止。
3. ISO-TP FC送信も禁止。
4. 実CANインターフェースは原則初期化しないかListen-Onlyに限定。
5. ビルド時にTX有効化フラグと併用するとエラーにする。
6. 試験で物理TXが0件であることを確認。

V1の要求文字列を受け取った場合も、CAN無効FWでは`TX LOCKED`相当を返す。

---

## 12. V2-1専用PlatformIO環境

### 12.1 M5Dial用

提案env：

`m5dial_signal_v2_can_disabled_lab`

継承元：

`m5dial_dual_ble_bench`

追加条件：

- Signal V2有効。
- CAN TX強制無効。
- 既存RAWログ転送V2の起動は必須としない。
- Flash partitionは現行配置を維持。
- whole-chip erase禁止。
- OTA partition migration禁止。
- NVSの一括初期化禁止。
- V1 NUSは通信互換テスト用に保持。
- 端末登録情報のみ専用namespaceに保存。

### 12.2 SmartRing用

SmartRingの既存ファーム正本は`papataku/TacoTacoMeter`にある。

V2-1では、既存の描画・車両取得系を変更するよりも、**V2プロトコル試験専用の最小Central FW**を推奨する。

M5CANリポジトリ内に、独立したSmartRing実験クライアントを置く方式がよい。

提案ディレクトリ：

`test_clients/smartring_signal_v2/`

提案環境：

`smartring_signal_v2_client_lab`

SmartRing側で必要なのは次だけ。

- BLE接続。
- 初回登録のUI。
- 新GATT発見。
- HELLO。
- SET_PROFILE。
- Status ACK表示。
- DATA受信・CRC検証。
- Snapshot要求。
- 接続・切断・RSSI・MTU等の計測。

既存のメーター画面・描画アセット・実車取得パイプラインはV2-1で移植しない。

SmartRing専用ビルドは、対象ボード設定とFlash配置が元のSmartRingプロジェクトと一致していることを確認してから書き込む。

別リポジトリ`papataku/TacoTacoMeter`へ変更・PRを作る場合は、今回のM5CANリポジトリ実装許可とは区別して扱う。

---

## 13. V2-1 BLEシナリオ

### 13.1 初回接続

```text id="149ne8"
M5Dial起動
  ↓
CAN TX disabledをログで確認
  ↓
M5Dialで登録モード開始
  ↓
SmartRing接続・bond
  ↓
オーナーによる初回登録承認
  ↓
登録情報を保存
  ↓
V2 GATT発見
  ↓
Status Indicate / Data Notify有効化
  ↓
HELLO
  ↓
HELLO ACK
  ↓
SET_PROFILE
  ↓
SET_PROFILE ACK
  ↓
DEMO DATA受信
```

### 13.2 通常の再接続

登録済みSmartRingで次を検証する。

- 再登録操作なしで接続。
- 新しいSessionIdを取得。
- 旧SessionIdのDATAを拒否。
- 新しいSET_PROFILEを適用。
- 前セッションのleaseを復元しない。
- 端末登録情報は保持する。

### 13.3 2 Central

M5DialにSmartRingと、iOSテストクライアントまたは別のCentralを接続する。

試験条件：

1. 両者同一PID・同一周期。
2. 両者異なるPID。
3. 両者異なる周期。
4. 片側低MTU。
5. 片側低速接続。
6. 片側切断。
7. 片側lease失効。
8. 片側通知停止。
9. Status Indicate滞留。
10. NUS/既存ログサービスとの競合。

CANが無効なので、「共有取得」は実CANの重複削減ではなく、模擬取得予約数で評価する。

### 13.4 失敗注入

- CRC不一致。
- Fragment欠落。
- 重複Fragment。
- Fragment順序逆転。
- RequestId重複。
- RequestIdの内容変更。
- ExpectedRevision不一致。
- ACK喪失。
- 接続直後の切断。
- BLE接続ハンドル再利用。
- old generation受信。
- CCCD解除。
- 暗号化失効。
- NVS登録情報破損。
- NVS書込み途中の電源断。
- Snapshot欠落。
- Status Indicateの停滞。

いずれも不正な購読・Fresh採用・権限拡大を起こしてはならない。

---

## 14. iOS V2 Adapterと10c-2

### 14.1 iOS側の変更範囲

iOSには独立したSignal V2 Adapterを追加する。

必要な責務：

| 領域 | 内容 |
|---|---|
| Capability | `SIGNALV2=1`確認 |
| Service discovery | 新GATT UUID発見 |
| Security state | bond・登録状態確認 |
| Wire codec | 論理PDU、CRC、Fragment |
| Session manager | BootId、SessionId、ConnectionGeneration |
| Control | HELLO、SET_PROFILE、RENEW、PAUSE |
| ACK matching | RequestId、ExpectedRevision |
| Notification decoder | DATA、Snapshot、Heartbeat |
| Source validation | ECU、来歴、世代、TTL |
| Lifecycle | background停止、再接続 |
| Fallback | 安全なV1への再初期化 |

iOSの既存RequestMachineとBLESessionを置換しない。

V2 Adapterは、新しい接続経路としてこれらの安全状態機械へ接続する。

### 14.2 iOSの250ms・4件/秒

ここは変更禁止とする。

- ホスト起点の取得・制御コマンドは、既存の単調時計による250ms下限と4件/秒予算を破らない。
- 接続世代切替で予算を消さない。
- 不許可要求の予約・取消は、既存の`propose → gatePassed → confirmDispatch`契約を尊重する。
- RENEWは必要に応じて制御予定を予約し、他の送信と競合させない。
- 緊急STOP等、停止側の安全操作については通常の取得要求との別扱いを設計審査する。
- V2 DATAの受信頻度を、iOSの取得コマンド送信回数として数えない。

M5CANが200ms周期を受理した場合でも、200msごとに新しいCAN観測が得られたと推定してはならない。

### 14.3 10c-1との関係

PR #12の10c-1は、信号需要、取得計画、host budget、PIDごとの研究証拠を扱う。

V2-1でこの既存契約を破壊しない。

`AcquisitionScheduler`が保持する250ms・4件/秒の制限を、V2採用を理由に削除しない。

### 14.4 10c-2との関係

10c-2へ渡すべき設計契約は次のとおり。

```text id="iq0clm"
Phase 10 Signal Demand
    ↓
Acquisition Scheduler
    ↓
V1 Adapter / V2 Adapter選択
    ↓
V2 HELLO・Admission・SUBSCRIBE
    ↓
Signal Observation
    ↓
ECU / TTL / Freshnessの最終判定
    ↓
Presentation Snapshot
```

10c-2では以下を扱うことを推奨する。

- V2 Adapterの取得側への結線。
- Admission結果のUI反映。
- 希望周期と実効周期の区別。
- 新規CAN観測Hzの計測。
- V2値のECU承認・TTLの独立検証。
- V1/V2切替時の旧データ無効化。
- SOC/POWERのLIVE承認ゲートとの接続。

**10c-2はM5CAN V2のCAN送信認可を代行しない。**

iOSのLIVE許可とM5CANのCAN送信許可を別の判定に維持する。

---

## 15. 想定ビルド・USB書込みコマンド

以下は**CodexがV2-1実装後に使用するコマンド案**であり、現時点の既存環境でそのまま成功することを保証するものではない。

### 15.1 事前確認

```bash id="y3r164"
git clone git@github.com:papataku/esp32-obd2-can-adapter.git
cd esp32-obd2-can-adapter

git fetch origin
git rev-parse origin/main
git status --short
git diff df468a084307fc84bff0a3151b7472e28b674442 origin/main

pio --version
pio device list
ls /dev/cu.*
```

実際のブランチは最新mainから作成する。デバイスのポートは一覧・USB抜き差しで特定し、推測で選ばない。

### 15.2 M5Dialビルド

```bash id="3p2n3f"
pio run -e m5dial_signal_v2_can_disabled_lab
```

書込み前の必須確認：

- 正しいM5Dialポート。
- CAN TX disabledフラグ有効。
- partition table不変。
- whole-chip eraseなし。
- NVS/OTA/evidenceの保護確認。
- ビルドされたFWのSHA記録。
- USB接続であり車両CANには接続されていない。

### 15.3 M5Dial書込み

```bash id="prgkgb"
pio run \
  -e m5dial_signal_v2_can_disabled_lab \
  --target upload \
  --upload-port /dev/cu.<確認済みM5Dialポート>
```

`erase_flash`、`erase`、partition migrationは実行しない。

既存FWのアプリ領域・partition配置と新FWサイズに不整合がある場合は、書込みを止める。

### 15.4 SmartRingビルド

SmartRing用の独立したPlatformIO設定ファイルを作成した場合の想定：

```bash id="nuz1k0"
pio run \
  -c platformio_smartring_v2_lab.ini \
  -e smartring_signal_v2_client_lab
```

### 15.5 SmartRing書込み

```bash id="wd27qo"
pio run \
  -c platformio_smartring_v2_lab.ini \
  -e smartring_signal_v2_client_lab \
  --target upload \
  --upload-port /dev/cu.<確認済みSmartRingポート>
```

SmartRingのボード設定、flash mode、partition table、PSRAM設定、リセット方法は、`papataku/TacoTacoMeter`の対応envと照合する。

誤ったボード構成のFWをSmartRingへ書き込まない。

### 15.6 ログ

```bash id="bgglrc"
pio device monitor \
  -p /dev/cu.<確認済みM5Dialポート> \
  -b 115200
```

想定するログ項目：

```text id="x70aex"
#SIGNALV2,boot=...,mode=DEMO,can_tx_disabled=1
#SIGNALV2,security=...,registered=...
#SIGNALV2,peer=...,generation=...,mtu=...
#SIGNALV2,hello=accepted,session=...
#SIGNALV2,profile=1,result=ACCEPT
#SIGNALV2,notify=...,dropped=...
#SIGNALV2,lease=expired
#SIGNALV2,can_tx_attempts=...,physical_tx=0
```

鍵、bond secret、Passkey、個人識別可能なpeer identity全文は通常ログへ出さない。

---

## 16. V2-1合否基準

### 16.1 機能

| 項目 | 合格条件 |
|---|---|
| Wire Golden | G01〜G12すべて合格 |
| 追加negative Golden | 不正入力の受理0件 |
| CRC | 破損フレームの採用0件 |
| MTU23 | 正しい再構築 |
| MTU247 | 正しい再構築 |
| HELLO | 適正なSessionId確立 |
| SET_PROFILE | 原子的適用 |
| ACK再送 | 二重適用0件 |
| Revision | 旧revision DATAの採用0件 |
| Snapshot | 正しいResumeSequence |
| Lease | 期限満了後の模擬新規取得0件 |
| 端末登録 | 登録後の自動再接続成功 |
| 失効 | 対象端末の認可停止 |
| 既存bond | 未許可の消去・破損0件 |
| V1 | 既存wire回帰試験合格 |
| CAN TX | 物理送信0件 |

### 16.2 安全

| 項目 | 合格条件 |
|---|---|
| 未登録Central | CAN権限なし |
| bondだけ存在 | CAN権限なし |
| 登録状態不正 | fail-closed |
| SessionId不一致 | 拒否 |
| SourceGeneration不一致 | 不採用 |
| MODE不一致 | 不採用 |
| DEMOのLIVE昇格 | 0件 |
| CRC破損のFresh採用 | 0件 |
| iOS250ms下限違反 | 0件 |
| iOS4件/秒超過 | 0件 |
| 物理CAN TX | 0件 |

### 16.3 性能・資源

| 指標 | V2-1の暫定合格値 |
|---|---|
| 2 Central連続試験 | 8時間 |
| Watchdog reset | 0件 |
| 予期しない再起動 | 0件 |
| NimBLE mbuf枯渇 | 通常試験で0件 |
| ACK未処理滞留 | 無制限増加なし |
| 正常通信時のQueue overflow | 0件 |
| Free internal heap最小 | 32KiB以上 |
| 最大連続internal heap | 16KiB以上 |
| BLE worker stack余裕 | 1.5KiB以上 |
| CAN owner stack余裕 | 1.5KiB以上 |
| メモリの時間依存的な減少 | なし |
| 片側Central滞留で他方が停止 | 0件 |
| 物理CAN送信 | 0件 |

値は合格基準の提案であり、実機で達成済みの数値ではない。満たせない場合は、原因・メモリ表・改善案をレビューし、勝手に閾値を緩和しない。

### 16.4 回帰

V2-1専用FWで少なくとも以下を確認する。

- V1の識別と`ATM5CAP`応答。
- V1の安全なTX拒否。
- 既存bond保持。
- 既存DEMO設定保持。
- 既存ログ・証跡データの保持。
- 既定`m5dial`環境にSignal V2の無断有効化がない。
- 既存Log Stream V2とのUUID衝突がない。
- iPhone/iPadおよびSmartRingの接続・切断でセッション混同がない。

---

## 17. Codexへの作業指示と報告形式

### 17.1 実装時の禁止事項

1. 今回のv3設計を理由に実車CAN送信を有効化しない。
2. `m5dial_v2_fresh_flash_lab`で書き込まない。
3. whole-chip eraseを行わない。
4. Flash partitionを変更しない。
5. 既存bondや設定を消去しない。
6. V1 wireを壊さない。
7. Signal V2と既存Log Stream V2を混合しない。
8. 未登録CentralへCAN送信権限を与えない。
9. SOC/POWERをLIVEへ自動昇格させない。
10. iOSの250ms・4件/秒を緩和しない。
11. 実機の試験結果なしに5Hzを達成したと主張しない。
12. 実車装着・走行試験へ移行しない。

### 17.2 Draft PRに必要な証拠

Codexは少なくとも次を報告する。

**実装証拠**

- Base SHA、HEAD SHA。
- 変更ファイル一覧。
- 仕様と実装の対応表。
- 新規テスト一覧。
- CI結果。
- Golden全件結果。

**FW証拠**

- M5DialとSmartRingのビルドenv。
- FWバイナリSHA。
- 使用したUSBポートの識別方法。
- 書込み前のpartition照合結果。
- 書込み成功/失敗。
- 起動ログ。
- CAN TX disabledの証明。

**BLE証拠**

- 初回登録。
- 登録済み再接続。
- 端末失効と再登録。
- HELLO/ACK。
- SET_PROFILE/ACK。
- DATA/Heartbeat。
- Snapshot復旧。
- MTU23/247。
- 2 Central同時接続。
- BLE流量制御とメモリ計測。
- 8時間試験。

**安全証拠**

- V1経由CAN TX拒否。
- V2経由CAN TX拒否。
- USB Serial経由CAN TX拒否。
- ISO-TP FC経路の送信禁止。
- 未登録端末の送信拒否。
- 旧世代・破損DATAのFresh採用0件。
- NVS・bond・既存ログ非破壊。

### 17.3 推奨コミット分割

| Commit | 内容 |
|---|---|
| C1 | V2-0 wire codecとGolden |
| C2 | Fragment、session、ACK、revision |
| C3 | 端末登録・NVS・認証 |
| C4 | GATT Control/Data/Status |
| C5 | Subscription、DEMO Source、Snapshot |
| C6 | 2 Central、流量制御、fault test |
| C7 | SmartRing試験クライアント、runbook |
| C8 | 実機検証結果・修正 |

Draft PRはレビュー用とし、PM・オーナーの最終承認なしにmainへマージしない。

---

## 18. 残余リスク

V2-0仕様を固定しても、以下の点は未実証である。

| リスク | 対応 |
|---|---|
| SmartRingとM5Dialの実際のLE Secure Connections対応 | V2-1実機で確認。MITMを推定しない |
| 二重Centralの安定性 | 8時間試験、mbuf・接続間隔・RSSI計測 |
| NVS更新時の電源断耐性 | fault注入・読戻し |
| 既存bondとの共存 | 未登録bondからの権限迂回試験 |
| V1経路の共通認可漏れ | CAN送信入口の静的・動的監査 |
| NimBLE内部mbufへの依存 | ライブラリ版固定、枯渇試験 |
| ATT MTU差と通知遅延 | MTU23/247両方で試験 |
| 2 Centralでの通知公平性 | 片側低速・切断・CCCD解除試験 |
| GATT Indication滞留 | 有限タイムアウトで停止 |
| V2-1の実行性能 | RAM・stack・CPU・8時間試験で確認 |
| 実CAN診断負荷 | V2-2以降の独立CANベンチで確認 |
| PID別物理ECUルート | PR #56/#57の安全条件を別途検証 |
| SOC/POWERの観測HzとTTL | V2-2以降の実機計測 |
| iOSの10c-2 API | 既存10c-1との整合レビュー後に確定 |

また、今回のV2-0で固定したwireが、将来の3〜6 PIDまとめ取りやService 22を無条件に許可するものではない。それらには別の能力bit、許可マニフェスト、診断予算、Golden、実車承認を必要とする。

---

## 19. PMへの最終引き継ぎ

| 判断 | 結論 |
|---|---|
| 方式C | 採用 |
| V2-0 wire | v3.0として固定 |
| G01〜G12 | バイト長・CRC検算済み |
| 端末登録 | 初回物理承認、以降自動再接続 |
| 既存ペアリング | 保持 |
| V1 wire | 保持 |
| V2-1 CAN | コンパイル時に無効 |
| M5Dial実験FW | オーナー承認範囲内 |
| SmartRing実験FW | オーナー承認範囲内 |
| SOC/POWER | 対応準備、TTL・信号承認までLIVE禁止 |
| iOS250ms・4件/秒 | 変更禁止 |
| 優先度 | 速度→主→補助→情報パネル |
| 5Hz | 実測で受入判断 |
| 実車CAN送信 | 未承認 |
| 次工程 | **V2-1実装・Golden・USB実機ベンチ** |

**Codexへの最終指示：** 本v3を設計基準として、`papataku/esp32-obd2-can-adapter`の最新mainから新ブランチを作成し、物理CAN TXをコンパイル時に無効化したSignal V2 BLEプロトタイプをDraft PRで実装する。Golden、端末登録、二重Central、NVS非破壊、リース、Snapshotを確認し、オーナーが許可したMac USB接続済みM5Dial/SmartRingだけで実機検証する。実車CAN送信、全flash消去、partition移行、mainへの自動マージは行わない。

### 参照

- [M5CAN 固定基準コミット df468a084](https://github.com/papataku/esp32-obd2-can-adapter/commit/df468a084307fc84bff0a3151b7472e28b674442)
- [M5CAN V1通信仕様](https://github.com/papataku/esp32-obd2-can-adapter/blob/main/docs/M5CAN_PROTOCOL_V1.md)
- [M5CAN PR #56](https://github.com/papataku/esp32-obd2-can-adapter/pull/56)
- [M5CAN PR #57](https://github.com/papataku/esp32-obd2-can-adapter/pull/57)
- [TacoTacoMeter iOS PR #12（10c-1）](https://github.com/papataku/tacotaco-meter-ios/pull/12)
- [ESP32 TacoTacoMeter](https://github.com/papataku/TacoTacoMeter)
- 独立レビュー：`ttm-v2-review-codex.md` / `ttm-v2-review-ble.md` / `ttm-v2-review-safety.md`

**本書のステータス：** PMによるV2-0仕様凍結候補。実装・実機の合格を証明する文書ではなく、V2-1の実装と受入を拘束する設計基準である。