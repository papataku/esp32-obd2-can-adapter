# M5CAN Signal V2-2 — 10Hz・複数PID取得の受入計画（設計のみ）

**日付:** 2026-10-27  
**根拠:** オーナー決定（V2標準10Hz目標、SmartRing V2移行、標準5信号）。正本は `docs/v2/m5can-v2-spec-v3.md` の「追補B」。  
**現状:** V2-1はCAN TXをハード無効化したDEMOプロトタイプ。USB/BLE基本ベンチの成功は実車の10Hz達成・ECU対応・CAN許可を意味しない。  
**今回のスコープ:** 計画記述だけ。CAN実装、有効化、書込み、実車送信、他リポジトリ改変は一切なし。

## 1. V2-2の目的と対象

- CAN由来の **新規に完了した診断応答** の時刻で車速 `010D` の実効10Hz（目標周期100ms）を検証する。BLE Notifyの間隔、キャッシュ再通知、同じSampleSeq、Heartbeat、画面描画FPSは達成証拠に使わない。
- V2標準セットは `010D` 速度、`010C` RPM、`0105` WATER、`015B` SOC、`019A` POWER。5表示枠は必須とし、取得不許可・欠測の場合に枠を削除したり0表示したりしない。各信号100msは**希望値**であり、受理済み実効周期を独立に記録する。
- 速度優先度0、RPM主1、WATER/SOC/POWER補助2を既定とする。PID `010D` 以外への優先度0は禁止。
- SmartRingはV2 Centralに移行する目標。2 Centralが同一PIDを要求してもCAN観測は可能な限り共有し、BLE通知はCentral別にする。旧V1の安全後退・登録境界の緩和はしない。
- SOC/POWERは**標準購読・表示枠の対象**だが、車両ごとのECU/PID/物理量/TTL承認とiOS LIVE許可は別。未検証の値はRESEARCH/UNBOUND/INVALID等として明示し、合成DEMOを実車値へ昇格しない。

## 2. V2-2を始めるための独立した安全ゲート

### Gate 0 — 送信権限（未承認、現時点では停止）

1. オーナーの個別許可と具体的な車両・日時・安全条件・実行責任者の決定。10Hz目標の決定自体を送信承認と見なさない。
2. V2-1の完全なUSB/BLE受入、登録、再接続、暗号化、2 Central、MTU23/247、8h soak、失効・復旧、Goldenが独立に合格していること。
3. 既存V1/NUS、USB、V2、ISO-TP FCを含むすべての物理TX入口が単一CAN ownerの認可・停止ゲートを共有し、未承認ではTX=0。
4. 既存NVS/bond/flash/ログ保全、TWAI起動モード、物理配線、監視方法、緊急STOP、独立したCAN観測器を準備。既存の安全設定・レート制限を新機能だけで迂回しない。
5. 実車TX前に机上の合成ECU/隔離CANベンチを通過し、FW新規ビルドと個別再書込み許可を取得する。

Gate 0が未達なら、CAN有効化実装や周期引上げに進まない。

### Gate 1 — PID/ECU/応答対応の調査

- 診断Service 01のsupport bitmap、実際のECU response CAN ID、物理リクエスト宛先を区別して取得元候補を列挙。
- PIDを同一要求に含められるのは**同一の実在する応答ECU**で同一Service、同時要求を受けることが観測できた組だけ。機能アドレス `18DB33F1` 等で複数ECUへ無条件に同報し、得られた値を混合しない。
- ISO 15765-4のService 01要求は単一フレーム時 **最大6 PID**（Service 1B + PID最大6B = 7B ISO-TP single-frame payload）。応答が長くなる場合はISO-TP First/Consecutive/Flow Control、物理送信元ID、全長、タイムアウト、FC予算を検証する。
- groupが未対応、部分返答、negative response、混入ECU、途中欠落、ISO-TP破損、容量不足の場合はそのgroupをfail-closedで無効化し、個別PID・別groupへ切替える場合も**追加予算のAdmission承認**が必要。
- SOC/POWERについては実車での物理的意味・値域・raw形式・動的TTLを独立検証するまではLIVE採用しない。

### Gate 2 — 取得・バス負荷予算（提案値、オーナー再承認待ち）

本段階では以下の**暫定ベンチ受入案**を記録し、実車送信条件として自動的に有効化しない。

| 予算 | 提案上限 | 計測単位 |
|---|---|---|
| 全V2診断トラフィック（自送信+ECU応答+FC+再送）の**増分** | 1秒移動窓でCAN線速度の5%以下 | 実バスのbit count。500kbpsなら25kbit/s相当 |
| 短期バースト | 100ms移動窓で10%以下 | 500kbpsなら5kbit/100ms |
| グループ化要求 | 1要求最大6 PID | 同一ECU・同一Service |
| 物理TX scheduler | 既存CAN TX pacer/owner/権限の範囲内。10Hzに必要な高レートは**別決裁まで禁止** | 単調時計とトークン予算 |
| 同時診断 | 1 CAN ownerが直列化、未完了ISO-TPがあるときはFCも含め予算・ECU分離を厳格に再確認 | 実測ログ |

フレーム長には29bit ID、DLC、CRC、bit stuffing（保守上限）、inter-frame space、FC、エラー再送を含める。平均CAN占有率だけではECU診断負荷は評価できない。既存車載フレームの欠落、遅延、他診断器への影響、ECU応答時間も停止条件に含める。閾値は車種・観測器で検証しPM/オーナー承認で正式化する。

負荷または安全予算に達したら **速度→RPM→補助** の優先度で低優先度の取得から緩和/停止。周期をまとめて「追いつく」burst送信は不可。速度も実効100msを維持できない場合はDelayed表示へ落とし、10Hz合格を主張しない。

### Gate 3 — 10Hzの測定と合否（数値は提案、現車合格なし）

次を同時に満たした場合に限り、**車速に関するV2-2 10Hz合格候補**とする。走行操作・法令遵守・試験環境については別途安全計画を承認する。

- 取得元ECUが承認済みで、診断応答の**ISO-TP完全受理時刻**を単調時計で `ObservedAtUs` に記録。SampleSeqは実際に完了した新規観測時だけ増加。同じ数値でも異なる検証済み診断応答なら新規観測でよい。
- 同一 `BootId/SessionId/SourceGeneration/ModeGeneration/ResponseCANID` に限定して、重複・再配信・Heartbeat・Snapshot replay・DEMO・REPLAY・古いTTL・誤ECUは計測から除外。
- 同時計測で `CAN_completed_at` → `BLE_received_at` → `UI_adopted_at` を別々に記録。BLE Notify 10Hzだけでは未達とする。
- 連続10分×少なくとも3条件（一定速度、変速・加減速相当、安全な停止/EV切替相当）で計測。各条件で車速の実効新規観測率 **毎60秒窓9.5Hz以上**、観測間隔のp50がおおむね100ms（90〜110ms）、p95≦120ms、p99≦150ms、継続中の最大gap≦300msを提案値として記録。
- 車速10Hzと、5信号すべてが100msで新規観測できたかは**別々に判定**。他4信号は必要性・承認ECU・診断予算でRELAXEDにしてもよいが、希望100ms/実効周期/p50,p95,p99/Delayed率/Invalid率を必ず個別に報告。
- 新鮮度の偽装、他ECU値混入、CAN owner違反、未承認送信、NVS破損、Watchdog reset、受入れ不能な他車載通信の阻害は **0件**。1件でも不合格・即時停止。
- センサーのECU内部更新HzはOBD要求回数と独立である。診断応答が10Hzで得られても物理センサーが10Hz更新された証明とはしない。

上記9.5Hz/p95/p99/負荷値は**設計者の提案であり承認済み実車SLAではない**。オーナー/PМが実験前に最終化する。

## 3. Delayed品質と観測期限

- 現行v3のwire Quality列挙には `Delayed` 値がない。**新しいwire値を勝手に割り当てず**、受信/描画側の派生状態 `Delayed` として扱う。新規で完了した観測が受入済みeffective period内に届かない場合、または **speedの100ms目標に対し150ms超の実サンプル間隔/観測年齢**になった場合、当該PIDだけDelayed。
- 実効周期が200msへRELAXEDでも、速度100ms目標を満たしていなければ「10Hz達成」としない。UIで `Requested=100ms` と `Admitted=200ms` を区別する。
- Wireで通知可能な既存品質 `DEADLINE_MISSED=0x08`（観測が要求期限を超えて新規完了した場合）、`STALE=0x07`（TTL失効）は維持する。DelayedはFreshの継続を保証せず、**TTL失効ならSTALE/INVALIDを優先**。未承認SOC/POWERは最初からLIVE=INVALID。
- 末到着のときは新規SampleSeq/ObservedAtUsを作らない。Heartbeatやキャッシュ再送で時計を更新しない。再び有効な**新規**観測が戻った場合のみDelayedから復帰させる。
- Snapshot/lease/revisionのfail-closedを維持する。追加wire Quality値が必要と判断した場合は別提案・別Golden・互換性レビューとする。

## 4. ベンチで実施できる試験（CAN TX未承認の段階）

| 項目 | 方法 | 必須証拠 |
|---|---|---|
| 5件標準profile | 010D/010C/0105/015B/019Aを同一profileに表現し16件構文/486B Snapshot検査 | ACK・全件レコード・Revision |
| 合成10Hz | 単調時刻で100ms刻みの模擬観測、70/100/150/300msやフリーズを注入 | 実観測Hzと遅延判定テスト |
| SmartRing V2 | 二重Central・共有需要、BLE Notify/ACK/Snapshot数をV1比較 | BLE Bytes/s、通知/秒、メモリ、再同期率 |
| 複数PIDパーサ | 6 PID以下の単一フレーム要求と、multi-frame/FC応答のホストfixture | 正常・部分・順序逆・誤ECU・途中切れのfail-closed |
| CAN予算 | フレーム長・bit stuffing上限・FC・リトライを積算するtrace replay | 1秒/100ms窓、しきい値逸脱時STOP |
| 故障注入 | 機能アドレス多ECU競合、group非対応、partial、応答遅延、意図しないburst | CAN owner/TTL/Origin fail-closed |
| Golden回帰 | G01〜G12とmajor/minor、GATT UUID、V1/Log V2 | 既存hex完全不変 |

「机上CANシミュレータでの応答」は**実車ECUがgroup要求を受ける証明ではない**。CAN TX有効化が必要な試験はGate 0の許可後の別工程。

## 5. 実車でのみ検証できる項目

- ECU別Service 01対応bitmap、`010D/010C/0105/015B/019A` の物理応答元・支持状況、29bit CAN IDと実際の宛先、機能アドレスの複数ECU応答衝突。
- **同一ECUに対するグループ要求の実可否**。個別要求とは独立に群ごとの成功率、最長応答時間、ISO-TP FC、機能応答と物理応答の差を記録。
- ECUが意図した10Hzの診断問い合わせと5信号の組み合わせを安全に処理できるか。実CAN busの通常占有率、追加診断占有率、burst、エラー再送、他車載/他診断器への影響。
- M5CAN timestampと独立観測器のクロック一致、長時間clock drift、BLE loss下での遅延、2 Central時の平均/最大遅延。
- SOC=015B、POWER=019Aの物理意味・単位・符号・異常値、実際の電池変化との対応、車両・走行モード別TTL。**iOS LIVE承認はこれらの後に別決定。**
- カメラ/運転中表示などの実安全性検証は別の受入事項。走行試験は安全な監督・承認が必要。

## 6. V2-2判定報告テンプレート

記録項目: 車両/ECU/ルート承認番号、baseline/measurement日時、Firmware SHA、Group組み合わせ（最大6）、要求/応答フレーム、ISO-TP FC数、機能/物理CAN ID、単調取得時刻、SampleSeq、BLE通知受信時刻、端末別受信bytes、iOS採用時刻、60秒窓の新規サンプルHz、Δt p50/p95/p99/max、Delayed/STALE/INVALID率、5信号別admitted period、CPU/heap、CAN増分負荷（100ms/1s）、STOP検知と原因、再実行結果、未合格理由、PM/オーナー承認署名。

**判定ラベル**: NOT_RUN / BENCH_ONLY / VEHICLE_GATED / FAIL / PASS_10HZ_SPEED / PASS_10HZ_FIVE_SIGNALS。最後の2つは混同しない。

**次工程**: 独立安全レビュー → 設計数値のオーナー承認 → Gate 0解除の明示許可 → V2-2別PRの実装・隔離ベンチ → 実車試験（別許可） → 安全・性能評価。現行PR #59や本ドキュメントのコミットだけでは一切のCAN TX権限を付与しない。

### 参照

- v3追補A：`docs/v2/m5can-v2-spec-v3.md`（G01〜G12、Session/Admission/Wireの正本）
- ELM327のマルチPID説明: https://www.manualshelf.com/manual/elm/327/operation-manual-english.html （ISO 15765-4のService 01最大6 PID）
- SAE J1979（ISO 15031-5）の適用範囲、ISO 15765-2／-4の診断転送は実装時に規格原典の当該版と照合する
