"""BPT Tape — recorder freshness, throughput, and disk health.

The dashboard a recorder operator opens first when something feels off.
Designed around the failure mode that bit us on 2026-05-09: writer alive,
no data flowing, no alert. Top row makes that condition visible at a
glance; lower rows are the supporting evidence (rate, rotations, errors).

Render with:
    cd monitoring && make

Output lands in monitoring/generated/bpt_tape.json and is picked up by
Grafana via file provisioning on next reload.
"""

from grafanalib.core import (
    BYTES_FORMAT,
    OPS_FORMAT,
    PERCENT_FORMAT,
    SECONDS_FORMAT,
    SHORT_FORMAT,
    Dashboard,
    Graph,
    GridPos,
    Stat,
    Target,
    Templating,
    Time,
    YAxes,
    YAxis,
)

DATASOURCE = "Prometheus"


def stat(
    title,
    expr,
    *,
    x,
    y,
    w=4,
    h=3,
    unit=SHORT_FORMAT,
    color_mode="value",
    thresholds=None,
    decimals=None,
):
    s = Stat(
        title=title,
        dataSource=DATASOURCE,
        targets=[Target(expr=expr, refId="A")],
        gridPos=GridPos(h=h, w=w, x=x, y=y),
        format=unit,
        colorMode=color_mode,
        reduceCalc="lastNotNull",
        graphMode="area",
        textMode="value_and_name",
    )
    if thresholds is not None:
        s.thresholds = thresholds
    if decimals is not None:
        s.decimals = decimals
    return s


def graph(title, targets, *, x, y, w=12, h=7, unit=SHORT_FORMAT):
    return Graph(
        title=title,
        dataSource=DATASOURCE,
        targets=targets,
        gridPos=GridPos(h=h, w=w, x=x, y=y),
        yAxes=YAxes(
            left=YAxis(format=unit),
            right=YAxis(format=SHORT_FORMAT),
        ),
    )


def target(expr, legend):
    return Target(expr=expr, legendFormat=legend, refId=legend)


# Freshness thresholds: green if last write was within 1 min, yellow up
# to 5 min, red beyond. Matches TapeWriterStale alert (300s = critical).
FRESHNESS_THRESHOLDS = [
    {"color": "green", "value": None},
    {"color": "yellow", "value": 60},
    {"color": "red", "value": 300},
]

HEALTH_THRESHOLDS = [
    {"color": "red", "value": None},
    {"color": "green", "value": 1},
]


# ── Row 1: At-a-glance freshness (y=0) ────────────────────────────────
# Two big numbers + the healthy gauge. If the writer-stale number turns
# red, you're in incident mode — go check the recorder before reading
# anything else on this dashboard.

top_row = [
    stat(
        "tape healthy",
        'bpt_tape_healthy{job="bpt-tape"} or on() vector(0)',
        x=0,
        y=0,
        w=4,
        h=4,
        thresholds=HEALTH_THRESHOLDS,
    ),
    stat(
        "since last write — md (s)",
        'time() - bpt_tape_last_wslog_write_unix_seconds{job="bpt-tape",venue="hyperliquid"}',
        x=4,
        y=0,
        w=10,
        h=4,
        unit=SECONDS_FORMAT,
        decimals=0,
        thresholds=FRESHNESS_THRESHOLDS,
    ),
    stat(
        "since last write — rest (s)",
        'time() - bpt_tape_last_wslog_write_unix_seconds{job="bpt-tape",venue="hyperliquid-rest"}',
        x=14,
        y=0,
        w=10,
        h=4,
        unit=SECONDS_FORMAT,
        decimals=0,
        # REST polls hourly so 5min freshness threshold doesn't apply;
        # set a wider threshold (90 min before red).
        thresholds=[
            {"color": "green", "value": None},
            {"color": "yellow", "value": 3700},  # ~62 min
            {"color": "red", "value": 5400},  # 90 min
        ],
    ),
]


# ── Row 2: Throughput (y=4) ───────────────────────────────────────────

throughput_panels = [
    graph(
        "Frames written rate",
        [target('rate(bpt_tape_frames_written_total{job="bpt-tape"}[1m])', "{{venue}}")],
        x=0,
        y=4,
        w=12,
        h=7,
        unit=OPS_FORMAT,
    ),
    graph(
        "Bytes written rate",
        [target('rate(bpt_tape_bytes_written_total{job="bpt-tape"}[1m])', "{{venue}}")],
        x=12,
        y=4,
        w=12,
        h=7,
        unit=BYTES_FORMAT,
    ),
]


# ── Row 3: Rotations + failures (y=11) ────────────────────────────────
# Rotations are a heartbeat: ~1/hour/venue under default config. A drop
# in rotation rate is the second-strongest signal of a stuck writer
# (after last_write going stale).
# rotation_failures is the smoking gun — non-zero means the recorder
# crashed-and-restarted.

rotation_panels = [
    graph(
        "Rotations / min",
        [target('rate(bpt_tape_wslog_rotations_total{job="bpt-tape"}[5m]) * 60', "{{venue}}")],
        x=0,
        y=11,
        w=12,
        h=7,
    ),
    graph(
        # `or vector(0)` so the panel renders 0 instead of "no data" when
        # the counter has never incremented. The series goes away again
        # if/when an actual failure happens (the labeled series takes
        # over via PromQL's set semantics).
        "Rotation failures (5m)",
        [
            target(
                'increase(bpt_tape_wslog_rotation_failures_total{job="bpt-tape"}[5m]) '
                "or on() vector(0)",
                "{{venue}}/{{cause}}",
            )
        ],
        x=12,
        y=11,
        w=12,
        h=7,
    ),
]


# ── Row 4: Disk on the recorder (y=18) ────────────────────────────────
# Pulled from node-exporter (filesystem collector). The recorder doesn't
# run node-exporter today; this row will be empty until #monitoring
# adds it. Keeping the panel here so the dashboard is complete the
# moment the data arrives.

disk_panels = [
    graph(
        "/opt/bpt/data free",
        [
            target(
                'node_filesystem_avail_bytes{mountpoint="/opt/bpt/data"}',
                "{{instance}}",
            )
        ],
        x=0,
        y=18,
        w=12,
        h=7,
        unit=BYTES_FORMAT,
    ),
    graph(
        "/opt/bpt/data % used",
        [
            target(
                '100 * (1 - node_filesystem_avail_bytes{mountpoint="/opt/bpt/data"}'
                '         / node_filesystem_size_bytes{mountpoint="/opt/bpt/data"})',
                "{{instance}}",
            )
        ],
        x=12,
        y=18,
        w=12,
        h=7,
    ),
]


# ── Row 5: WebSocket connection state (y=25) ─────────────────────────
# bpt_tape_ws_connected: bool, current state per venue (1 = connected).
# bpt_tape_subscriptions: count of currently-subscribed instruments per
# venue; catches the "config regression shrunk the universe" failure
# class (would have caught the 12-vs-230 confusion immediately).

ws_state_row = [
    stat(
        "ws connected — md",
        'bpt_tape_ws_connected{job="bpt-tape",venue="hyperliquid"} or on() vector(0)',
        x=0,
        y=25,
        w=6,
        h=4,
        thresholds=HEALTH_THRESHOLDS,
    ),
    stat(
        "subscriptions — hyperliquid",
        'bpt_tape_subscriptions{job="bpt-tape",venue="hyperliquid"}',
        x=6,
        y=25,
        w=6,
        h=4,
    ),
    stat(
        "ws connected — rest",
        # No bpt_tape_ws_connected for the REST stream — the REST poller
        # opens connections per-call rather than maintaining a long-lived
        # one. Substitute the freshness gauge: if rest hasn't written in
        # over 90 min, it's effectively "down".
        'clamp_max(bpt_tape_last_wslog_write_unix_seconds{job="bpt-tape",venue="hyperliquid-rest"} > bool (time() - 5400), 1) or on() vector(1)',
        x=12,
        y=25,
        w=6,
        h=4,
        thresholds=HEALTH_THRESHOLDS,
    ),
    stat(
        "subscriptions — rest endpoints",
        # REST endpoints aren't subscribed in the same way; show 0 for
        # consistency until refdata-poller exposes its own count.
        'count(bpt_tape_last_wslog_write_unix_seconds{job="bpt-tape",venue="hyperliquid-rest"}) or on() vector(0)',
        x=18,
        y=25,
        w=6,
        h=4,
    ),
]


# ── Row 6: WS reconnect rate (y=29) ──────────────────────────────────
# Counter increments on every ws_connect (including initial bootstrap).
# rate() over 5m removes the bootstrap noise; sustained > 0.01/s = a
# reconnect every couple of minutes = flapping. TapeWsFlapping alert
# fires at that threshold.

ws_rate_panels = [
    graph(
        "WS reconnects / min (per venue)",
        [target('rate(bpt_tape_ws_reconnects_total{job="bpt-tape"}[5m]) * 60', "{{venue}}")],
        x=0,
        y=29,
        w=12,
        h=7,
        unit=OPS_FORMAT,
    ),
    graph(
        "WS reconnect cumulative",
        [target('bpt_tape_ws_reconnects_total{job="bpt-tape"}', "{{venue}}")],
        x=12,
        y=29,
        w=12,
        h=7,
    ),
]


# ── Row 7: Tape host CPU + memory (y=36) ─────────────────────────────
# From node-exporter on the tape host (job="tape-host"). Catches host-
# level pressure (contention, runaway process, leak) before it cascades
# into app-level symptoms.

host_cpu_mem_panels = [
    graph(
        "Host CPU % (per mode)",
        [
            # 1 - irate(idle) gives total non-idle. Per-mode breakdown
            # below shows where CPU went (user/sys/iowait/etc).
            target(
                'avg by (mode) (rate(node_cpu_seconds_total{job="tape-host"}[2m])) * 100',
                "{{mode}}",
            ),
        ],
        x=0,
        y=36,
        w=12,
        h=7,
        unit=PERCENT_FORMAT,
    ),
    graph(
        "Host memory",
        [
            target('node_memory_MemAvailable_bytes{job="tape-host"}', "available"),
            target('node_memory_MemFree_bytes{job="tape-host"}', "free"),
            target('node_memory_Cached_bytes{job="tape-host"}', "cached"),
            target('node_memory_Buffers_bytes{job="tape-host"}', "buffers"),
        ],
        x=12,
        y=36,
        w=12,
        h=7,
        unit=BYTES_FORMAT,
    ),
]


# ── Row 8: Tape host network + load (y=43) ───────────────────────────
# Network: cross-check against bytes_written. RX should roughly track
# venue WS data rate; TX is mostly S3 sync traffic during the hourly
# upload window.
# Load: single-number health summary. >2 sustained on a 2-vCPU box =
# queueing.

host_net_load_panels = [
    graph(
        "Host network throughput",
        [
            # ens5 / eth0 / etc — match all named interfaces, exclude
            # loopback. node_network_receive_bytes_total is per-NIC.
            target(
                'rate(node_network_receive_bytes_total{job="tape-host",device!~"lo|docker.*"}[1m])',
                "rx {{device}}",
            ),
            target(
                'rate(node_network_transmit_bytes_total{job="tape-host",device!~"lo|docker.*"}[1m])',
                "tx {{device}}",
            ),
        ],
        x=0,
        y=43,
        w=12,
        h=7,
        unit=BYTES_FORMAT,
    ),
    graph(
        "Host load average",
        [
            target('node_load1{job="tape-host"}', "1m"),
            target('node_load5{job="tape-host"}', "5m"),
            target('node_load15{job="tape-host"}', "15m"),
        ],
        x=12,
        y=43,
        w=12,
        h=7,
    ),
]


# ── Row 9: Inode usage on /opt/bpt/data (y=50) ───────────────────────
# Inode exhaustion is a different failure mode from byte-exhaustion
# (lots of small files): df -h says 50% but mkfs reports ENOSPC. The
# wslog stream rotates hourly = ~24 files/day, well below limits, but
# parquet conversions can produce many small files. Catch it before
# ENOSPC hits.

inode_panel = [
    stat(
        "/opt/bpt/data inode % free",
        '100 * node_filesystem_files_free{job="tape-host",mountpoint="/opt/bpt/data"}'
        ' / node_filesystem_files{job="tape-host",mountpoint="/opt/bpt/data"}',
        x=0,
        y=50,
        w=12,
        h=4,
        unit=PERCENT_FORMAT,
        decimals=1,
        thresholds=[
            {"color": "red", "value": None},
            {"color": "yellow", "value": 20},
            {"color": "green", "value": 50},
        ],
    ),
    stat(
        "/opt/bpt/data inodes used",
        'node_filesystem_files{job="tape-host",mountpoint="/opt/bpt/data"} '
        '- node_filesystem_files_free{job="tape-host",mountpoint="/opt/bpt/data"}',
        x=12,
        y=50,
        w=12,
        h=4,
        decimals=0,
    ),
]


# ── Dashboard ─────────────────────────────────────────────────────────

dashboard = Dashboard(
    title="BPT Tape",
    description=(
        "Recorder freshness, throughput, rotations, disk, ws state, "
        "host health. Top row answers 'is tape capturing right now'; "
        "lower rows are layered evidence (app → host)."
    ),
    tags=["bpt", "tape", "ops"],
    timezone="browser",
    refresh="10s",
    time=Time("now-30m", "now"),
    templating=Templating(list=[]),
    panels=(
        top_row
        + throughput_panels
        + rotation_panels
        + disk_panels
        + ws_state_row
        + ws_rate_panels
        + host_cpu_mem_panels
        + host_net_load_panels
        + inode_panel
    ),
    uid="bpt-tape",
    version=2,
    schemaVersion=30,
).auto_panel_ids()
