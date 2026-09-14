---
name: openusim-capture-insights
description: Use when a verified OpenUSim root cause, reusable insight, or stable interpretation rule should become a knowledge card.
---

# OpenUSim Capture Insights

## Overview

Capture only the conclusions that are worth reusing later.
This skill writes or updates knowledge cards under `../openusim-references/` only after the conclusion is stable enough and the user explicitly agrees.

<HARD-GATE>
Do not create or modify a knowledge card unless the user has clearly agreed in the current conversation.

Do not write a card for:
- a live hypothesis
- a case-specific note with no broader reuse
- a duplicate of an existing card
- a narrative debugging timeline
</HARD-GATE>

## When to Use

- The investigation has reached a verified root cause, not just a suspicion.
- The work produced a reusable simulation insight that should help future runs.
- The work produced a stable interpretation rule, such as what some metric does or does not mean.
- The user explicitly asks to turn the conclusion into a knowledge card.

Do not use this skill for ordinary run summaries, one-off notes, or unfinished analysis.

## The Process

Completion criterion: the user-approved insight is either written into a non-duplicate knowledge card or rejected as not stable/reusable enough, with the reason reported.

1. Check whether the conclusion is stable, reusable, and worth preserving.
2. Scan existing cards in `../openusim-references/` through their `<reference-hint>` blocks first.
3. Decide whether to update an existing card or create a new one.
4. Ask the user for permission in one short sentence before editing any knowledge card.
5. After the user agrees, write the card or update the existing one.
6. Keep the structured header correct:
   - title
   - `<reference-hint>`
   - `<use-when>`
   - `<focus>`
   - `<keywords>`
7. Write the body insight-first:
   - extract the reusable lesson from the problem itself
   - write the judgment or insight, not the chat transcript
   - keep concrete case details, logs, traces, and code paths only as examples or evidence
8. Review the result before finishing:
   - if the example case were removed, would the main insight still stand
   - would a future reader who does not know this conversation still get a useful insight from the card
   - does the card say what it means and not accidentally imply more than it should
   - is this clearly different from existing cards
   - is the body centered on reusable guidance rather than a debugging timeline
9. If the card is broadly useful beyond the local case, suggest a main-repo PR in natural wording.

## Writing Mindset

- Start from the most reusable idea, not from the order of discovery.
- Prefer judgments, checks, and durable interpretation rules over storytelling.
- Use concrete examples to support the insight, not to become the whole card.
- Keep the card understandable without the current conversation.
- Write so a future reader who does not know this case or chat can still get a useful insight from the card.

## Stop And Ask

- The conclusion is still speculative.
- The evidence is too weak to separate fact from inference.
- The material belongs in an existing card but the correct target is still unclear.
- The user has not yet agreed to write the card.

## Handoff

Stay in this skill when:

- the user has agreed to capture the conclusion
- you are deciding new-card versus update-existing-card
- you are writing or reviewing the knowledge card itself

Return to the caller when:

- the card has been written or updated
- the user declined to capture the conclusion
- you determined that the conclusion is not stable or general enough to preserve

Before returning, report:

- whether you updated an existing card or created a new one
- why the conclusion was judged reusable or not reusable
- whether this looks broad enough to merit a main-repo PR suggestion

## Integration

- Called by: `openusim-analyze-results`, direct user requests to write or update a knowledge card
- Reads from: `../openusim-references/`
- Writes to: `../openusim-references/`
- Uses the existing `<reference-hint>` routing structure to avoid duplicate cards

## Common Mistakes

- Writing the investigation timeline instead of the reusable lesson
- Treating one successful reproduction as proof of a general rule
- Creating a new card when a focused extension to an existing card would do
- Writing before asking the user
- Turning a local workaround into a universal recommendation
