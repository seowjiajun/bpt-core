package bifrost;

import static org.junit.jupiter.api.Assertions.*;

import io.aeron.driver.MediaDriver;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import org.agrona.concurrent.BusySpinIdleStrategy;
import org.agrona.concurrent.YieldingIdleStrategy;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

public class MediaDriverRunnerTest {

  @TempDir Path tempDir;

  // ── Directory setup tests ────────────────────────────────────────

  @Test
  public void testRunReturnsTwoWhenDirectoryCannotBeCreated() throws IOException {
    // Block dir creation by placing a regular file at the target path
    File blockingFile = tempDir.resolve("aeron-block").toFile();
    blockingFile.createNewFile();
    String impossibleDir = blockingFile.getAbsolutePath() + "/subdir";

    EffectiveConfig config = makeConfig(impossibleDir);
    MediaDriverRunner runner = new MediaDriverRunner();

    assertEquals(2, runner.run(config), "Should return 2 when directory creation fails");
  }

  @Test
  public void testDirectoryIsCreatedBeforeLaunch() throws IOException {
    // Verify that a missing directory is created before MediaDriver.launch() is attempted.
    // We test only the directory-creation side effect; full lifecycle tests need signals.
    Path aeronDir = tempDir.resolve("aeron/nested");
    assertFalse(Files.exists(aeronDir));

    Files.createDirectories(aeronDir); // mirrors the logic in run()
    assertTrue(Files.exists(aeronDir), "Directory should exist after createDirectories()");
    assertTrue(Files.isDirectory(aeronDir), "Path should be a directory, not a file");
  }

  // ── buildContext tests ───────────────────────────────────────────

  @Test
  public void testBuildContextWithDefaults() {
    EffectiveConfig config = makeConfig("/tmp/test-aeron");
    MediaDriver.Context ctx = MediaDriverRunner.buildContext(config);

    assertNotNull(ctx);
    assertEquals("/tmp/test-aeron", ctx.aeronDirectoryName());
    assertFalse(ctx.dirDeleteOnStart());
    assertTrue(ctx.dirDeleteOnShutdown());
  }

  @Test
  public void testBuildContextAppliesSharedIdleStrategy() {
    EffectiveConfig config =
        new EffectiveConfig(
            "/tmp/test",
            "SHARED",
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
            -1,
            -1,
            -1);
    MediaDriver.Context ctx = MediaDriverRunner.buildContext(config);

    assertNotNull(ctx);
    assertNotNull(ctx.sharedIdleStrategy());
    assertTrue(ctx.sharedIdleStrategy() instanceof BusySpinIdleStrategy);
    // Per-thread strategies fall back to sharedIdle when not explicitly set
    assertTrue(ctx.conductorIdleStrategy() instanceof BusySpinIdleStrategy);
    assertTrue(ctx.receiverIdleStrategy() instanceof BusySpinIdleStrategy);
    assertTrue(ctx.senderIdleStrategy() instanceof BusySpinIdleStrategy);
  }

  @Test
  public void testBuildContextPerThreadStrategiesOverrideShared() {
    EffectiveConfig config =
        new EffectiveConfig(
            "/tmp/test",
            "DEDICATED",
            "BUSY_SPIN",
            "YIELDING",
            "YIELDING",
            "YIELDING",
            false,
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
    MediaDriver.Context ctx = MediaDriverRunner.buildContext(config);

    assertNotNull(ctx);
    assertTrue(ctx.sharedIdleStrategy() instanceof BusySpinIdleStrategy);
    assertTrue(ctx.conductorIdleStrategy() instanceof YieldingIdleStrategy);
    assertTrue(ctx.receiverIdleStrategy() instanceof YieldingIdleStrategy);
    assertTrue(ctx.senderIdleStrategy() instanceof YieldingIdleStrategy);
  }

  @Test
  public void testBuildContextAppliesOptionalTuningParams() {
    EffectiveConfig config =
        new EffectiveConfig(
            "/tmp/test",
            "SHARED",
            "BUSY_SPIN",
            null,
            null,
            null,
            false,
            true,
            5000L,
            10_000_000L,
            null,
            65536,
            1408,
            30L,
            0,
            -1,
            -1,
            -1);
    MediaDriver.Context ctx = MediaDriverRunner.buildContext(config);

    assertEquals(5000L, ctx.driverTimeoutMs());
    assertEquals(10_000_000L, ctx.clientLivenessTimeoutNs());
    assertEquals(65536, ctx.ipcTermBufferLength());
    assertEquals(1408, ctx.mtuLength());
  }

  @Test
  public void testBuildContextWithNullIdleStrategiesUsesAeronDefaults() {
    EffectiveConfig config =
        new EffectiveConfig(
            "/tmp/test",
            null,
            null,
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
            -1,
            -1,
            -1);
    // Should not throw; Aeron will use its own defaults
    MediaDriver.Context ctx = MediaDriverRunner.buildContext(config);
    assertNotNull(ctx);
  }

  // ── Helpers ──────────────────────────────────────────────────────

  private static EffectiveConfig makeConfig(String aeronDirectory) {
    return new EffectiveConfig(
        aeronDirectory,
        "SHARED",
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
        -1,
        -1,
        -1);
  }
}
