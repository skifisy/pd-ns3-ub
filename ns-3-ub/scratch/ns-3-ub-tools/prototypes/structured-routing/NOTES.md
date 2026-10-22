# Prototype findings

## Question

Can an unlabeled NetworkX topology graph recover useful structured routing information while
preserving node, link, parallel-link, port, and shortest-path candidate semantics?

## Current answer

Yes, for exact structures with deterministic recognizers and an independent verifier. The
prototype recovered:

- a 4 x 4 two-dimensional dimension full mesh;
- a two-layer Clos with single or parallel links;
- two internal Clos blocks connected through explicit switch port extensions;
- two dimension-fullmesh groups connected through two independent Clos planes;
- the same multi-plane topology after node IDs and edge insertion order were shuffled.

The two regular standalone structures and the complete multi-plane combination reached full
candidate-port equivalence. The port-extension case retained cross-block lookups in table routing,
showing that mixed ownership can preserve an explicit irregular boundary without rejecting the
verified internal Clos blocks.

Missing links, extra links, and an edge swap were not accepted as complete structures. Unaffected
verified regions remained eligible for rule routing, while affected route lookup items remained in
table routing.

Observed results on the built-in small cases:

| Case | Graph verified | Route coverage | Remaining table items |
| --- | --- | ---: | ---: |
| 4 x 4 dimension full mesh | yes | 100% | 0 |
| two-layer Clos | yes | 100% | 0 |
| parallel-link Clos | yes | 100% | 0 |
| two Clos blocks with port extension | yes | 43.8% | 72 |
| dimension full mesh with two Clos planes | yes | 100% | 0 |
| shuffled IDs and edge order | yes | 100% | 0 |
| missing link | no | 63.0% | 306 |
| extra link | no | 63.0% | 306 |
| dimension edge swap | no | 0% | 392 |

The standalone dimension-fullmesh and Clos cases, including parallel links, matched the current
`gen_compressed_route_table()` output after both sides were expanded to route lookup items. The
multi-plane case kept the same 828 route lookup items and full equivalence after IDs and edge order
were shuffled.

The complete prototype run took about 0.11 seconds and reached about 42.6 MB maximum RSS on the
development machine. These numbers describe only these tiny synthetic cases and must not be used
as a production performance claim.

## Important limitation

The prototype computes candidate coverage with all-pairs shortest paths. It proves that the
recognized structure can own those route lookup items, but it is not the production rule program
and does not demonstrate Stage 2 cold-start or peak-memory improvements. Stage 1 still needs a
baseline comparison against the current route generator. Stage 2 must replace this all-pairs step
with direct rule generation and measure startup time, peak RSS, steady route memory, and lookup
cost.
