package bifrost;

/**
 * Immutable snapshot of the effective configuration after merging file config with CLI overrides.
 */
public record EffectiveConfig(
    String aeronDirectory,
    String threadingMode,
    String sharedIdleStrategy,
    String conductorIdleStrategy,
    String receiverIdleStrategy,
    String senderIdleStrategy,
    boolean dirDeleteOnStart,
    boolean dirDeleteOnShutdown,
    Long driverTimeoutMs,
    Long clientLivenessTimeoutNs,
    Long publicationUnblockTimeoutNs,
    Integer termBufferLength,
    Integer mtuLength,
    long heartbeatIntervalSec,
    int metricsPort,
    int conductorCore,
    int senderCore,
    int receiverCore) {

  /** Merges file-based {@link Config} with CLI override flags into an immutable snapshot. */
  public static EffectiveConfig from(
      Config c, boolean forceDeleteOnStart, boolean forceDeleteOnShutdown) {
    return new EffectiveConfig(
        c.aeronDirectory,
        c.threadingMode,
        c.idleStrategy,
        c.conductorIdleStrategy,
        c.receiverIdleStrategy,
        c.senderIdleStrategy,
        forceDeleteOnStart || c.dirDeleteOnStart,
        forceDeleteOnShutdown || c.dirDeleteOnShutdown,
        c.driverTimeoutMs,
        c.clientLivenessTimeoutNs,
        c.publicationUnblockTimeoutNs,
        c.termBufferLength,
        c.mtuLength,
        c.heartbeatIntervalSec,
        c.metricsPort,
        c.conductorCore,
        c.senderCore,
        c.receiverCore);
  }
}
