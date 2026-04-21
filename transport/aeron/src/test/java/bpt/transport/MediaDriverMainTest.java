package bpt.transport;

import static org.junit.jupiter.api.Assertions.*;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import org.junit.jupiter.api.Test;

/** Integration tests for the full CLI → validate pipeline via {@link MediaDriverMain#call()}. */
public class MediaDriverMainTest {

  @Test
  public void testValidationOnlyFailures() throws Exception {
    File configFile = createTempConfig("aeron:\n  directory: ''\n");
    MediaDriverMain driver = new MediaDriverMain();
    driver.setConfigPath(configFile.getAbsolutePath());
    driver.setValidateOnly(true);

    assertEquals(2, driver.call(), "Empty directory should return exit code 2");

    configFile = createTempConfig("aeron:\n  directory: '/tmp/test'\n  threading_mode: 'FOO'\n");
    driver.setConfigPath(configFile.getAbsolutePath());
    assertEquals(2, driver.call(), "Invalid threading mode should return exit code 2");

    configFile = createTempConfig("aeron:\n  directory: '/tmp/test'\n  idle_strategy: 'INVALID'\n");
    driver.setConfigPath(configFile.getAbsolutePath());
    assertEquals(2, driver.call(), "Invalid idle strategy should return exit code 2");
  }

  @Test
  public void testProdSafety() throws Exception {
    File configFile =
        createTempConfig("aeron:\n  directory: '/tmp/test'\n  dir_delete_on_start: true\n");
    MediaDriverMain driver = new MediaDriverMain();
    driver.setConfigPath(configFile.getAbsolutePath());
    driver.setValidateOnly(true);
    driver.setEnv("PROD");

    assertEquals(2, driver.call(), "PROD + dirDeleteOnStart without override should fail");

    driver.setForceDeleteOnStart(true);
    assertEquals(0, driver.call(), "PROD + dirDeleteOnStart WITH override should pass");
  }

  @Test
  public void testPrintEffectiveConfig() throws Exception {
    File configFile = createTempConfig("aeron:\n  directory: '/tmp/test'\n");
    MediaDriverMain driver = new MediaDriverMain();
    driver.setConfigPath(configFile.getAbsolutePath());
    driver.setPrintEffectiveConfig(true);

    assertEquals(0, driver.call(), "Print effective config should return exit code 0");
  }

  @Test
  public void testConfigLoadFailure() {
    MediaDriverMain driver = new MediaDriverMain();
    driver.setConfigPath("/nonexistent/config.yaml");
    driver.setValidateOnly(true);

    assertEquals(2, driver.call(), "Missing config file should return exit code 2");
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
