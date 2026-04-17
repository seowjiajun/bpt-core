package bifrost;

import static org.junit.jupiter.api.Assertions.*;

import io.aeron.driver.ThreadingMode;
import org.agrona.concurrent.BusySpinIdleStrategy;
import org.agrona.concurrent.IdleStrategy;
import org.agrona.concurrent.SleepingIdleStrategy;
import org.agrona.concurrent.YieldingIdleStrategy;
import org.junit.jupiter.api.Test;

public class IdleStrategyParserTest {

  @Test
  public void testParseIdleStrategy() {
    assertTrue(IdleStrategyParser.parseIdleStrategy("BUSY_SPIN") instanceof BusySpinIdleStrategy);
    assertTrue(IdleStrategyParser.parseIdleStrategy("busy_spin") instanceof BusySpinIdleStrategy);
    assertTrue(IdleStrategyParser.parseIdleStrategy("YIELDING") instanceof YieldingIdleStrategy);
    assertTrue(
        IdleStrategyParser.parseIdleStrategy("  Yielding  ") instanceof YieldingIdleStrategy);

    IdleStrategy sleepStrategy = IdleStrategyParser.parseIdleStrategy("SLEEPING");
    assertTrue(sleepStrategy instanceof SleepingIdleStrategy);

    IdleStrategy sleepNsStrategy = IdleStrategyParser.parseIdleStrategy("SLEEPING:1000");
    assertTrue(sleepNsStrategy instanceof SleepingIdleStrategy);

    assertNull(IdleStrategyParser.parseIdleStrategy(null));
    assertNull(IdleStrategyParser.parseIdleStrategy(""));
    assertNull(IdleStrategyParser.parseIdleStrategy("   "));

    assertThrows(
        IllegalArgumentException.class,
        () -> IdleStrategyParser.parseIdleStrategy("UNKNOWN_STRATEGY"));
    assertThrows(
        IllegalArgumentException.class, () -> IdleStrategyParser.parseIdleStrategy("SLEEPING:abc"));
  }

  @Test
  public void testParseThreadingMode() {
    assertEquals(ThreadingMode.SHARED, IdleStrategyParser.parseThreadingMode("SHARED"));
    assertEquals(ThreadingMode.DEDICATED, IdleStrategyParser.parseThreadingMode("dedicated"));
    assertEquals(
        ThreadingMode.SHARED_NETWORK, IdleStrategyParser.parseThreadingMode("SHARED_NETWORK"));

    assertThrows(
        IllegalArgumentException.class, () -> IdleStrategyParser.parseThreadingMode("INVALID"));
  }
}
