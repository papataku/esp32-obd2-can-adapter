# Signal V2 フライトレコーダー（読み出し手順）

PM決定 2026-11-15。対象は env `m5dial_signal_v2_can_29bit_5hz_owner_live` だけです（`-DM5CAN_SIGNAL_V2_FLIGHT_RECORDER=1`）。ほかの env には入っていません。

## 1. 何をするものか

- 実車で `STOPPED F:<理由>` が出た走行の証拠を、M5Dial本体のflash（既存の `spiffs` EvidenceStore）に残します。あとからUSBで読み出せます。
- **CAN送信は変えていません**。PID・周期・deadline・budget・ECU選定・停止条件はそのままです（理由は `tools/test_v2_flight_recorder.py` を参照）。
- 記録しないもの：BLEアドレス/MAC、VIN、位置情報、鍵。BLEリンクは「起動ごとのslot番号・generation番号・接続handle」だけで区別します。

## 2. いつflashへ書くか（重要）

TWAI割り込みはIRAMに置かれていません（`CONFIG_TWAI_ISR_IN_IRAM` 無効）。このため、CAN稼働中にflashへ書くと受信が取りこぼされ、`RX_ACCOUNTING_OR_BUS_FAULT` や `QUERY_ADMISSION_FAILED` を**レコーダー自身が起こす**おそれがあります。そこで書き込みは次の2回に限ります。

| タイミング | 内容 |
| --- | --- |
| 起動直後（TWAI/BLEの開始より前） | BOOTページ1枚（git SHA、リセット理由、boot番号） |
| 停止理由が確定した（latched）stop() のあと、処理中のCAN要求がなくなってから | RAMに貯めた記録を数ページ。既存のCAN-idleチェックポイントの内側で書く |

- ARM中・LIVE中・未ARMでTWAIが動いている間は**一切書きません**。RAMに貯めるだけです。
- AUTO_ARMビルドでは、latched stopのあと電源を切るまで再ARMできません（`grantCanArm` がstopReasonで拒否）。そのため書き込み中に再ARMされることはありません。
- 書き込みは1ブートあたり最大12回です。停止後に追加の記録があれば、前の書き込みから60秒以上あけて再度書きます。
- flashへの書き込みに1回でも失敗したら、そのブートの間は永続化を止めます。CANには影響しません。
- **画面タップや `SIGV2 CAN STOP` による手動停止と、走行中の電源OFF（ACC OFF）ではflashへ書きません。** この場合に残るのはBOOTページだけで、走行中の記録はRAMごと失われます（制限事項）。

## 3. 読み出し方法

### A. USB（推奨：走行後、M5DialをMacにつなぐ）

USB CDC（115200）で次のコマンドを1行ずつ送ります。シリアルポートの開き方は `tools/owner_live_arm.py` と同じ注意（DTR/RTSで再起動させない）に従ってください。

| コマンド | 内容 | 条件 |
| --- | --- | --- |
| `V2REC STATUS` | 保存状況とRAM上の最新累積値（`#V2REC_STATUS` / `#V2REC_LIVE`） | いつでも可（RAMを読むだけ） |
| `V2REC DUMP` | flashに残る全ページを古い順に表示し、続けてまだ保存していないRAMページも表示 | **CANがARMされていない時だけ**（ARM中は `refused=CAN_ARMED`。途中でARMされたら中断） |
| `V2REC DUMP HEX` | 上記に加えて、各ページの生バイト（`#V2REC_HEX`） | 同上 |

- Ringは接続したままで構いません。停止後（latched）または未ARMなら、DUMPはflashを読むだけです。
- `V2REC CLEAR` はありません。EvidenceStoreには削除操作がなく、消すにはflashへ書く必要があるためです。古いページはローテーションで自然に上書きされます。
- オフラインで再解析するときは、`V2REC DUMP HEX` のログを保存して `python3 tools/decode_v2_flight_recorder.py --text log.txt` に渡します。

### B. flashダンプから（PMが行う）

フルバックアップ（`esptool.py read_flash 0 0x800000 full.bin`）またはspiffs領域だけ（`read_flash 0x670000 0x180000 spiffs.bin`）から変換します。

```
python3 tools/decode_v2_flight_recorder.py full.bin > rec.json
python3 tools/decode_v2_flight_recorder.py spiffs.bin --csv summaries.csv > rec.json
```

- 8MiBのファイルは、自動的に0x670000から読みます。それ以外のオフセットは `--offset` で指定します。
- 所有ヘッダのない領域（他者のデータ）は拒否します。読み取り専用で、デバイスには触れません。
- JSONには、ブートごとの記録、`stopped`（停止理由）、`nodata_per_minute`（1分ごとのNoData件数）が入ります。

## 4. 記録の種類とフィールド

| type | タイミング | 主なフィールド |
| --- | --- | --- |
| BOOT | 起動時 | boot番号、reset（esp_reset_reason）、rom_reset、abnormal_streak、git SHA |
| EVENT | 発生時すぐ | ARM/DISARM（epoch）、MANUAL_STOP、ECU_PIN/UNPIN（応答ID）、ECU_STATE（a8=状態, a16=consecutive_bad）、LINK_UP/DOWN/SUB/UNSUB（slot, generation）、AUTH_END（SESSION_ENDED / USB_ARM_EXPIRED / OPERATOR_STOP / CONTROL_FIFO_OVERFLOW / CENTRAL_UNSUBSCRIBE / AUTO_ARM_GRANTED）、STOP_IDLE、RING_DROP、STORE_FAIL |
| STOP | latched stopの瞬間 | **停止理由の全文**、uptime、epoch、状態flags、ECU応答ID、取得数/無効化数、PIDごとの最終成功からの経過ms、最後のDiagnosticResult（status/応答数/RAW数）、ECU状態とconsecutive_bad、NoData連続数/最大、CanMonitor lastError |
| SUMMARY | ARM中またはリンク中は10秒ごと（停止後は変化があった時だけ60秒ごと） | PID（0D,0C,05,5B,9A）ごとの demand/admitted/completed/failed/shed/denied/cancelled（PID S1カウンタの差分）、成功サンプル数、**成功サンプル間の最大間隔ms**、取得/無効化（NoData=q3、検証失敗=q5、停止=q6/10、その他）、ECU状態、NoData連続の最大 |
| SUMMARY2 | SUMMARYと対で出る | CanStatsの差分（TX試行/成功、query/ok/timeout/no_data/failed、V2 RX、FC、app queue drops、NRC）、result_status_count[]の差分、累積の rx_missed/rx_overrun/bus_error、TWAI状態、fault_locked、Ringへ送ったDATA/SNAPSHOT数、空SNAPSHOT数、SNAPSHOTのレコード数、op4（lease更新）/op7（snapshot要求）の受理/拒否、最後の拒否op/コード、**GATTが破棄した送信待ちPDU**（transition=取得や無効化に伴う破棄、stale=STALE_OR_INVALID、control=制御受理時、expired=lease切れ）、応答遅延ヒストグラム（<10/<20/<30/<50/<80/<120/≥120ms/応答なし）、マルチフレーム数、最後の結果のSF/FF/CF数とFF/CFのオフセットµs |
| LIVE | 1秒ごと（RAM上の最新値。停止時に1件だけ保存） | 起動からの累積値 |
| BLE | GAPイベント時 | CONNECT/DISCONNECT/REJECT/ERROR、接続handle、generation、切断理由コード、MTU、接続interval/latency/supervision |
| TEXT | CanMonitor lastError が変わった時 | エラー文字列（CAN ownerが同時に書き換えると文字化けすることがある） |

flags（SUMMARY/STOP/LIVE）のビット：armed, stopped, busy, manual, pinned, link0, link1, subscribed。

## 5. 保持量の上限

- flashには**直近48ページ**が残ります（1ページ ≦ 2304バイト）。
- 1ブートで書くのは、BOOTページ1枚と、停止時の最大11ページです。最悪でも直近4ブート、停止のない普通の走行なら約48ブート分が残ります。
- 停止時に残るRAM上の記録は、起動直後のページ（約1分半）、停止直前の約5〜6分（4ページ）、累積値（LIVE）です。停止前にRAMが一杯になると古いページを捨て、`flags.overflow` と `dropped_before` に記録します。停止後は保存前のページを捨てません。
- 異常リセット（panic/WDT/brownout）が8回続くと、それ以降のBOOTページを書きません。電源投入リセットで再開します。

## 6. flash摩耗の見積もり

- 書き込み先は、空のセクタか、自分（V2Rec）のセクタだけです（PM指示：自分以外のセクタは消去しない）。旧ビルドのBLE記録などは消しません。
- 1回の書き込みで消去するのは1セクタ（4KiB）です。spiffsは382データセクタあり、そのうち回せるのは約300セクタです。
- 1日10ブート・各ブート最大12回書き込むと仮定すると、1セクタの消去は約2.5日に1回です。保証値の10万回に達するまで数百年かかります。
- 最悪の例として、電源投入リセットが3秒ごとに続く場合（リミッタの対象外）は約3年で10万回に達します。

## 7. 未確認・制限

- 実機でのビルド後の動作（ヒープの空き、書き込み時間、DUMPの出力）は**未確認**です。デバイスにはまだ書き込んでいません。静的RAMは約22KB増えています（165,060 → 187,556バイト）。
- 手動停止や走行中の電源OFFでは、走行の記録は残りません（2章）。
- 停止後の書き込みでCAN受信のoverrunが起き、CanMonitorがfault lockになる可能性があります。V2のTXはすでに止まっているので影響しませんが、同じブート中にUSB ELMで問い合わせると拒否されます。電源を入れ直せば戻ります。
- LCDへの停止理由の全文表示（PM任意項目）は、この版では実装していません。
