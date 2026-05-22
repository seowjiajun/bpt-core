# bpt-research

Jupyter notebooks + Python helpers for strategy *discovery* — the
under-built side of bpt-core. Lives outside the C++ Bazel build because
it's iterative research, not production code.

Consumes:
- `bpt-canon/python/bpt_canon` — canon → pandas
- `bpt-features/python/bpt_features` — same C++ feature impls AS uses,
  via pybind11 (so research and prod can't drift)

See `notebooks/` for current work. Roadmap in `docs/backlog.md` →
"Research stack".

## Setup (one-time, for notebooks importing bpt_features)

The pybind11 `_core.so` is bazel-built. Two stitches needed for a
Jupyter notebook to import it cleanly:

```bash
# 1. Build the compiled extension
bazel build //bpt-features/python/bpt_features:_core

# 2. Symlink it into the source dir so `import bpt_features` finds both
#    the .py wrappers AND _core.so via one path.
ln -sf "$(pwd)/bazel-bin/bpt-features/python/bpt_features/_core.so" \
       bpt-features/python/bpt_features/_core.so
```

If Jupyter runs in Anaconda Python and you hit
`GLIBCXX_3.4.31 not found`, preload the system libstdc++ at launch:

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 jupyter lab
```

Or just use system python3 (`/usr/bin/python3`) — it links the system
libstdc++ already.

## Producing canon files for a notebook

```bash
# From an existing wslog capture (lossless replay):
bazel run //bpt-canon:bpt-canon-replay -- \
  --wslog /opt/bpt/data/raw/hyperliquid/<date>/hyperliquid-*.wslog \
  --instrument-mapping config/instruments/instrument_mapping.hyperliquid-mainnet.json \
  --output /tmp/bpt_canon/hl-<date>.canon

# From OKX historical archives:
bazel run //bpt-canon:bpt-canon-ingest-okx-trades -- --csv ... --output ...
bazel run //bpt-canon:bpt-canon-ingest-okx-l2     -- --ndjson ... --output ...
bazel run //bpt-canon:bpt-canon-merge -- <trades>.canon <l2>.canon --output merged.canon
```
