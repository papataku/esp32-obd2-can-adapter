# M5Dial owner-live 操作（2026-11-01）

対象は `m5dial_signal_v2_can_29bit_5hz_owner_live` のみです。Owner の選択
「ロックを外して連続取得」により S1/S2 の実車証拠を省略し、S1/S2/S3 staging
なしで既存の連続取得を開始します。[決定と適用範囲](signal-v2-2-29bit-vehicle-runbook.md#2026-11-01-owner-addendum-continuous-5hz)
を参照してください。ビルド成功は実車検証完了を意味しません。

接続前に、既存 runbook の [U179 配線・電源・終端抵抗・非絶縁 GND・バックアップと
app-only 確認](signal-v2-2-29bit-vehicle-runbook.md#1-inventory-wiring-and-protected-backups-offline-no-vehicle)
を実施します。書込み手順の代替ではありません。この作業では実機操作していません。

Mac では停車中に操作し、運転者は端末を操作しないでください。実行前に USB port
と実機・ビルドを照合します。他のシリアルモニターは閉じます。暗号化・bond 済み、
USB 確認により登録済みの V2 central を接続し、STATUS indication / DATA notification
を購読して active session にします。central 側で五 PID（0D/0C/05/5B/9A）を
200ms 周期で要求し、3 秒の lease が切れないよう KeepAlive を継続します。
登録手順は [V2 bench runbook](signal-v2-1-bench-runbook.md) を参照してください。
USB スクリプトは登録・KeepAlive・自動再 ARM を行いません。

```sh
python3 -m venv /tmp/m5can-owner-usb
/tmp/m5can-owner-usb/bin/python3 -m pip install pyserial
# owner が照合済みの実際の port を設定する（下記はプレースホルダー）
PORT='/dev/cu.usbmodem_REPLACE_WITH_VERIFIED_PORT'
/tmp/m5can-owner-usb/bin/python3 tools/owner_live_arm.py --port "$PORT" arm
```

送信文字列は `SIGV2 CAN ARM\n`（115200 baud）。成功 ack は
`#SIGNALV2_CAN,armed=1,reason=USB_VERIFIED,ttl_ms=600000`。
`NOT_READY` は central の準備不足、`STAGE_AUTHORIZATION_REQUIRED` は対象外の
staged lab image です。成功時も demand がなければ診断送信しません。
ARM 後は既存の物理 discovery → GROUP → Fresh 経路で、最大 5Hz の取得になります。
要求周期と実測 Hz は別です。候補は RP8 `{18DAF101}` のみです。

停止は次のコマンドです。送信文字列は `SIGV2 CAN STOP\n`。

```sh
/tmp/m5can-owner-usb/bin/python3 tools/owner_live_arm.py --port "$PORT" stop
```

`ack=REQUESTED` は停止要求のみです。スクリプトは続く `ack=STOPPED` まで待ちます。
8 秒以内に確認できなければ終了コード 1（状態未確認）。LCD と USB 出力を確認し、
既存 runbook の [STOP/abort 手順](signal-v2-2-29bit-vehicle-runbook.md#usb-arm-renewal-stop-and-abort-procedure)
に従ってください。STOP は既に送信済みのフレームを取り消しません。

LCD は既存 5Hz lab の RX/診断/TX rate・エラー・BLE ページを維持します。
中央の最小表示は `CAN TX ARMED/LIVE`（緑：TX 許可中）、`CAN TX STOPPING`
（処理排出中）、`CAN TX STOPPED`（停止）、`STOPPED F:xxxxxxxxxx`
（赤：fault reason の先頭 10 文字、例 `RX_ACCOUNT` / `LOAD_BUDGE`）です。
詳細 reason は USB の `#SIGNALV2_CAN,stopped=1,reason=...` で確認します。
ARMED/LIVE は許可状態であり、線上の送信や Fresh の証明ではありません。
LCD の物理表示確認は未実施です。タッチでは ARM できません。

再起動時は TX 無効、BLE 切断・ARM の 600000ms 期限切れ・session lease/KeepAlive
切れで送信許可が失われ、fault/BUS-OFF/予算・応答検証違反では STOP/Invalid に
なります。BUS-OFF 復帰や再接続で自動 ARM しません。復旧は停車して原因確認後、
明示操作が必要です。**USB スクリプト終了・USB コンソールを閉じるだけでは即停止
しません**。終了後も ARM 期限内かつ central が有効なら TX が継続し得ます。
必ず STOP を明示し、drained ack と停止後 2 秒以上の TX/FC カウンタを確認します。
ログ共有時は BLE address / VIN / 生ログをコミットしないでください。

## 追加・訂正：電源投入だけで開始

Owner の決定（2026-11-01）：「コマンド必要なの？パソコン繋げられないので、刺して電源入れるだけの状態にして」

**以下が owner_live の現行手順です。上記の USB 必須・600秒で失効・再接続では
自動 ARM しない説明を訂正します。** 他の image は変更しません。

1. 接続前に [U179 配線・電源・終端抵抗チェック](signal-v2-2-29bit-vehicle-runbook.md#1-inventory-wiring-and-protected-backups-offline-no-vehicle)を実施します。電源は従来どおり ACC/IG USB 5V、OBD Pin16 は使いません。
2. OBD に接続して電源を入れ、事前登録・bond 済みの SmartRing を接続します。車内で Mac や ARM コマンドは不要です。
3. SmartRing が HELLO/PROFILE を完了し五 PID を要求、3秒 lease の KeepAlive を続けると、有効な接続が2秒継続した後に自動開始します。最大5Hz、従来の物理 discovery と予算制限を保ちます。

LCD は `WAITING`（接続待ち・2秒待機）、`LIVE`（緑：送信許可中）、
`STOPPED F:xxx`（赤：fault、理由の先頭10文字）、`STOPPED (manual)`（手動停止）。
LIVE は実測 Hz や Fresh の証明ではありません。

停止は **画面の任意の場所をタップ**、または電源を抜きます。タップ・USB STOP 後は
電源を入れ直すまで自動再開しません。USB ARM は手動停止だけ解除できます。
切断・lease 切れ・session 終了は STOP/Invalid にし、新しい有効 session が2秒
継続すれば新 epoch で再開します。古い値は Fresh に戻しません。
有効 session 中は600秒で停止しません。

bus error/BUS-OFF、RX欠落・集計、予算、応答検証、TX失敗等は再接続・USB ARM
でも解除できません。`STOPPED F:xxx` になったら停車して原因を確認し、電源を
抜き差ししてください。STOP は送信済みフレームを取り消せません。
実機・車両・LCD表示の確認は未実施です。
