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
        0);
  }
}
