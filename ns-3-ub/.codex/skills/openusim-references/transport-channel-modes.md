# Transport Channel Modes

<reference-hint>
<use-when>Use this reference when discussing `transport_channel.csv`, especially precomputed transport channels versus on-demand TP generation.</use-when>
<focus>When the file is required, optional, precomputed, or on-demand.</focus>
<keywords>transport_channel.csv, on-demand, precomputed, TP, execution path</keywords>
</reference-hint>

## Hard Facts

- `scratch/ns-3-ub-tools/net_sim_builder.py` writes `transport_channel.csv` when its normal generation flow is used.
- The simulator can still run without a populated `transport_channel.csv`; `UbApp` can generate TP configurations on demand.
- The user-facing default should be `on-demand`; only switch to `precomputed` transport channels when the user clearly asks to pin TP mappings ahead of time.
- A light case-file gate should require `transport_channel.csv` only when the chosen execution path is `precomputed`.

## RTP vs CTP

- `RTP`: TP Channel / TPG semantics. RTP uses TPN, PSN, TPACK/TPNAK/TPSACK, and RTP retransmission.
- `CTP`: Entity/VL semantics. CTP uses CTPH without SrcTPN/DstTPN/PSN. CTP traffic
  does not use RTP TP mappings from `transport_channel.csv`, though case files may
  still include that file for setup or comparison.

## Non-Implications

- Do not claim that `on-demand` transport setup is invalid just because `transport_channel.csv` is absent.
- Do not claim that every runnable case must contain `transport_channel.csv`.
- Do not describe on-demand TP generation as a broken artifact merely because the precomputed file is missing.

## Safe Wording

- A `precomputed` path means the case includes an explicit `transport_channel.csv`.
- An on-demand path may omit `transport_channel.csv`, and that can still be valid.
- The planning default is `on-demand`; do not make users fill in TP details unless they explicitly want to.
- If we switch from `precomputed` transport channels to on-demand TP generation, the case-file expectation for `transport_channel.csv` changes with it.

## Unsafe Wording

- missing `transport_channel.csv` always means the case is broken
- on-demand TP generation still needs `transport_channel.csv` to be valid
- `transport_channel.csv` is always mandatory

## Authority

- `code/doc fact`: `scratch/ns-3-ub-tools/net_sim_builder.py` writes `transport_channel.csv` as part of its normal generation flow
- `code/doc fact`: `scratch/README.md` documents automatic TP generation when `transport_channel.csv` is missing or empty
