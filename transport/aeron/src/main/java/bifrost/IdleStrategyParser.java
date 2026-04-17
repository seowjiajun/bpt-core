package bifrost;

import io.aeron.driver.ThreadingMode;
import java.util.Locale;
import org.agrona.concurrent.BusySpinIdleStrategy;
import org.agrona.concurrent.IdleStrategy;
import org.agrona.concurrent.SleepingIdleStrategy;
import org.agrona.concurrent.YieldingIdleStrategy;

/** Static utility for parsing Aeron threading modes and idle strategies from string descriptors. */
public final class IdleStrategyParser {

  /**
   * Supported idle strategy types. Private to this parser — callers work with {@link IdleStrategy}
   * instances, not with this enum directly.
   */
  private enum Type {
    /** Spins in a tight loop — lowest latency, highest CPU usage. */
    BUSY_SPIN {
      @Override
      IdleStrategy create(Long parameterNs) {
        return new BusySpinIdleStrategy();
      }
    },

    /** Calls {@code Thread.yield()} between polls. */
    YIELDING {
      @Override
      IdleStrategy create(Long parameterNs) {
        return new YieldingIdleStrategy();
      }
    },

    /**
     * Sleeps between polls. Accepts an optional parameter in nanoseconds; defaults to 1,000,000 ns
     * (1 ms) if not specified.
     */
    SLEEPING {
      @Override
      IdleStrategy create(Long parameterNs) {
        return new SleepingIdleStrategy(parameterNs != null ? parameterNs : DEFAULT_SLEEP_NS);
      }
    };

    private static final long DEFAULT_SLEEP_NS = 1_000_000L;

    abstract IdleStrategy create(Long parameterNs);
  }

  private IdleStrategyParser() {}

  /**
   * Parses a threading mode string into a {@link ThreadingMode} enum value.
   *
   * @param mode the mode name (case-insensitive, trimmed)
   * @return the corresponding {@link ThreadingMode}
   * @throws IllegalArgumentException if the mode is unrecognized
   */
  public static ThreadingMode parseThreadingMode(String mode) {
    return ThreadingMode.valueOf(mode.trim().toUpperCase(Locale.ROOT));
  }

  /**
   * Parses an idle strategy string into an {@link IdleStrategy} instance.
   *
   * <p>Supported values:
   *
   * <ul>
   *   <li>{@code BUSY_SPIN} — spins in a tight loop (lowest latency, highest CPU)
   *   <li>{@code YIELDING} — calls {@code Thread.yield()} between polls
   *   <li>{@code SLEEPING} — sleeps for a default of 1,000,000 nanoseconds (1 ms)
   *   <li>{@code SLEEPING:}<i>&lt;nanos&gt;</i> — sleeps for the specified number of
   *       <b>nanoseconds</b>, e.g. {@code SLEEPING:1000} = 1 microsecond, {@code SLEEPING:1000000}
   *       = 1 millisecond
   * </ul>
   *
   * @param strategyStr the strategy descriptor (case-insensitive, trimmed)
   * @return the corresponding {@link IdleStrategy}, or {@code null} if the input is null/blank
   * @throws IllegalArgumentException if the strategy name is unrecognized or the sleep parameter is
   *     not a valid number
   */
  public static IdleStrategy parseIdleStrategy(String strategyStr) {
    if (strategyStr == null || strategyStr.trim().isEmpty()) {
      return null;
    }

    String trimmed = strategyStr.trim();
    String[] parts = trimmed.split(":", 2);
    String name = parts[0].toUpperCase(Locale.ROOT);

    Type type;
    try {
      type = Type.valueOf(name);
    } catch (IllegalArgumentException e) {
      throw new IllegalArgumentException("Unknown IdleStrategy: " + strategyStr);
    }

    Long parameterNs = null;
    if (parts.length > 1) {
      try {
        parameterNs = Long.parseLong(parts[1].trim());
      } catch (NumberFormatException e) {
        throw new IllegalArgumentException(
            "Invalid sleepNs in SLEEPING strategy: \"" + parts[1].trim() + '"');
      }
    }

    return type.create(parameterNs);
  }
}
