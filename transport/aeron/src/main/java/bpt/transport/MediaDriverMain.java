package bpt.transport;

import ch.qos.logback.classic.Level;
import ch.qos.logback.classic.LoggerContext;
import java.io.InputStream;
import java.util.List;
import java.util.Properties;
import java.util.concurrent.Callable;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import picocli.CommandLine;
import picocli.CommandLine.Command;
import picocli.CommandLine.Option;

/** CLI entry point for launching an Aeron Media Driver with YAML-based configuration. */
@Command(
    name = "MediaDriverMain",
    mixinStandardHelpOptions = true,
    description = "Launches an Aeron Media Driver.",
    versionProvider = MediaDriverMain.VersionProvider.class)
public class MediaDriverMain implements Callable<Integer> {

  private static final Logger LOGGER = LoggerFactory.getLogger(MediaDriverMain.class);

  @Option(
      names = {"-c", "--config"},
      required = true,
      description = "Path to config YAML")
  private String configPath;

  @Option(
      names = "--env",
      description = "Environment mode (DEV, QA, PROD). Default: DEV",
      defaultValue = "DEV")
  private String env = "DEV";

  @Option(names = "--force-delete-on-start", description = "Force directory deletion on start")
  private boolean forceDeleteOnStart;

  @Option(
      names = "--force-delete-on-shutdown",
      description = "Force directory deletion on shutdown")
  private boolean forceDeleteOnShutdown;

  @Option(
      names = "--validate-only",
      description = "Loads and validates config, exits 0 if OK, 2 if invalid.")
  private boolean validateOnly;

  @Option(names = "--print-effective-config", description = "Prints merged config and exits.")
  private boolean printEffectiveConfig;

  @Option(names = "--print-config", hidden = true, description = "Alias for print-effective-config")
  private boolean printConfigOld;

  public static void main(String[] args) {
    int exitCode = new CommandLine(new MediaDriverMain()).execute(args);
    System.exit(exitCode);
  }

  /** Provides the application version from the build-generated version.properties file. */
  static class VersionProvider implements CommandLine.IVersionProvider {
    @Override
    public String[] getVersion() {
      try (InputStream is = getClass().getResourceAsStream("/version.properties")) {
        if (is != null) {
          Properties props = new Properties();
          props.load(is);
          return new String[] {"bpt-transport " + props.getProperty("version", "unknown")};
        }
      } catch (Exception ignored) {
        // fall through
      }
      return new String[] {"bpt-transport version unknown"};
    }
  }

  @Override
  public Integer call() {
    if (printConfigOld) printEffectiveConfig = true;

    // ── Load config ──────────────────────────────────────────────
    Config rawConfig;
    try {
      rawConfig = Config.load(configPath);
    } catch (Exception e) {
      LOGGER.error("Error loading config '{}': {}", configPath, e.getMessage());
      LOGGER.debug("Config load failure detail", e);
      return 2;
    }

    applyLogLevel(rawConfig.logLevel);

    // ── Merge with CLI overrides ─────────────────────────────────
    EffectiveConfig merged =
        EffectiveConfig.from(rawConfig, forceDeleteOnStart, forceDeleteOnShutdown);

    // ── Validate ─────────────────────────────────────────────────
    List<String> errors = ConfigValidator.validate(merged, env, forceDeleteOnStart);
    if (!errors.isEmpty()) {
      errors.forEach(e -> LOGGER.error("Validation Error: {}", e));
      return 2;
    }

    // ── Early-exit modes ─────────────────────────────────────────
    if (printEffectiveConfig) {
      printEffectiveConfig(merged);
      return 0;
    }

    if (validateOnly) {
      LOGGER.info("Configuration validation successful.");
      return 0;
    }

    // ── Run the driver ───────────────────────────────────────────
    return new MediaDriverRunner().run(merged);
  }

  private void printEffectiveConfig(EffectiveConfig c) {
    LOGGER.info("Effective Configuration:");
    LOGGER.info("  env                       = {}", env);
    LOGGER.info("  aeronDirectory            = {}", c.aeronDirectory());
    LOGGER.info("  dirDeleteOnStart          = {}", c.dirDeleteOnStart());
    LOGGER.info("  dirDeleteOnShutdown       = {}", c.dirDeleteOnShutdown());
    LOGGER.info("  threadingMode             = {}", c.threadingMode());
    LOGGER.info("  sharedIdleStrategy        = {}", c.sharedIdleStrategy());
    LOGGER.info("  conductorIdleStrategy     = {}", c.conductorIdleStrategy());
    LOGGER.info("  receiverIdleStrategy      = {}", c.receiverIdleStrategy());
    LOGGER.info("  senderIdleStrategy        = {}", c.senderIdleStrategy());
    LOGGER.info("  heartbeatIntervalSec      = {}", c.heartbeatIntervalSec());
    LOGGER.info("  driverTimeoutMs           = {}", c.driverTimeoutMs());
    LOGGER.info("  clientLivenessTimeoutNs   = {}", c.clientLivenessTimeoutNs());
    LOGGER.info("  termBufferLength          = {}", c.termBufferLength());
    LOGGER.info("  mtuLength                 = {}", c.mtuLength());
  }

  private void applyLogLevel(String logLevel) {
    if (logLevel == null || logLevel.isBlank()) return;
    try {
      if (!(LoggerFactory.getILoggerFactory() instanceof LoggerContext loggerContext)) {
        LOGGER.warn(
            "Log level '{}' not applied: logback is not the active logging implementation",
            logLevel);
        return;
      }
      ch.qos.logback.classic.Logger rootLogger =
          loggerContext.getLogger(org.slf4j.Logger.ROOT_LOGGER_NAME);
      rootLogger.setLevel(Level.toLevel(logLevel, Level.INFO));
      LOGGER.debug("Log level set to: {}", logLevel);
    } catch (Exception e) {
      LOGGER.warn("Could not apply log level '{}'", logLevel);
      LOGGER.debug("Log level application failure", e);
    }
  }

  // ── Test helpers ───────────────────────────────────────────────

  public void setConfigPath(String cp) {
    this.configPath = cp;
  }

  public void setEnv(String env) {
    this.env = env;
  }

  public void setForceDeleteOnStart(boolean b) {
    this.forceDeleteOnStart = b;
  }

  public void setForceDeleteOnShutdown(boolean b) {
    this.forceDeleteOnShutdown = b;
  }

  public void setValidateOnly(boolean b) {
    this.validateOnly = b;
  }

  public void setPrintEffectiveConfig(boolean b) {
    this.printEffectiveConfig = b;
  }
}
