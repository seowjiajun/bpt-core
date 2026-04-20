package bifrost;

import io.aeron.driver.MediaDriver;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.agrona.concurrent.IdleStrategy;
import org.agrona.concurrent.ShutdownSignalBarrier;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Manages the full Aeron {@link MediaDriver} lifecycle: context construction, launch, heartbeat
 * monitoring, and graceful shutdown.
 */
public class MediaDriverRunner {

  private static final Logger LOGGER = LoggerFactory.getLogger(MediaDriverRunner.class);

  /**
   * Builds a {@link MediaDriver.Context}, launches the driver, schedules a heartbeat, and blocks
   * until a shutdown signal is received.
   *
   * @param config the effective configuration
   * @return 0 on clean shutdown, 1 on fatal error, 2 on setup failure
   */
  public int run(EffectiveConfig config) {
    long startTimeMs = System.currentTimeMillis();

    // ── Ensure aeron directory exists ────────────────────────────
    Path aeronPath = Paths.get(config.aeronDirectory());
    boolean exists = Files.exists(aeronPath);
    LOGGER.info("Aeron Directory: {} (Exists: {})", aeronPath.toAbsolutePath(), exists);

    if (!exists) {
      try {
        Files.createDirectories(aeronPath);
        LOGGER.info("Created Aeron Directory: {}", aeronPath.toAbsolutePath());
      } catch (IOException e) {
        LOGGER.error(
            "Failed to create Aeron Directory '{}': {}",
            aeronPath.toAbsolutePath(),
            e.getMessage());
        return 2;
      }
    }

    if (config.dirDeleteOnStart()) {
      LOGGER.warn(
          "dirDeleteOnStart=true: existing Aeron directory contents will be deleted on launch");
    }

    // ── Build MediaDriver.Context ────────────────────────────────
    MediaDriver.Context ctx = buildContext(config);

    // ── Heartbeat executor ───────────────────────────────────────
    ScheduledExecutorService heartbeatExecutor =
        Executors.newSingleThreadScheduledExecutor(
            r -> {
              Thread t = new Thread(r, "bifrost-heartbeat");
              t.setDaemon(true);
              return t;
            });

    // AtomicBoolean prevents double-shutdown between the main thread and the shutdown hook
    AtomicBoolean shutdownInitiated = new AtomicBoolean(false);

    // ── Metrics server ───────────────────────────────────────────
    MetricsServer metricsServer = null;
    try {
      metricsServer = new MetricsServer(config.metricsPort());
    } catch (IOException e) {
      LOGGER.warn(
          "Failed to start metrics server on port {}: {}", config.metricsPort(), e.getMessage());
    }
    final MetricsServer finalMetricsServer = metricsServer;

    // ── Shutdown hook ────────────────────────────────────────────
    Runtime.getRuntime()
        .addShutdownHook(
            new Thread(
                () -> {
                  long uptimeSec = (System.currentTimeMillis() - startTimeMs) / 1000;
                  LOGGER.info(
                      "Shutdown hook fired (uptime={}s, aeronDir={})",
                      uptimeSec,
                      config.aeronDirectory());
                  if (shutdownInitiated.compareAndSet(false, true)) {
                    if (finalMetricsServer != null) finalMetricsServer.close();
                    heartbeatExecutor.shutdownNow();
                  }
                },
                "bifrost-shutdown-hook"));

    // ── Launch ───────────────────────────────────────────────────
    try (MediaDriver driver = MediaDriver.launch(ctx)) {
      LOGGER.info("MediaDriver launched successfully.");

      long intervalSec = config.heartbeatIntervalSec();
      heartbeatExecutor.scheduleAtFixedRate(
          () -> {
            long uptime = (System.currentTimeMillis() - startTimeMs) / 1000;
            LOGGER.info(
                "MediaDriver alive (uptime={}s, aeronDir={}, threading={}, idle={})",
                uptime,
                config.aeronDirectory(),
                config.threadingMode() != null ? config.threadingMode() : "DEFAULT",
                config.sharedIdleStrategy() != null ? config.sharedIdleStrategy() : "DEFAULT");
          },
          intervalSec,
          intervalSec,
          TimeUnit.SECONDS);

      new ShutdownSignalBarrier().await();
      LOGGER.info("Shutting down MediaDriver...");
      if (shutdownInitiated.compareAndSet(false, true)) {
        if (metricsServer != null) metricsServer.close();
        heartbeatExecutor.shutdownNow();
      }
      return 0;
    } catch (Exception | Error ex) {
      // Catch Error (e.g. ExceptionInInitializerError from native init) as a fatal top-level
      // failure so callers receive a non-zero exit code rather than an uncaught throwable
      LOGGER.error("Fatal exception during MediaDriver execution", ex);
      if (shutdownInitiated.compareAndSet(false, true)) {
        if (metricsServer != null) metricsServer.close();
        heartbeatExecutor.shutdownNow();
      }
      return 1;
    }
  }

  // ── Context construction ─────────────────────────────────────────

  /**
   * Constructs a {@link MediaDriver.Context} from the given configuration.
   *
   * <p>Idle strategy precedence per thread role:
   *
   * <ol>
   *   <li>Per-thread override (conductor / receiver / sender), if configured
   *   <li>Shared idle strategy, as fallback
   *   <li>Aeron defaults if all are null
   * </ol>
   */
  static MediaDriver.Context buildContext(EffectiveConfig config) {
    MediaDriver.Context ctx =
        new MediaDriver.Context()
            .aeronDirectoryName(config.aeronDirectory())
            .dirDeleteOnStart(config.dirDeleteOnStart())
            .dirDeleteOnShutdown(config.dirDeleteOnShutdown());

    if (config.threadingMode() != null) {
      ctx.threadingMode(IdleStrategyParser.parseThreadingMode(config.threadingMode()));
    }

    IdleStrategy sharedIdle = IdleStrategyParser.parseIdleStrategy(config.sharedIdleStrategy());
    if (sharedIdle != null) {
      ctx.sharedIdleStrategy(sharedIdle);
    }

    ctx.conductorIdleStrategy(
        config.conductorIdleStrategy() != null
            ? IdleStrategyParser.parseIdleStrategy(config.conductorIdleStrategy())
            : sharedIdle);

    ctx.receiverIdleStrategy(
        config.receiverIdleStrategy() != null
            ? IdleStrategyParser.parseIdleStrategy(config.receiverIdleStrategy())
            : sharedIdle);

    ctx.senderIdleStrategy(
        config.senderIdleStrategy() != null
            ? IdleStrategyParser.parseIdleStrategy(config.senderIdleStrategy())
            : sharedIdle);

    if (config.driverTimeoutMs() != null) ctx.driverTimeoutMs(config.driverTimeoutMs());
    if (config.clientLivenessTimeoutNs() != null)
      ctx.clientLivenessTimeoutNs(config.clientLivenessTimeoutNs());
    if (config.publicationUnblockTimeoutNs() != null)
      ctx.publicationUnblockTimeoutNs(config.publicationUnblockTimeoutNs());
    if (config.termBufferLength() != null) ctx.ipcTermBufferLength(config.termBufferLength());
    if (config.mtuLength() != null) ctx.mtuLength(config.mtuLength());

    // Per-thread CPU affinity for the three DEDICATED-mode agent threads.
    // Aeron Context exposes per-role ThreadFactory hooks; AffinityThreadFactory
    // sets the OS thread name and pins via net.openhft.affinity on each new
    // thread's run(). coreId == -1 means unpinned (dev-laptop default).
    ctx.conductorThreadFactory(
        new AffinityThreadFactory("aeron-conductor", config.conductorCore()));
    ctx.senderThreadFactory(new AffinityThreadFactory("aeron-sender", config.senderCore()));
    ctx.receiverThreadFactory(new AffinityThreadFactory("aeron-receiver", config.receiverCore()));

    return ctx;
  }
}
