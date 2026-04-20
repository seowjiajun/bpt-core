package bifrost;

import static org.junit.jupiter.api.Assertions.*;

import java.util.List;
import org.junit.jupiter.api.Test;

public class ConfigValidatorTest {

  @Test
  public void testValidConfig() {
    EffectiveConfig config = makeConfig("/dev/shm/test", "SHARED", "BUSY_SPIN", false);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertTrue(errors.isEmpty(), "Valid config should produce no errors");
  }

  @Test
  public void testEmptyDirectory() {
    EffectiveConfig config = makeConfig("", "SHARED", "BUSY_SPIN", false);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertEquals(1, errors.size());
    assertTrue(errors.get(0).contains("aeronDirectory"));
  }

  @Test
  public void testNullDirectory() {
    EffectiveConfig config = makeConfig(null, "SHARED", "BUSY_SPIN", false);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertEquals(1, errors.size());
    assertTrue(errors.get(0).contains("aeronDirectory"));
  }

  @Test
  public void testInvalidThreadingMode() {
    EffectiveConfig config = makeConfig("/dev/shm/test", "FOO", "BUSY_SPIN", false);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertEquals(1, errors.size());
    assertTrue(errors.get(0).contains("threadingMode"));
  }

  @Test
  public void testInvalidIdleStrategy() {
    EffectiveConfig config = makeConfig("/dev/shm/test", "SHARED", "MAGICAL", false);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertEquals(1, errors.size());
    assertTrue(errors.get(0).contains("sharedIdleStrategy"));
  }

  @Test
  public void testProdSafetyBlocked() {
    EffectiveConfig config = makeConfig("/dev/shm/test", "SHARED", "BUSY_SPIN", true);
    List<String> errors = ConfigValidator.validate(config, "PROD", false);
    assertEquals(1, errors.size());
    assertTrue(errors.get(0).contains("PROD"));
  }

  @Test
  public void testProdSafetyWithForceOverride() {
    EffectiveConfig config = makeConfig("/dev/shm/test", "SHARED", "BUSY_SPIN", true);
    List<String> errors = ConfigValidator.validate(config, "PROD", true);
    assertTrue(errors.isEmpty(), "PROD + force-delete-on-start should pass");
  }

  @Test
  public void testMultipleErrors() {
    EffectiveConfig config = makeConfig("", "FOO", "INVALID", false);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertEquals(3, errors.size(), "Should have errors for dir, threading, and idle strategy");
  }

  @Test
  public void testTopologyCoreOutOfRange() {
    int nproc = Runtime.getRuntime().availableProcessors();
    EffectiveConfig config = withTopology(nproc + 10, -1, -1);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertTrue(
        errors.stream().anyMatch(s -> s.contains("conductor_core") && s.contains("exceeds")),
        "Out-of-range core should be flagged; got: " + errors);
  }

  @Test
  public void testTopologyDuplicateCoresRejected() {
    EffectiveConfig config = withTopology(2, 2, -1);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertTrue(
        errors.stream().anyMatch(s -> s.contains("assigned to both")),
        "Duplicate core assignment should be flagged; got: " + errors);
  }

  @Test
  public void testTopologyAllUnpinnedIsValid() {
    EffectiveConfig config = withTopology(-1, -1, -1);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertTrue(errors.isEmpty(), "Fully-unpinned topology should pass validation");
  }

  @Test
  public void testTopologyDistinctCoresIsValid() {
    // Use low cores that exist on any reasonable host
    EffectiveConfig config = withTopology(0, 1, 2);
    List<String> errors = ConfigValidator.validate(config, "DEV", false);
    assertTrue(
        errors.isEmpty(),
        "Topology with three distinct in-range cores should pass; got: " + errors);
  }

  private static EffectiveConfig withTopology(int conductor, int sender, int receiver) {
    return new EffectiveConfig(
        "/dev/shm/test",
        "DEDICATED",
        "BUSY_SPIN",
        null,
        null,
        null,
        false,
        true,
        null,
        null,
        null,
        null,
        null,
        30L,
        0,
        conductor,
        sender,
        receiver);
  }

  private static EffectiveConfig makeConfig(
      String dir, String threading, String idle, boolean deleteOnStart) {
    return new EffectiveConfig(
        dir,
        threading,
        idle,
        null,
        null,
        null,
        deleteOnStart,
        true,
        null,
        null,
        null,
        null,
        null,
        30L,
        0,
        -1,
        -1,
        -1);
  }
}
