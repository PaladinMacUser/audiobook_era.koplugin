# G2P gate corpora — provenance

The `dev/snt-g2p-gate.sh` corpus OOV gate runs the lexicon G2P over three
public-domain English novels (Project Gutenberg). The files are fetched at
gate time to `$G2P_CORPUS_DIR` (default `/tmp/g2p-corpus`) and are not
committed; verify a downloaded copy against these hashes before trusting a
gate result:

| file | source | sha256 |
|---|---|---|
| `pride_prejudice.txt` | https://www.gutenberg.org/files/1342/1342-0.txt | `81300b79e8a8d65ac530a97578417d06137e3bbc90622a10a65e5036183d2500` |
| `moby_dick.txt` | https://www.gutenberg.org/files/2701/2701-0.txt | `f12eb530502755ef2b8f535e2ecef3e95802eab584e60ac7992ccf856256fdbe` |
| `great_gatsby.txt` | https://www.gutenberg.org/cache/epub/64317/pg64317.txt | `ce760ec377accd352b41bb8f64504a72d7aa18ab3afb42ded2b56cecacf29e35` |

Fetched 2026-10-20. All three are US public domain (` ebooks posted
before 2026-01-01`). Re-download with:

    curl -fsSL <source> -o <file>

## Baseline numbers at the default threshold (0.25), 2026-10-20

| corpus | token OOV | clauses routed to espeak |
|---|---|---|
| Pride and Prejudice | 2.12% | 2.07% |
| Moby Dick | 2.53% | 4.26% |
| The Great Gatsby | 1.87% | 1.77% |

Moby Dick sits above the 3% clause target on archaic whaling vocabulary
and proper nouns (Queequeg, Tashtego, leviathan); the gate flags it as a
note rather than a failure because the listening A/B, not the rate, decides
whether per-clause mixing is audible. Levers if it matters: a wider
dictionary (the tables regenerate from misaki's gold/silver data) or a
higher `--g2p-oov-threshold` for that kind of text.
