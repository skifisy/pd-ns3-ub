# RTP retransmission review case

本目录只把一个代表性重传验证 case 纳入本次 MR：

```text
scratch/retrans/SELE_fast/non_first_tp_packet_loss_markpsn/
```

选择这个 case 的原因是它覆盖本轮改动里最有区分度的路径：

- `RetransmissionMode=SELECTIVE`
- `EnableFastSelectiveRetrans=true`
- `EnableSelectiveMarkPsn=true`
- DATA PSN 1 原包和第一次重传均被确定性丢弃
- TPACK PSN 0 被延迟，让 TPSACK 先驱动快速选择性重传

其余本地 sweep case 和流程图留在工作区用于人工对照，但不进入本次 MR，避免把批量验证产物和核心代码改动绑在一起。

运行方式：

```bash
./ns3 run 'scratch/ub-quick-example --case-path=scratch/retrans/SELE_fast/non_first_tp_packet_loss_markpsn'
```
