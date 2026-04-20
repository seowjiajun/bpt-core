package bifrost;

import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.Map;
import org.yaml.snakeyaml.LoaderOptions;
import org.yaml.snakeyaml.Yaml;
import org.yaml.snakeyaml.constructor.SafeConstructor;
import org.yaml.snakeyaml.error.YAMLException;

public class Config {

  // ── Default values ──────────────────────────────────────────────
  static final String DEFAULT_AERON_DIRECTORY = "/dev/shm/aeron-bifrost";
  static final String DEFAULT_THREADING_MODE = "SHARED";
  static final String DEFAULT_IDLE_STRATEGY = "BUSY_SPIN";
  static final boolean DEFAULT_DIR_DELETE_ON_START = false;
  static final boolean DEFAULT_DIR_DELETE_ON_SHUTDOWN = true;
  static final String DEFAULT_LOG_LEVEL = "info";
  // BUSY_SPIN default: lowest latency, highest CPU; SHARED threading: single-threaded simplicity
  static final long DEFAULT_HEARTBEAT_INTERVAL_SEC = 30L;

  // ── Fields ──────────────────────────────────────────────────────
  public final String aeronDirectory;
  public final String threadingMode;
  public final String idleStrategy;
  public final boolean dirDeleteOnStart;
  public final boolean dirDeleteOnShutdown;
  public final String logLevel;
  public final long heartbeatIntervalSec;

  public final Long driverTimeoutMs;
  public final Long clientLivenessTimeoutNs;
  public final Long publicationUnblockTimeoutNs;
  public final Integer termBufferLength;
  public final Integer mtuLength;

  public final String conductorIdleStrategy;
  public final String receiverIdleStrategy;
  public final String senderIdleStrategy;

  public final int metricsPort;

  // CPU affinity assignments for the three Aeron MediaDriver agent
  // threads when DEDICATED threading mode is active. -1 means unpinned.
  // Role vocabulary matches bpt-core's topology.toml: aeron.conductor,
  // aeron.sender, aeron.receiver. Operator keeps these in sync with the
  // C++ services' topology.toml so both ends of the IPC agree.
  public final int conductorCore;
  public final int senderCore;
  public final int receiverCore;

  @SuppressWarnings("unchecked") // YAML parser returns raw Map; inner keys are always String
  private Config(Map<String, Object> data) {
    Object aeronRaw = data != null ? data.get("aeron") : null;
    Map<String, Object> aeron = aeronRaw instanceof Map ? (Map<String, Object>) aeronRaw : null;

    this.aeronDirectory = getString(aeron, "directory", DEFAULT_AERON_DIRECTORY);
    this.threadingMode = getString(aeron, "threading_mode", DEFAULT_THREADING_MODE);
    this.idleStrategy = getString(aeron, "idle_strategy", DEFAULT_IDLE_STRATEGY);
    this.dirDeleteOnStart = getBoolean(aeron, "dir_delete_on_start", DEFAULT_DIR_DELETE_ON_START);
    this.dirDeleteOnShutdown =
        getBoolean(aeron, "dir_delete_on_shutdown", DEFAULT_DIR_DELETE_ON_SHUTDOWN);
    this.driverTimeoutMs = getLong(aeron, "driver_timeout_ms");
    this.clientLivenessTimeoutNs = getLong(aeron, "client_liveness_timeout_ns");
    this.publicationUnblockTimeoutNs = getLong(aeron, "publication_unblock_timeout_ns");
    this.termBufferLength = getInteger(aeron, "term_buffer_length");
    this.mtuLength = getInteger(aeron, "mtu_length");
    this.conductorIdleStrategy = getString(aeron, "conductor_idle_strategy", null);
    this.receiverIdleStrategy = getString(aeron, "receiver_idle_strategy", null);
    this.senderIdleStrategy = getString(aeron, "sender_idle_strategy", null);

    Long heartbeat = getLong(aeron, "heartbeat_interval_sec");
    this.heartbeatIntervalSec = heartbeat != null ? heartbeat : DEFAULT_HEARTBEAT_INTERVAL_SEC;

    Object loggerRaw = data != null ? data.get("logger") : null;
    Map<String, Object> logger = loggerRaw instanceof Map ? (Map<String, Object>) loggerRaw : null;
    this.logLevel = getString(logger, "level", DEFAULT_LOG_LEVEL);

    Object metricsRaw = data != null ? data.get("metrics") : null;
    Map<String, Object> metrics =
        metricsRaw instanceof Map ? (Map<String, Object>) metricsRaw : null;
    Integer metricsPort = getInteger(metrics, "port");
    this.metricsPort = metricsPort != null ? metricsPort : 9100;

    // topology: { conductor_core, sender_core, receiver_core }
    // Missing = -1 = unpinned (dev-laptop default).
    Object topologyRaw = data != null ? data.get("topology") : null;
    Map<String, Object> topology =
        topologyRaw instanceof Map ? (Map<String, Object>) topologyRaw : null;
    Integer cc = getInteger(topology, "conductor_core");
    Integer sc = getInteger(topology, "sender_core");
    Integer rc = getInteger(topology, "receiver_core");
    this.conductorCore = cc != null ? cc : -1;
    this.senderCore = sc != null ? sc : -1;
    this.receiverCore = rc != null ? rc : -1;
  }

  public static Config load(String path) {
    try (InputStream inputStream = new FileInputStream(path)) {
      // SafeConstructor prevents arbitrary Java object deserialization (RCE via YAML gadgets)
      Yaml yaml = new Yaml(new SafeConstructor(new LoaderOptions()));
      Map<String, Object> data = yaml.load(inputStream);
      // yaml.load() returns null for empty files; constructor handles null gracefully
      return new Config(data);
    } catch (FileNotFoundException e) {
      throw new RuntimeException("Config file not found: " + path, e);
    } catch (YAMLException e) {
      throw new RuntimeException("Invalid YAML in config file: " + path, e);
    } catch (IOException e) {
      throw new RuntimeException("Failed to read config file: " + path, e);
    }
  }

  @Override
  public String toString() {
    return "Config{"
        + "aeronDirectory='"
        + aeronDirectory
        + '\''
        + ", threadingMode='"
        + threadingMode
        + '\''
        + ", idleStrategy='"
        + idleStrategy
        + '\''
        + ", dirDeleteOnStart="
        + dirDeleteOnStart
        + ", dirDeleteOnShutdown="
        + dirDeleteOnShutdown
        + ", logLevel='"
        + logLevel
        + '\''
        + ", heartbeatIntervalSec="
        + heartbeatIntervalSec
        + ", driverTimeoutMs="
        + driverTimeoutMs
        + ", clientLivenessTimeoutNs="
        + clientLivenessTimeoutNs
        + ", publicationUnblockTimeoutNs="
        + publicationUnblockTimeoutNs
        + ", termBufferLength="
        + termBufferLength
        + ", mtuLength="
        + mtuLength
        + ", conductorIdleStrategy='"
        + conductorIdleStrategy
        + '\''
        + ", receiverIdleStrategy='"
        + receiverIdleStrategy
        + '\''
        + ", senderIdleStrategy='"
        + senderIdleStrategy
        + '\''
        + '}';
  }

  // ── Helper methods ──────────────────────────────────────────────

  private static String getString(Map<String, Object> map, String key, String defaultValue) {
    if (map == null || !map.containsKey(key)) return defaultValue;
    Object value = map.get(key);
    if (value == null) return defaultValue;
    if (value instanceof String) return (String) value;
    // Coerce non-string scalars (e.g., YAML parsed a number where a string was expected)
    return String.valueOf(value);
  }

  private static boolean getBoolean(Map<String, Object> map, String key, boolean defaultValue) {
    if (map == null || !map.containsKey(key)) return defaultValue;
    Object value = map.get(key);
    if (value == null) return defaultValue;
    if (value instanceof Boolean) return (Boolean) value;
    if (value instanceof String) {
      String s = ((String) value).trim().toLowerCase(Locale.ROOT);
      if ("true".equals(s)) return true;
      if ("false".equals(s)) return false;
    }
    throw new IllegalArgumentException(
        "Expected boolean for key '" + key + "', got: " + value.getClass().getSimpleName());
  }

  private static Long getLong(Map<String, Object> map, String key) {
    if (map == null || !map.containsKey(key)) return null;
    Object value = map.get(key);
    if (value == null) return null;
    if (value instanceof Number) return ((Number) value).longValue();
    if (value instanceof String) {
      try {
        return Long.parseLong(((String) value).trim());
      } catch (NumberFormatException e) {
        throw new IllegalArgumentException(
            "Expected integer for key '" + key + "', got: \"" + value + '"');
      }
    }
    throw new IllegalArgumentException(
        "Expected integer for key '" + key + "', got: " + value.getClass().getSimpleName());
  }

  private static Integer getInteger(Map<String, Object> map, String key) {
    if (map == null || !map.containsKey(key)) return null;
    Object value = map.get(key);
    if (value == null) return null;
    if (value instanceof Number) return ((Number) value).intValue();
    if (value instanceof String) {
      try {
        return Integer.parseInt(((String) value).trim());
      } catch (NumberFormatException e) {
        throw new IllegalArgumentException(
            "Expected integer for key '" + key + "', got: \"" + value + '"');
      }
    }
    throw new IllegalArgumentException(
        "Expected integer for key '" + key + "', got: " + value.getClass().getSimpleName());
  }
}
