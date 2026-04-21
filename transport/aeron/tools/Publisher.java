import io.aeron.Aeron;
import io.aeron.Publication;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.concurrent.Callable;
import org.agrona.BufferUtil;
import org.agrona.concurrent.UnsafeBuffer;
import picocli.CommandLine;
import picocli.CommandLine.Command;
import picocli.CommandLine.Option;

@Command(
    name = "Publisher",
    mixinStandardHelpOptions = true,
    description = "Publishes messages to an Aeron channel. Type a message and press Enter to send.")
public class Publisher implements Callable<Integer> {

  @Option(names = {"-c", "--channel"}, description = "Aeron channel URI", defaultValue = "aeron:ipc")
  private String channel;

  @Option(names = {"-s", "--stream"}, description = "Stream ID", defaultValue = "1001")
  private int streamId;

  @Option(names = {"-d", "--dir"}, description = "Aeron directory", defaultValue = "/dev/shm/aeron-bpt")
  private String aeronDir;

  public static void main(String[] args) {
    System.exit(new CommandLine(new Publisher()).execute(args));
  }

  @Override
  public Integer call() throws Exception {
    System.out.println("Publishing to " + channel + " stream " + streamId);
    System.out.println("Aeron dir: " + aeronDir);

    Aeron.Context ctx = new Aeron.Context().aeronDirectoryName(aeronDir);

    try (Aeron aeron = Aeron.connect(ctx);
        Publication pub = aeron.addPublication(channel, streamId)) {

      System.out.print("Waiting for subscriber...");
      while (!pub.isConnected()) {
        Thread.onSpinWait();
        System.out.print(".");
      }
      System.out.println(" connected.");
      System.out.println("Type a message and press Enter to send. Ctrl+D to quit.");

      UnsafeBuffer buffer =
          new UnsafeBuffer(BufferUtil.allocateDirectAligned(256, 64));
      BufferedReader stdin = new BufferedReader(new InputStreamReader(System.in));

      String line;
      while ((line = stdin.readLine()) != null) {
        if (line.isBlank()) continue;

        int length = buffer.putStringWithoutLengthUtf8(0, line);
        long result;
        do {
          result = pub.offer(buffer, 0, length);
        } while (result < 0);

        System.out.println("[sent] " + line);
      }
    }
    return 0;
  }
}
