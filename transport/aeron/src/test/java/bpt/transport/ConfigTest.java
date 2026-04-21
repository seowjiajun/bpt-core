package bpt.transport;

import static org.junit.jupiter.api.Assertions.*;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import org.junit.jupiter.api.Test;

public class ConfigTest {

  @Test
  public void testLoadValidConfig() throws Exception {
    File configFile =
        createTempConfig(
            "aeron:\n"
                + "  directory: '/dev/shm/test'\n"
                + "  threading_mode: 'DEDICATED'\n"
                + "  idle_strategy: 'YIELDING'\n"
                + "  dir_delete_on_start: true\n"
                + "  dir_delete_on_shutdown: false\n"
                + "  driver_timeout_ms: 5000\n"
                + "  client_liveness_timeout_ns: 10000000\n"
                + "  term_buffer_length: 65536\n"
                + "  mtu_length: 1408\n"
                + "  conductor_idle_strategy: 'SLEEPING:500'\n"
                + "  receiver_idle_strategy: 'BUSY_SPIN'\n"
                + "  sender_idle_strategy: 'YIELDING'\n"
                + "  heartbeat_interval_sec: 60\n"
                + "logger:\n"
                + "  level: 'debug'\n");

    Config config = Config.load(configFile.getAbsolutePath());

    assertEquals("/dev/shm/test", config.aeronDirectory);
    assertEquals("DEDICATED", config.threadingMode);
    assertEquals("YIELDING", config.idleStrategy);
    assertTrue(config.dirDeleteOnStart);
    assertFalse(config.dirDeleteOnShutdown);
    assertEquals(5000L, config.driverTimeoutMs);
    assertEquals(10000000L, config.clientLivenessTimeoutNs);
    assertEquals(65536, config.termBufferLength);
    assertEquals(1408, config.mtuLength);
    assertEquals("SLEEPING:500", config.conductorIdleStrategy);
    assertEquals("BUSY_SPIN", config.receiverIdleStrategy);
    assertEquals("YIELDING", config.senderIdleStrategy);
    assertEquals(60L, config.heartbeatIntervalSec);
    assertEquals("debug", config.logLevel);
  }

  @Test
  public void testLoadTopologyAssignments() throws Exception {
    File configFile =
        createTempConfig(
            "aeron:\n"
                + "  directory: '/dev/shm/t'\n"
                + "topology:\n"
                + "  conductor_core: 1\n"
                + "  sender_core: 2\n"
                + "  receiver_core: 3\n");

    Config config = Config.load(configFile.getAbsolutePath());
    assertEquals(1, config.conductorCore);
    assertEquals(2, config.senderCore);
    assertEquals(3, config.receiverCore);
  }

  @Test
  public void testTopologyDefaultsToUnpinned() throws Exception {
    // Absent topology block → all roles -1 (dev-laptop default).
    File configFile = createTempConfig("aeron:\n  directory: '/dev/shm/t'\n");
    Config config = Config.load(configFile.getAbsolutePath());
    assertEquals(-1, config.conductorCore);
    assertEquals(-1, config.senderCore);
    assertEquals(-1, config.receiverCore);
  }

  @Test
  public void testDefaultsWhenEmptyFile() throws Exception {
    File configFile = createTempConfig("");
    Config config = Config.load(configFile.getAbsolutePath());

    assertEquals(Config.DEFAULT_AERON_DIRECTORY, config.aeronDirectory);
    assertEquals(Config.DEFAULT_THREADING_MODE, config.threadingMode);
    assertEquals(Config.DEFAULT_IDLE_STRATEGY, config.idleStrategy);
    assertEquals(Config.DEFAULT_DIR_DELETE_ON_START, config.dirDeleteOnStart);
    assertEquals(Config.DEFAULT_DIR_DELETE_ON_SHUTDOWN, config.dirDeleteOnShutdown);
    assertEquals(Config.DEFAULT_LOG_LEVEL, config.logLevel);
    assertEquals(Config.DEFAULT_HEARTBEAT_INTERVAL_SEC, config.heartbeatIntervalSec);
    assertNull(config.driverTimeoutMs);
    assertNull(config.clientLivenessTimeoutNs);
    assertNull(config.termBufferLength);
    assertNull(config.mtuLength);
    assertNull(config.conductorIdleStrategy);
    assertNull(config.receiverIdleStrategy);
    assertNull(config.senderIdleStrategy);
  }

  @Test
  public void testDefaultsWhenAeronKeyMissing() throws Exception {
    File configFile = createTempConfig("logger:\n  level: 'warn'\n");
    Config config = Config.load(configFile.getAbsolutePath());

    assertEquals(Config.DEFAULT_AERON_DIRECTORY, config.aeronDirectory);
    assertEquals(Config.DEFAULT_THREADING_MODE, config.threadingMode);
    assertEquals(Config.DEFAULT_IDLE_STRATEGY, config.idleStrategy);
    assertEquals(Config.DEFAULT_HEARTBEAT_INTERVAL_SEC, config.heartbeatIntervalSec);
    assertEquals("warn", config.logLevel);
  }

  @Test
  public void testPartialAeronConfig() throws Exception {
    File configFile = createTempConfig("aeron:\n  directory: '/tmp/custom'\n");
    Config config = Config.load(configFile.getAbsolutePath());

    assertEquals("/tmp/custom", config.aeronDirectory);
    assertEquals(Config.DEFAULT_THREADING_MODE, config.threadingMode);
    assertEquals(Config.DEFAULT_IDLE_STRATEGY, config.idleStrategy);
    assertEquals(Config.DEFAULT_DIR_DELETE_ON_START, config.dirDeleteOnStart);
    assertEquals(Config.DEFAULT_DIR_DELETE_ON_SHUTDOWN, config.dirDeleteOnShutdown);
  }

  @Test
  public void testFileNotFound() {
    RuntimeException ex =
        assertThrows(RuntimeException.class, () -> Config.load("/nonexistent/config.yaml"));
    assertTrue(
        ex.getMessage().contains("not found") || ex.getMessage().contains("nonexistent"),
        "Error message should mention the missing file");
  }

  @Test
  public void testInvalidYamlThrowsRuntimeException() throws Exception {
    File configFile = createTempConfig("aeron:\n  directory: ': broken yaml [[[");
    assertThrows(RuntimeException.class, () -> Config.load(configFile.getAbsolutePath()));
  }

  @Test
  public void testDirectoryAsNumberIsCoercedToString() throws Exception {
    // YAML may parse unquoted numbers as Integer; getString should coerce them
    File configFile = createTempConfig("aeron:\n  directory: 12345\n");
    Config config = Config.load(configFile.getAbsolutePath());
    assertEquals("12345", config.aeronDirectory);
  }

  @Test
  public void testBooleanAsStringIsAccepted() throws Exception {
    File configFile =
        createTempConfig(
            "aeron:\n" + "  dir_delete_on_start: 'true'\n" + "  dir_delete_on_shutdown: 'false'\n");
    Config config = Config.load(configFile.getAbsolutePath());
    assertTrue(config.dirDeleteOnStart);
    assertFalse(config.dirDeleteOnShutdown);
  }

  @Test
  public void testInvalidBooleanThrowsIllegalArgumentException() throws Exception {
    File configFile = createTempConfig("aeron:\n  dir_delete_on_start: maybe\n");
    assertThrows(IllegalArgumentException.class, () -> Config.load(configFile.getAbsolutePath()));
  }

  @Test
  public void testNumericFieldsAsStringsAreAccepted() throws Exception {
    File configFile =
        createTempConfig("aeron:\n" + "  driver_timeout_ms: '5000'\n" + "  mtu_length: '1408'\n");
    Config config = Config.load(configFile.getAbsolutePath());
    assertEquals(5000L, config.driverTimeoutMs);
    assertEquals(1408, config.mtuLength);
  }

  @Test
  public void testInvalidNumericFieldThrowsIllegalArgumentException() throws Exception {
    File configFile = createTempConfig("aeron:\n  driver_timeout_ms: 'not-a-number'\n");
    assertThrows(IllegalArgumentException.class, () -> Config.load(configFile.getAbsolutePath()));
  }

  @Test
  public void testToString() throws Exception {
    File configFile = createTempConfig("aeron:\n  directory: '/dev/shm/test'\n");
    Config config = Config.load(configFile.getAbsolutePath());
    String str = config.toString();

    assertTrue(str.contains("aeronDirectory='/dev/shm/test'"));
    assertTrue(str.startsWith("Config{"));
    assertTrue(str.endsWith("}"));
  }

  private File createTempConfig(String content) throws IOException {
    File temp = File.createTempFile("test-config", ".yaml");
    temp.deleteOnExit();
    try (FileWriter writer = new FileWriter(temp)) {
      writer.write(content);
    }
    return temp;
  }
}
