import io.aeron.Aeron;
import io.aeron.Subscription;
import io.aeron.logbuffer.FragmentHandler;
import java.util.concurrent.Callable;
import java.util.concurrent.atomic.AtomicBoolean;
import org.agrona.concurrent.SigInt;
import picocli.CommandLine;
import picocli.CommandLine.Command;
import picocli.CommandLine.Option;

@Command(
    name = "Subscriber",
    mixinStandardHelpOptions = true,
    description = "Subscribes to an Aeron channel and prints incoming messages.")
public class Subscriber implements Callable<Integer> {

  @Option(names = {"-c", "--channel"}, description = "Aeron channel URI", defaultValue = "aeron:ipc")
  private String channel;

  @Option(names = {"-s", "--stream"}, description = "Stream ID", defaultValue = "1001")
  private int streamId;

  @Option(names = {"-d", "--dir"}, description = "Aeron directory", defaultValue = "/dev/shm/aeron-bifrost")
  private String aeronDir;

  public static void main(String[] args) {
    System.exit(new CommandLine(new Subscriber()).execute(args));
  }

  @Override
  public Integer call() throws Exception {
    System.out.println("Subscribing on " + channel + " stream " + streamId);
    System.out.println("Aeron dir: " + aeronDir);

    Aeron.Context ctx = new Aeron.Context().aeronDirectoryName(aeronDir);

    FragmentHandler handler =
        (buffer, offset, length, header) -> {
          String msg = buffer.getStringWithoutLengthUtf8(offset, length);
          System.out.println("[recv] " + msg);
        };

    AtomicBoolean running = new AtomicBoolean(true);
    SigInt.register(() -> running.set(false));

    try (Aeron aeron = Aeron.connect(ctx);
        Subscription sub = aeron.addSubscription(channel, streamId)) {

      System.out.println("Waiting for messages... (Ctrl+C to stop)");
      while (running.get()) {
        int fragments = sub.poll(handler, 10);
        if (fragments == 0) Thread.onSpinWait();
      }
    }
    return 0;
  }
}
