# retrans_fault.csv schema

This file is the deterministic fault input for retransmission review cases.
When `UB_FAULT_ENABLE` is `true`, `UbFault::InitFault(".../fault.csv")`
also attempts to load `.../retrans_fault.csv` from the same case directory.

Columns:

- `ruleId`: stable name for the rule.
- `enabled`: `true` or `false`.
- `taskId`: task id from `traffic.csv`.
- `nodeId`, `portId`: egress port where the packet should be inspected.
- `direction`: `forward`, `reverse`, or `any`.
- `packetType`: `DATA`, `TPACK`, `TPSACK`, `TPNAK`, or `ANY`.
- `psn`: numeric PSN or `any`.
- `lastPacket`: `true`, `false`, or `any`.
- `dropCount`: drop this many matching packets from the first match.
- `delayNs` (optional): non-blocking delay for matching packets, in ns.
- `delayCount` (optional): delay this many matching packets from the first match.
- `comment`: human-readable note.

Packet count is selected by each case to match the document figure:

```text
12288 Byte = 3 * 4096 Byte
16384 Byte = 4 * 4096 Byte
20480 Byte = 5 * 4096 Byte
```
