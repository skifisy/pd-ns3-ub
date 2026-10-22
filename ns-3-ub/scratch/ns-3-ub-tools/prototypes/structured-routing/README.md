# PROTOTYPE: structured topology recognition

This throwaway prototype answers one question: can the existing NetworkX topology graph recover
useful repeated structure without relying on node IDs or user-provided topology labels, and can the
result be checked against physical links, ports, and shortest-path candidate ports before mixed
routing uses it?

It recognizes only exact two-dimensional dimension full mesh groups and exact two-layer Clos
blocks. Switch links outside a recognized Clos block are retained as explicit port extensions. The
prototype deliberately uses all-pairs shortest paths to measure potential rule coverage; it does
not implement the production rule runtime or its Stage 2 direct route generator.

Run every built-in positive and negative case:

```bash
uv run --with networkx==3.6.1 --with tqdm==4.68.4 \
  python scratch/ns-3-ub-tools/prototypes/structured-routing/run.py --all
```

Run without `--all` to select cases interactively.
