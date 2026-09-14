# CBFC Force-Control Threshold

<reference-hint>
<use-when>Use this reference when tuning CBFC forced credit return, control-frame grain, or pending returned-credit retention.</use-when>
<focus>How to interpret the CBFC forced control-frame threshold as a receiver-side pending-credit retention limit, why the control-packet grain must be considered together with the threshold, and how to choose a repo default that keeps control frames as a late fallback instead of the primary return path.</focus>
<keywords>CBFC, CbfcCtrlCrdRtrThldCell, CbfcRetCellGrainControlPacket, credit return, control frame, Crd_Ack Block, threshold, tuning</keywords>
</reference-hint>

## Contents

- Core Judgment
- What The Threshold Protects
- First-Principles Tuning Model
- Repo Default Rule
- Safe Tuning Rules
- Safe Wording
- Unsafe Wording
- Practical Use

## Core Judgment

`CbfcCtrlCrdRtrThldCell` is not a generic "credit low-watermark".

It is a `receiver-side pending credit return retention limit`:

- local RX has already released credits
- those credits have not yet been returned to the peer TX
- once locally retained pending credit reaches this limit, RX should stop waiting for piggyback and send a control frame

So this threshold answers:

`how much returned credit am I willing to temporarily keep on the receiver before I flush it explicitly?`

Not:

`how little transmit credit can the sender still tolerate?`

## What The Threshold Protects

The relevant tradeoff is:

`piggyback efficiency` vs `credit-return latency`

If the threshold is too large:

- RX keeps too much pending credit locally
- explicit `Crd_Ack Block` is delayed too long
- asymmetric traffic or sparse reverse traffic can make credit return look sticky

If the threshold is too small:

- RX flushes pending credit too eagerly
- control frames are generated more often than necessary
- piggyback opportunities are cut short

## First-Principles Tuning Model

Think in two steps.

Step 1:

decide how much `temporarily retained pending credit` you want to tolerate on the RX side before forcing a flush.

Step 2:

check whether the chosen `control-return burst size` is consistent with that retention budget and with the sender continuity margin.

So the sender-side budget still matters, but only after we first define what RX is allowed to retain.

The important code behavior is:

- trigger condition is `pending >= CbfcCtrlCrdRtrThldCell`
- once triggered, fallback does **not** stop at one control frame
- `MaybeQueueControlReturn()` loops and keeps sending control frames until pending credit drops below one control-credit grain

So the relevant pair is:

- `CbfcCtrlCrdRtrThldCell`
- `CbfcRetCellGrainControlPacket`

not the threshold alone.

Per-frame maximum credit return for one VL is:

`63 * CbfcRetCellGrainControlPacket`

because the control header encodes at most `63` grains per VL.

That means:

- with `control grain = 1`, one frame returns at most `63 cells`
- with `control grain = 32`, one frame returns at most `2016 cells`
- with `control grain = 16`, one frame returns at most `1008 cells`

So a larger threshold can still be reasonable if the control-packet grain is also large enough to make the fallback drain fast once it starts.

## Repo Default Rule

This repo currently uses:

- `CbfcCtrlCrdRtrThldCell = 1024`
- `CbfcRetCellGrainControlPacket = 32`

Interpret that as:

- RX may temporarily accumulate about `1024` cells of already-returnable credit on one VL
- but once fallback is triggered, each control frame may return up to `63 * 32 = 2016` cells for that VL
- so the default behavior is intentionally `late fallback, fast drain`

At the common repo geometry:

- `cell size = 160B`
- `1024 cells = 163840B = 160KB`
- `CbfcInitCreditCell = 6553`, so `1024` is about `15.6%` of one VL's initial credit budget

This default therefore means:

- give piggyback a long runway
- keep explicit control frames rare in steady state
- accept that fallback happens late and may send a short burst of control frames to drain pending credit quickly

The right question is:

- do we want RX to keep this much pending credit locally before flushing?
- once fallback starts, is the configured control grain large enough to return credit quickly?
- is this temporary retention still acceptable for the peer and the workload?

It favors:

- giving piggyback more time
- reducing explicit control-frame traffic
- treating control frames as a true fallback path rather than the normal return path

It does **not** mean "threshold should always be some fixed fraction of initial credit."

Treat `1024/32` as a repo-level strategy choice:

- `large retention budget`
- `large control-return grain`
- `late fallback`
- `fast drain once fallback fires`

## Safe Tuning Rules

Start from these rules:

1. First decide how much pending returned credit RX is allowed to retain temporarily on one VL.
2. Choose `CbfcRetCellGrainControlPacket` together with that retention budget; the larger the threshold, the more important it is that fallback can drain quickly.
3. Then sanity-check that this retained-credit limit is still below a sender-stall-risk region for your link rate, delay, and cell size.
4. If the peer visibly waits too long for returned credits, lower `CbfcCtrlCrdRtrThldCell` or raise `CbfcRetCellGrainControlPacket`.
5. If control frames are too frequent and reverse traffic already provides enough piggyback opportunities, raise the threshold or raise the control-packet grain.
6. Do not tune this parameter from `sender remaining credit` as if it were a sender-side low-watermark.
7. Tune against:
   - link rate
   - link delay
   - cell size
   - reverse-traffic sparsity
   - how much pending credit the RX side is willing to keep before explicit flush
   - how many control frames you are willing to burst once fallback starts
8. Do not tune this parameter as a fixed percentage of `CbfcInitCreditCell` unless that percentage has been justified by actual pending-credit retention intent and then checked against sender continuity.

## Safe Wording

- `CbfcCtrlCrdRtrThldCell` is a receiver-side pending-credit retention threshold.
- It controls how much already-returnable credit RX may temporarily keep before forcing `Crd_Ack Block`.
- `CbfcRetCellGrainControlPacket` controls how much credit one fallback control frame can return per VL.
- Sender-side continuity only provides a safety ceiling; it is not the primary semantic definition.
- In the common repo default setup, `1024/32` is a deliberate `late fallback, fast drain` default.

## Unsafe Wording

- threshold should always be `1/16` of initial credit
- bigger initial credit always means bigger threshold is better
- threshold means "sender still has enough credit left"
- this threshold means the local port itself is out of credits

## Practical Use

When comparing CBFC settings, change this parameter independently from:

- `CbfcInitCreditCell`
- `CbfcRetCellGrainDataPacket`
- `CbfcRetCellGrainControlPacket`

Interpret the result like this:

- if lower threshold improves peer continuity but increases control-frame count, the old threshold allowed RX to retain too much pending credit
- if higher threshold reduces control traffic without hurting peer continuity, the old threshold was flushing too eagerly
- if larger control-packet grain keeps fallback rare and still drains pending credit quickly, the old fallback burst size was too small
- if larger control-packet grain leaves too much residual credit below one grain, the control-packet grain became too coarse for that workload
