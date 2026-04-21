"""BPT System Overview — operational health at a glance.

Scope is intentionally ops-only: service up/down, exchange connectivity,
trading activity rates, error counters. PnL / fills / order-book views
belong in the bpt-core dashboard under the existing React frontend.

Render with:
    cd monitoring && make

Output lands in monitoring/generated/bpt_system_overview.json and is
picked up by Grafana via file provisioning on next reload.
"""
from grafanalib.core import (
    Dashboard,
    Graph,
    GridPos,
    OPS_FORMAT,
    SHORT_FORMAT,
    Stat,
    Target,
    Templating,
    Time,
    YAxes,
    YAxis,
)

DATASOURCE = "Prometheus"

# ── Helpers ───────────────────────────────────────────────────────────

def stat(title, expr, *, x, y, w=4, h=3, unit=SHORT_FORMAT, color_mode="value",
         thresholds=None):
    """Compact stat panel with sensible defaults for boolean-ish gauges."""
    s = Stat(
        title=title,
        dataSource=DATASOURCE,
        targets=[Target(expr=expr, refId="A")],
        gridPos=GridPos(h=h, w=w, x=x, y=y),
        format=unit,
        colorMode=color_mode,
        reduceCalc="lastNotNull",
        graphMode="none",
        textMode="value_and_name",
    )
    if thresholds is not None:
        s.thresholds = thresholds
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

# Red-below, green-above thresholds for 0/1 health gauges.
HEALTH_THRESHOLDS = [
    {"color": "red",   "value": None},
    {"color": "green", "value": 1},
]


# ── Row 1: Service health (y=0) ───────────────────────────────────────
# Service _healthy gauges — the bpt convention is 1 = up/healthy, 0 = down.
# The `up` metric from Prometheus itself also goes to 0 when the scrape
# fails, which is how we detect a hard process crash vs a soft unhealthy.

health_panels = [
    stat("refdata",   'refdata_healthy or on() vector(0)',
         x=0,  y=0, w=3, thresholds=HEALTH_THRESHOLDS),
    stat("md-gw",   'md_gateway_healthy or on() vector(0)',
         x=3,  y=0, w=3, thresholds=HEALTH_THRESHOLDS),
    stat("order-gw", 'order_gateway_healthy or on() vector(0)',
         x=6,  y=0, w=3, thresholds=HEALTH_THRESHOLDS),
    stat("strategy",   'strategy_healthy or on() vector(0)',
         x=9,  y=0, w=3, thresholds=HEALTH_THRESHOLDS),
    stat("strategy active", 'strategy_strategy_active or on() vector(0)',
         x=12, y=0, w=3, thresholds=HEALTH_THRESHOLDS),
    stat("trading paused", 'strategy_trading_paused or on() vector(0)',
         x=15, y=0, w=3),
    stat("open orders", 'order_gateway_open_orders',
         x=18, y=0, w=3),
    stat("instruments", 'refdata_instruments_total',
         x=21, y=0, w=3),
]


# ── Row 2: Exchange connectivity (y=3) ───────────────────────────────
# Both order-gw and md-gw expose per-exchange connected gauges. Show
# them as a single graph so a drop on either side is visible.

connectivity_panels = [
    graph(
        "Exchange connectivity — order-gw",
        [target('order_gateway_exchange_connected', '{{exchange}}')],
        x=0, y=3, w=12, h=7,
    ),
    graph(
        "Exchange connectivity — md-gw",
        [target('md_gateway_exchange_connected', '{{exchange}}')],
        x=12, y=3, w=12, h=7,
    ),
]


# ── Row 3: Trading activity (y=10) ────────────────────────────────────
# All rates in per-second ops format. rate() over a 1m window is smooth
# enough for a 5s scrape without hiding traffic bursts.

activity_panels = [
    graph(
        "Order rate (order-gw)",
        [
            target('sum(rate(order_gateway_orders_received_total[1m]))', 'new orders'),
            target('sum(rate(order_gateway_exec_reports_total[1m]))',    'exec reports'),
        ],
        x=0, y=10, w=12, h=7, unit=OPS_FORMAT,
    ),
    graph(
        "Market data publish rate (md-gw)",
        [target('sum(rate(md_gateway_md_messages_published_total[1m])) by (exchange)',
                '{{exchange}}')],
        x=12, y=10, w=12, h=7, unit=OPS_FORMAT,
    ),
]


# ── Row 4: Order ACK latency (y=17) ───────────────────────────────────
# order_gateway_order_ack_rtt_ns is a histogram. Use histogram_quantile with
# the _bucket suffix to get p50 / p90 / p99. Units stay ns in the query
# and we divide to microseconds client-side for readability.

latency_panels = [
    graph(
        "Order ACK RTT (order-gw) — µs",
        [
            target('histogram_quantile(0.50, sum(rate(order_gateway_order_ack_rtt_ns_bucket[5m])) by (le)) / 1000',
                   'p50'),
            target('histogram_quantile(0.90, sum(rate(order_gateway_order_ack_rtt_ns_bucket[5m])) by (le)) / 1000',
                   'p90'),
            target('histogram_quantile(0.99, sum(rate(order_gateway_order_ack_rtt_ns_bucket[5m])) by (le)) / 1000',
                   'p99'),
        ],
        x=0, y=17, w=12, h=8,
    ),
    # Strategy internal latency. Measures T0 = md-gw tick publish time →
    # T3 = strategy callback returns (and the order-placed subset). The
    # order-gw panel above is the EXCHANGE round-trip; this one isolates
    # our own code path, which should be microseconds on a healthy box.
    graph(
        "Strategy tick→strategy latency — µs",
        [
            target('histogram_quantile(0.50, sum(rate(strategy_tick_to_strategy_ns_bucket[5m])) by (le)) / 1000',
                   'tick p50'),
            target('histogram_quantile(0.99, sum(rate(strategy_tick_to_strategy_ns_bucket[5m])) by (le)) / 1000',
                   'tick p99'),
            target('histogram_quantile(0.50, sum(rate(strategy_tick_to_order_ns_bucket[5m])) by (le)) / 1000',
                   'order p50'),
            target('histogram_quantile(0.99, sum(rate(strategy_tick_to_order_ns_bucket[5m])) by (le)) / 1000',
                   'order p99'),
        ],
        x=12, y=17, w=12, h=8,
    ),
]


# ── Row 5: Errors (y=25) ──────────────────────────────────────────────

error_panels = [
    graph(
        "Stale orders",
        [target('sum(rate(order_gateway_stale_orders_total[5m]))', 'rate/s')],
        x=0, y=25, w=8, h=7, unit=OPS_FORMAT,
    ),
    graph(
        "Risk rejects",
        [target('sum(rate(order_gateway_risk_rejects_total[5m])) by (reason)', '{{reason}}')],
        x=8, y=25, w=8, h=7, unit=OPS_FORMAT,
    ),
    graph(
        "MD validation drops (md-gw)",
        [target('sum(rate(md_gateway_md_validation_drops_total[5m])) by (exchange)',
                '{{exchange}}')],
        x=16, y=25, w=8, h=7, unit=OPS_FORMAT,
    ),
]


# ── Dashboard ─────────────────────────────────────────────────────────

dashboard = Dashboard(
    title="BPT System Overview",
    description="Operational health for refdata / md-gw / order-gw / strategy.",
    tags=["bpt", "ops"],
    timezone="browser",
    refresh="10s",
    time=Time("now-15m", "now"),
    templating=Templating(list=[]),
    panels=(
        health_panels
        + connectivity_panels
        + activity_panels
        + latency_panels
        + error_panels
    ),
    uid="bpt-system-overview",
    version=1,
    schemaVersion=30,
).auto_panel_ids()
