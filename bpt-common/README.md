# bpt-common

Shared C++ utility library for the bpt-core trading services.

All C++ services (`bpt-order-gateway`, `bpt-md-gateway`, `bpt-strategy`, `bpt-pricer`, `bpt-analytics`, `bpt-refdata`, `bpt-backtester`, `bpt-bridge`) link against this library for their common primitives.

## Requirements

- C++20
- Quill (async logging — `bpt::common::logging`, `bpt::common::log`)
- Aeron C++ client (`bpt::common::aeron`)
- Boost.Beast + Boost.Asio + OpenSSL (`bpt::common::ws`)
- fast_float (`bpt::common::util::parse_double`)

## Bazel integration

```python
cc_library(
    name = "my_service",
    deps = ["//bpt-common"],
)
```

## Headers

**`aeron/`**

| Header | Namespace | Description |
|---|---|---|
| `aeron/aeron_utils.h` | `bpt::common::aeron` | `wait_for_publication`, `wait_for_subscription`, `connect` — spin until Aeron connects |
| `aeron/stream_config.h` | `bpt::common::config` | `StreamConfig{channel, stream_id}` — Aeron addressing unit |

**`util/`**

| Header | Namespace | Description |
|---|---|---|
| `util/latency_histogram.h` | `bpt::common::util` | Lock-free power-of-2 bucket histogram; ~5ns `record()` cost |
| `util/parse_double.h` | `bpt::common::util` | `ff_double` — fast_float wrapper for simdjson-sourced quoted numbers |
| `util/spsc_queue.h` | `bpt::common::util` | `SpscQueue<CAPACITY, MAX_PAYLOAD_BYTES>` — lock-free ring buffer |
| `util/thread_pin.h` | `bpt::common::util` | `pin_thread_to_cpu(cpu_id, name)` — `pthread_setaffinity_np` wrapper |
| `util/tsc_clock.h` | `bpt::common::util` | `TscClock` — invariant-TSC wall clock; ~4ns vs ~20ns for vDSO |

**`ws/`**

| Header | Namespace | Description |
|---|---|---|
| `ws/ws_connect.h` | `bpt::common::ws` | TLS WebSocket connect: DNS + TCP + TLS + WS upgrade |
| `ws/run_loop.h` | `bpt::common::ws` | Shared WS read/send/heartbeat loop |

**Root**

| Header | Namespace | Description |
|---|---|---|
| `logging.h` | `bpt::common::logging`, `bpt::common::log` | Async Quill init + `info/warn/error/debug` macros |
| `logging_toml.h` | `bpt::common::logging` | `from_toml(table)` — parse a `[logging]` block into `LogConfig` |
| `signal.h` | `bpt::common::signal` | `install()` / `is_running()` / `stop()` — SIGINT/SIGTERM handling |
| `secrets/secrets_client.h` | `bpt::common::secrets` | Load credentials from `$CREDENTIALS_DIRECTORY` (systemd-creds) |

## Usage examples

```cpp
#include <bpt_common/signal.h>
#include <bpt_common/logging.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/util/tsc_clock.h>

// Signal handling
bpt::common::signal::install();
while (bpt::common::signal::is_running()) { /* main loop */ }

// TSC clock — call once at startup
bpt::common::util::TscClock::calibrate();
uint64_t t = bpt::common::util::TscClock::now_epoch_ns();

// Aeron
auto aeron = bpt::common::aeron::connect("/dev/shm/aeron-bpt");
auto pub   = bpt::common::aeron::wait_for_publication(aeron, "aeron:ipc", 2001);
```
