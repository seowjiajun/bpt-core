<div align="center">
  <img src="docs/bpt-transport-logo.png" alt="BPT Transport" width="200"/>
  <h1>BPT Transport</h1>
  <p><strong>High-performance Aeron Media Driver wrapper for low-latency messaging infrastructure.</strong></p>
</div>

BPT Transport is a production-grade standalone runner for the [Aeron](https://github.com/real-logic/aeron) Media Driver. It provides YAML-driven configuration, CLI overrides, environment-aware validation, and structured lifecycle management — designed for latency-sensitive systems such as market-data distribution and order routing.

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [CLI Reference](#cli-reference)
- [Thread Model and Idle Strategies](#thread-model-and-idle-strategies)
- [Operational Notes](#operational-notes)
- [Developer Tools](#developer-tools)
- [Project Structure](#project-structure)
- [Development](#development)
- [License](#license)

---

## Prerequisites

| Requirement | Version            |
|-------------|--------------------|
| JDK         | 17+                |
| OS          | Linux (recommended), WSL2 on Windows |
| Gradle      | Wrapper included (`./gradlew`) |

---

## Quick Start

### Build

```bash
./gradlew build
```

### Run via Gradle

```bash
./gradlew run --args="--config config/config.yaml"
```

### Run via fat JAR

```bash
./gradlew shadowJar
./scripts/start_jar.sh --config config/config.yaml
```

### Dev workflow (background daemon)

```bash
./scripts/dev_start.sh            # Start in background, tail logs on success
./scripts/dev_stop.sh             # Stop gracefully
./scripts/dev_start.sh config/config.yaml  # Custom config
```

---

## Configuration

Configuration is loaded from a YAML file specified via the `--config` flag. All keys are optional — sensible defaults are applied when omitted.

### Full Reference

```yaml
aeron:
  directory: "/dev/shm/aeron-bpt"     # Aeron shared-memory directory
  threading_mode: "SHARED"                # SHARED | SHARED_NETWORK | DEDICATED
  idle_strategy: "BUSY_SPIN"              # Shared idle strategy (see table below)

  conductor_idle_strategy: null           # Per-thread overrides (fall back to idle_strategy)
  receiver_idle_strategy: null
  sender_idle_strategy: null

  dir_delete_on_start: false              # Delete the Aeron directory on launch
  dir_delete_on_shutdown: true            # Delete the Aeron directory on clean exit

  driver_timeout_ms: null                 # Driver timeout (ms)
  client_liveness_timeout_ns: null        # Client liveness timeout (ns)
  term_buffer_length: null                # IPC term buffer length (bytes, must be power of 2)
  mtu_length: null                        # MTU length (bytes)
  heartbeat_interval_sec: 30              # How often the alive heartbeat is logged (seconds)

logger:
  level: "info"                           # Root log level: TRACE, DEBUG, INFO, WARN, ERROR
```

> **Note:** When a per-thread idle strategy (`conductor_idle_strategy`, etc.) is not set, it inherits the value of `idle_strategy`.

### Environment-Aware Validation

The configuration validator enforces safety rules based on the `--env` flag:

- **PROD**: `dir_delete_on_start` is rejected unless explicitly overridden with `--force-delete-on-start`, preventing accidental data loss in production.
- **DEV / QA**: A warning is logged when `dir_delete_on_start: true` to alert operators before data is wiped.

---

## CLI Reference

```
Usage: MediaDriverMain [-hV] -c=<config> [--env=<env>] [OPTIONS]

Launches an Aeron Media Driver.

Options:
  -c, --config=<path>              Path to the configuration YAML (required)
      --env=<ENV>                  Environment: DEV, QA, PROD (default: DEV)
      --force-delete-on-start      Override dir_delete_on_start safety guard
      --force-delete-on-shutdown   Force directory deletion on shutdown
      --validate-only              Load and validate config, then exit (0 = OK, 2 = invalid)
      --print-effective-config     Print the merged effective config and exit
      --version                    Print version and exit
  -h, --help                       Display help and exit
```

### Diagnostic Modes

Validate a configuration file without starting the driver:

```bash
./gradlew run --args="--config config/config.yaml --validate-only"
```

Inspect the fully-merged configuration (file defaults + CLI overrides):

```bash
./gradlew run --args="--config config/config.yaml --print-effective-config"
```

---

## Thread Model and Idle Strategies

### Threading Modes

| Mode             | Description                                                                 |
|------------------|-----------------------------------------------------------------------------|
| `SHARED`         | All threads share a single agent. Lowest thread count, higher contention.   |
| `SHARED_NETWORK` | Receiver and sender share one thread; conductor runs separately.            |
| `DEDICATED`      | Each agent (conductor, receiver, sender) runs on its own thread.            |

### Idle Strategies

| Strategy           | Description                                                          |
|--------------------|----------------------------------------------------------------------|
| `BUSY_SPIN`        | Tight spin loop. Lowest latency, highest CPU usage.                  |
| `YIELDING`         | Calls `Thread.yield()` between polls. Moderate latency and CPU.      |
| `SLEEPING`         | Sleeps for 1 ms (1,000,000 ns) between polls. Lowest CPU.           |
| `SLEEPING:<nanos>` | Sleeps for the specified nanoseconds, e.g., `SLEEPING:1000` = 1 µs. |

Per-thread overrides allow fine-grained tuning — for example, using `BUSY_SPIN` for the receiver while keeping the conductor on `SLEEPING`:

```yaml
aeron:
  idle_strategy: "SLEEPING"
  receiver_idle_strategy: "BUSY_SPIN"
```

---

## Operational Notes

### Verifying a Running Driver

Confirm the shared-memory directory exists:

```bash
ls -ld /dev/shm/aeron-bpt
```

View live driver counters (publications, subscriptions, bytes sent/received):

```bash
java --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
     --add-opens java.base/java.nio=ALL-UNNAMED \
     -Daeron.dir=/dev/shm/aeron-bpt \
     -cp build/libs/*-all.jar \
     io.aeron.samples.AeronStat
```

### Heartbeat Logging

Once launched, the driver emits a heartbeat log at the configured interval (`heartbeat_interval_sec`, default 30 s):

```
MediaDriver alive (uptime=60s, aeronDir=/dev/shm/aeron-bpt, threading=SHARED, idle=BUSY_SPIN)
```

### Log Files

Logs are written to both stdout and a rolling file in `logs/bpt-transport.log` (100 MB per file, 30-day history, 1 GB total cap). Override the log directory with:

```bash
java -DBPT_TRANSPORT_LOG_DIR=/var/log/bpt-transport ...
```

### Shutdown

The driver responds to standard OS signals (`SIGINT`, `SIGTERM`). On shutdown:

1. The heartbeat executor is terminated.
2. The `MediaDriver` is closed.
3. The Aeron directory is optionally deleted (per `dir_delete_on_shutdown`).

### Exit Codes

| Code | Meaning                               |
|------|---------------------------------------|
| `0`  | Clean shutdown                        |
| `1`  | Fatal runtime exception               |
| `2`  | Setup failure (config load/validation)|

---

## Developer Tools

The `tools/` directory contains lightweight Aeron client utilities for manual testing. They connect to the running MediaDriver (bpt-transport) and require no additional setup.

### Starting

Use `tools/run.sh` as a launcher — it sets the required JVM flags and auto-builds the fat JAR if missing:

```bash
./tools/run.sh <Tool> [flags]
```

### Subscriber

Connects to a channel and prints every incoming message. Runs until `Ctrl+C`.

```bash
./tools/run.sh Subscriber
./tools/run.sh Subscriber --channel aeron:ipc --stream 1001
./tools/run.sh Subscriber --help
```

### Publisher

Connects to a channel and sends a message for each line typed. Exit with `Ctrl+D`.

```bash
./tools/run.sh Publisher
./tools/run.sh Publisher --channel aeron:ipc --stream 1001
./tools/run.sh Publisher --help
```

### Flags

| Flag | Subscriber | Publisher | Default |
|------|:---:|:---:|---------|
| `-c`, `--channel` | ✓ | ✓ | `aeron:ipc` |
| `-s`, `--stream` | ✓ | ✓ | `1001` |
| `-d`, `--dir` | ✓ | ✓ | `/dev/shm/aeron-bpt` |

### Example session

**Terminal 1:**
```
$ ./tools/run.sh Subscriber
Subscribing on aeron:ipc stream 1001
Waiting for messages... (Ctrl+C to stop)
[recv] hello bpt
[recv] second message
```

**Terminal 2:**
```
$ ./tools/run.sh Publisher
Waiting for subscriber... connected.
Type a message and press Enter to send. Ctrl+D to quit.
hello bpt
[sent] hello bpt
second message
[sent] second message
```

---

## Project Structure

```
bpt-transport/
├── config/
│   └── config.yaml                  # Default configuration
├── scripts/
│   ├── dev_start.sh                 # Start driver as background daemon
│   ├── dev_stop.sh                  # Stop background daemon
│   ├── start.sh                     # Gradle-based launch wrapper
│   ├── start_jar.sh                 # Fat-JAR launch wrapper
│   ├── clean_build.sh               # Clean + rebuild helper
│   └── bootstrap_build.sh           # First-time bootstrap (no Gradle required)
├── tools/
│   ├── run.sh                       # Tool launcher (sets JVM flags, builds JAR if needed)
│   ├── Subscriber.java              # Interactive Aeron subscriber
│   └── Publisher.java               # Interactive Aeron publisher
├── src/
│   ├── main/java/bpt/transport/
│   │   ├── MediaDriverMain.java     # CLI entry point (picocli)
│   │   ├── MediaDriverRunner.java   # Driver lifecycle management
│   │   ├── Config.java              # YAML config loader and defaults
│   │   ├── EffectiveConfig.java     # Immutable merged config record
│   │   ├── ConfigValidator.java     # Environment-aware validation
│   │   └── IdleStrategyParser.java  # Idle strategy and threading mode parser
│   ├── main/resources/
│   │   └── logback.xml              # Logging configuration (console + rolling file)
│   └── test/java/bpt/transport/
│       ├── MediaDriverMainTest.java
│       ├── MediaDriverRunnerTest.java
│       ├── ConfigTest.java
│       ├── ConfigValidatorTest.java
│       └── IdleStrategyParserTest.java
├── build.gradle                     # Gradle build (shadowJar, Spotless)
└── settings.gradle
```

---

## Development

### Build and Test

```bash
./gradlew build          # Compile, test, and package
./gradlew test           # Run unit tests only
./gradlew spotlessApply  # Auto-format source (Google Java Format)
./gradlew shadowJar      # Build fat JAR (build/libs/*-all.jar)
```

### JVM Flags

Aeron requires reflective access to NIO internals. These flags are pre-configured in `build.gradle`, all launch scripts, and the test task:

```
--add-opens java.base/sun.nio.ch=ALL-UNNAMED
--add-opens java.base/java.nio=ALL-UNNAMED
```

### Key Dependencies

| Dependency          | Version        | Purpose                             |
|---------------------|----------------|-------------------------------------|
| Aeron               | 1.44.1         | Low-latency messaging transport     |
| picocli             | 4.7.5          | CLI argument parsing                |
| SnakeYAML           | 2.0            | YAML configuration loading          |
| SLF4J + Logback     | 2.0.7 / 1.4.11 | Structured logging                  |
| JUnit 5             | 5.9.3          | Unit testing                        |
| Mockito             | 5.5.0          | Test mocking                        |

---

## License

This project is proprietary and confidential. Unauthorized distribution is prohibited.
