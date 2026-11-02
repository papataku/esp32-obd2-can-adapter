# PID expansion slice 1 — budget audit / implementation blocked

先行監査の履歴。PM 2026-11-03判断後の実装契約は [S1 diagnostics](pid-s1-diagnostics-20261103.md) を参照。以下は停止時点の記録。

状態: **監査のみ。PID別カウンタ実装・slice受入は未完了**。実装を禁止された隔離checkerの変更が必要になるため、PMへ選択肢を報告して停止する。

## 固定根拠と範囲

- Adapter base: `3e43dfe440d800b1ec82c17b6749e6e1ca322314`。
- [計画全文、§10 item 1、2026-11-02 owner decisions](https://github.com/papataku/tacotaco-meter-ios/blob/4be832d2d0aac9bc4b650a63145c3b53a7f4efe7/docs/protocol/pid-expansion-plan.md)。指定SHAの全文を `gh api repos/papataku/tacotaco-meter-ios/contents/docs/protocol/pid-expansion-plan.md?ref=4be832d2d0aac9bc4b650a63145c3b53a7f4efe7 -H "Accept: application/vnd.github.raw"` で読んだ。
- O1/O2: SOC/POWERは最初から正式計器。物理的意味・符号は未確認のまま。今回はdecoder/UIを変更しない。
- O3: 5Hz中間gate後、各必須PID >=9.5Hz / p95<=150ms / Fresh>=99%。O4: 実車GO保留。既存owner_live許可から10Hz許可を推定しない。
- Phase 4B / Signal V2-2の監査。Golden、liveAllowed、隔離checker、既存コード・設定・テストは変更しない。

## 監査結果

`include/signal_v2_vehicle_profile.h` と `src/diagnostic_policy.cpp` に既に5 PID `0D/0C/05/5B/9A` のexact groupとessential `0C/0D`がある。重複追加・単独HV要求の追加は不要。組込み`legacy-rp8`、200 bits/frame、5000 bits/100ms、25000 bits/1s、上位CanOwner/下位CanMonitorの二段gateを維持する。

`src/signal_v2_can_budget.h` は予約を100+duration / 1000+duration ms保持する。120ms予約ならfull groupは2400bits、essentialは800bits。5Hzで最大2予約=4800bits/100ms、6予約=14400bits/1s。10Hz連続fullの仮計算は3予約=7200bits/100ms、12予約=28800bits/1sで上限を超える。実スケジューラはin-flight制限とheadroom不足によるessential縮退で先に抑制される。予約値と実測RXを足して二重計上しない。

`src/signal_v2_can_owner.h` の現状:

- `SIGNALV2_LOAD`は予約/chargedピーク、aggregate denied/acquired/invalid/requested_periodを出す。
- `CanSamples::Sample.sequence`は検証済みの新しい完成だけ増える。Snapshot/BLE通知数からCAN取得率を推定しない。
- essential結果では05/5B/9Aをquality=8、observed_us=0で即Invalidにする。aggregate acquiredはessentialでも増えるので、全必須PIDの取得率に転用できない。
- PID別の需要母数、admitted/denied/shed累積値、観測窓の完成率は未実装。今回の要求を満たすにはowner側のread-only instrumentationが必要。
- owner_liveは5Hz、2s auto-ARM、manual STOP、fault latch、sticky unsubscribe STOP、non-owner disconnect規則を保持。wire/BLE/auth/registration/NUSを変更する理由はない。

## 実装を止めた理由

`tools/test_p5_scope.py::check_tree()`は固定P3 main `0424017cb6bdb1446609d7f1318d98fd2a9ef125`の既存ファイルを、P5 guarded additionsを除きバイト完全一致で検査する。さらに `preprocess()` は全既存envでowner/monitorのpreprocessor token完全一致を要求する。

`tools/test_p3_scope.py`もownerを固定baseと比較し、追加ファイルを列挙して制限している。`tests/run_all.sh`はP3/P5を直接実行する。したがってread-only member追加だけでも既存suiteを失敗させる。診断をP5 markerへ隠す、テストを削除/skip、条件を弱める変更は行わない。

一時的な `git archive HEAD` コピー内だけで `CanBudget budget_{};` の直後に `uint32_t pid_demand_diagnostic_=0;` を挿入し、**変更していない** `test_p5_scope.check_tree(temp_root)` を実行した。期待どおり `AssertionError: src/signal_v2_can_owner.h`。本体は無変更。このscope-blocker再現は、未実装のカウンタ/shed mutation proofとは別の証拠。

新規文書もP3の追加ファイルallowlistの対象。このファイルは許可済み `docs/evidence/p5/` に置き、P5/P3隔離制約との衝突の証拠として記録する。

## PMへ報告する選択肢と推奨

1. **推奨**: 診断専用の変更を認める隔離契約の更新を、範囲を限定して明示許可する。旧baseの意味的freezeを残し、計測だけを取り除いたpreprocessor比較とowner_live LOAD差分の独立確認を追加する。新規PID/TX形態、予算、動作変更は許可しない。その後、このsliceのPID別計測を実装する。
2. checkerを固定のまま保つ: 本監査で停止。外部ホストは既存SampleSeqを数えられるが、需要母数・admission時のshed原因の正確な観測には不足するためslice完了とはしない。

10Hz再設計は今の監査だけではexact request追加が必要と確定できない。まず全5 PIDの完成時間/RX上限/FC/denied/shedを計測する。未受信リスクを残した早期予約解除や予算緩和は推奨しない。分割要求/phase調整/確定分精算の比較は別reviewとSTAGE_GATED labだけで行い、owner_liveへ有効化しない。

## 許可後の診断契約案（未実装）

各必須PIDに `demand_due / admitted / completed / shed / denied / failed / cancelled` を持たせる。demand_dueは失敗・busy・budget不足を除外しない周期母数。複数centralの同時需要は既存mergeと同じ最短周期へ集約し、同じCAN完成を二重計数しない。due取りこぼしをtick頻度で隠さない。armed/epoch、需要開始/終了、STOP区間、観測窓の長さを明示する。

- admitted: submit成功時の実際のexact request集合。予約成功だけでTX済みとしない。
- completed: 同epochの検証済みgroupを原子的に採用した時だけ。essentialで05/5B/9Aを増やさない。probeを正式計器の完成に数えない。
- shed: fullを希望しessentialへ縮退する選択時に05/5B/9Aを記録。essential自体の拒否、discovery中、STOP後の不足は別原因。
- 成功率: 観測窓の新規完成数 / 欠測を含む需要母数。実効Hz: 新規完成数 / 観測秒。SampleSeq/Snapshot/通知の重複で増やさない。
- STOP/epochで未完了要求をcancelledへ閉じ、旧epoch結果でcompletedを増やさない。counter overflowを正常0件に戻さない。

必要なmutation proof: completed更新削除、essentialを5 PID成功と誤計数、shed更新削除、需要をsubmit成功時だけに変更、Snapshot再送で完成加算、STOP後の旧epoch完成昇格をそれぞれ一時コピーで壊し、独立assertionが失敗すること。未実装のため今回これらはNOT RUN。

## 検証状態

基準版テスト/owner buildの結果はPRとworkspaceのreport.mdへ記録する。既存の120ms/rolling/FC/overrun/STOP/shed回帰の成功と、新カウンタの受入を混同しない。全env build、実機serial HIL、bench、実車、flashing、CAN接続は今回の停止時点で完了主張しない。実機・serial portにはアクセスしない。
