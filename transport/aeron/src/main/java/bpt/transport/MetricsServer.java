package bpt.transport;

import io.prometheus.metrics.core.metrics.Gauge;
import io.prometheus.metrics.exporter.httpserver.HTTPServer;
import io.prometheus.metrics.instrumentation.jvm.JvmMetrics;
import java.io.IOException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Starts a Prometheus HTTP metrics server and registers JVM metrics. Port 0 disables the server
 * (useful in tests).
 */
public class MetricsServer implements AutoCloseable {

  private static final Logger LOGGER = LoggerFactory.getLogger(MetricsServer.class);

  private final HTTPServer server;
  private final Gauge healthyGauge;

  public MetricsServer(int port) throws IOException {
    if (port <= 0) {
      this.server = null;
      this.healthyGauge = null;
      return;
    }

    JvmMetrics.builder().register();

    this.healthyGauge =
        Gauge.builder()
            .name("bpt_transport_healthy")
            .help("1 if bpt-transport MediaDriver is running")
            .register();
    this.healthyGauge.set(1.0);

    this.server = HTTPServer.builder().port(port).buildAndStart();
    LOGGER.info("Metrics server started on :{}/metrics", port);
  }

  @Override
  public void close() {
    if (healthyGauge != null) healthyGauge.set(0.0);
    if (server != null) server.close();
  }
}
