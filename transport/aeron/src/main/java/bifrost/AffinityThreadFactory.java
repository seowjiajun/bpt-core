package bifrost;

import java.util.BitSet;
import java.util.concurrent.ThreadFactory;
import net.openhft.affinity.Affinity;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * ThreadFactory that pins each thread it creates to a specific CPU core via OpenHFT's Affinity
 * library (JNI wrapper around {@code sched_setaffinity}).
 *
 * <p>Used by {@link MediaDriverRunner} to pin the three Aeron MediaDriver agent threads
 * (conductor, sender, receiver) when DEDICATED threading mode is active, per the central topology
 * file. Pinning is per-thread, not per-process, so this must run inside the thread's own {@code
 * run()} — Aeron's Agrona AgentRunner creates the thread via the factory and then immediately
 * starts it.
 *
 * <p>A {@code coreId} of -1 disables pinning and yields a factory behaviourally identical to
 * {@code r -> new Thread(r, name)} — useful on dev laptops where the topology is empty.
 */
public final class AffinityThreadFactory implements ThreadFactory {

  private static final Logger LOGGER = LoggerFactory.getLogger(AffinityThreadFactory.class);

  private final String threadName;
  private final int coreId;

  /**
   * @param threadName OS thread name (visible in top/ps/perf)
   * @param coreId CPU core to pin to; -1 = no pin
   */
  public AffinityThreadFactory(String threadName, int coreId) {
    this.threadName = threadName;
    this.coreId = coreId;
  }

  @Override
  public Thread newThread(Runnable r) {
    return new Thread(
        () -> {
          if (coreId >= 0) {
            try {
              BitSet mask = new BitSet();
              mask.set(coreId);
              Affinity.setAffinity(mask);
              LOGGER.info("{}: pinned to CPU {}", threadName, coreId);
            } catch (RuntimeException e) {
              // Affinity JNI missing (unsupported platform, missing native lib) is
              // not fatal — fall back to unpinned and continue. Trading hosts
              // should have the lib; dev laptops may not.
              LOGGER.warn(
                  "{}: failed to pin to CPU {} ({}); running unpinned",
                  threadName,
                  coreId,
                  e.getMessage());
            }
          } else {
            LOGGER.info("{}: running unpinned (no topology assignment)", threadName);
          }
          r.run();
        },
        threadName);
  }
}
