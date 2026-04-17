package bifrost;

import java.util.ArrayList;
import java.util.List;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Validates an {@link EffectiveConfig} and returns human-readable error messages. An empty list
 * means the configuration is valid.
 */
public final class ConfigValidator {

  private static final Logger LOGGER = LoggerFactory.getLogger(ConfigValidator.class);

  private ConfigValidator() {}

  /**
   * Validates the given effective config.
   *
   * @param c the effective config to validate
   * @param env the deployment environment (DEV, QA, PROD)
   * @param forceDeleteOnStart whether the CLI override for dir-delete-on-start was supplied
   * @return a list of validation error messages; empty if the config is valid
   */
  public static List<String> validate(EffectiveConfig c, String env, boolean forceDeleteOnStart) {
    List<String> errors = new ArrayList<>();

    if (c.aeronDirectory() == null || c.aeronDirectory().trim().isEmpty()) {
      errors.add("aeronDirectory must be non-empty");
    }

    if (c.threadingMode() != null) {
      try {
        IdleStrategyParser.parseThreadingMode(c.threadingMode());
      } catch (Exception e) {
        errors.add(
            "Invalid threadingMode '"
                + c.threadingMode()
                + "'. Valid values: SHARED, SHARED_NETWORK, DEDICATED");
      }
    }

    validateIdleStrategy(c.sharedIdleStrategy(), "sharedIdleStrategy", errors);
    validateIdleStrategy(c.conductorIdleStrategy(), "conductorIdleStrategy", errors);
    validateIdleStrategy(c.receiverIdleStrategy(), "receiverIdleStrategy", errors);
    validateIdleStrategy(c.senderIdleStrategy(), "senderIdleStrategy", errors);

    if (c.dirDeleteOnStart()) {
      if ("PROD".equalsIgnoreCase(env) && !forceDeleteOnStart) {
        errors.add(
            "In PROD mode, dirDeleteOnStart is true but --force-delete-on-start was not supplied");
      } else if (!"PROD".equalsIgnoreCase(env)) {
        LOGGER.warn(
            "dirDeleteOnStart=true in {} environment — Aeron directory will be wiped on launch",
            env.toUpperCase());
      }
    }

    return errors;
  }

  private static void validateIdleStrategy(
      String strategyStr, String fieldName, List<String> errors) {
    if (strategyStr != null) {
      try {
        IdleStrategyParser.parseIdleStrategy(strategyStr);
      } catch (Exception e) {
        errors.add(
            "Invalid "
                + fieldName
                + " '"
                + strategyStr
                + "'. Example valid values: BUSY_SPIN, YIELDING, SLEEPING, SLEEPING:1000");
      }
    }
  }
}
