import org.apache.hadoop.mapreduce.RecordReader;
import org.apache.hadoop.mapreduce.InputSplit;
import org.apache.hadoop.mapreduce.TaskAttemptContext;
import org.apache.hadoop.io.ArrayWritable;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.fs.FSDataInputStream;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.mapreduce.lib.input.FileSplit;

import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class BinaryRecordReader extends RecordReader<Text, FractionWritable> {
    private FSDataInputStream inputStream; 
    private long start, end, pos;
    private Text currentKey = new Text();
    private FractionWritable currentValue;
    private boolean finished = false;

    @Override
    public void initialize(InputSplit split, TaskAttemptContext context) throws IOException {


        FileSplit fileSplit = (FileSplit) split;  // Cast to FileSplit
        Path filePath = fileSplit.getPath();  // Extract only the file path    //test
        start = fileSplit.getStart();  // Now `getStart()` works!
        end = start + fileSplit.getLength();
        pos = start;

        // Use FileSystem to open the file correctly  //test
        FileSystem fs = filePath.getFileSystem(context.getConfiguration());
        inputStream = fs.open(filePath);  // ✅ Open the actual file (ignores byte range)
    }

    @Override
    public boolean nextKeyValue() throws IOException {
        if (finished || pos >= end) {
            return false;
        }

        // Read the first DWORD (4 bytes) to get the total record size
        byte[] totalLengthBytes = new byte[4];
        if (inputStream.read(totalLengthBytes) == -1) {
            finished = true;
            return false;
        }
        int totalLengthWords = ByteBuffer.wrap(totalLengthBytes).order(ByteOrder.LITTLE_ENDIAN).getInt();

        // If totalLengthWords is 0, it marks the end of records
        if (totalLengthWords == 0) {
            finished = true;
            return false;
        }
        // Read the entire record (totalLengthWords * 4 bytes)
        int totalRecordSizeBytes = (totalLengthWords - 1) * 4;
        byte[] recordBytes = new byte[totalRecordSizeBytes];
        if (inputStream.read(recordBytes) == -1) {
            finished = true;
            return false;
        }
        
        // Extract the last WORD (4 bytes) to get coefficient length
        int coeffLengthBytes = 4;
        int coeffLengthWords = ByteBuffer.wrap(recordBytes, totalRecordSizeBytes - coeffLengthBytes, coeffLengthBytes)
                                         .order(ByteOrder.LITTLE_ENDIAN)
                                         .getInt();
        int sign = 1;
        if (coeffLengthWords < 0)
        {
            sign = -1;
            coeffLengthWords *=-1;
        }
        // Compute term and coefficient sizes
        int termLengthBytes = (totalLengthWords - coeffLengthWords - 1) * 4;
        int coeffSizeBytes = (coeffLengthWords - 1) * 4;  // Exclude the length indicator itself
        // Extract term
        byte[] termBytes = new byte[termLengthBytes];
        System.arraycopy(recordBytes, 0, termBytes, 0, termLengthBytes);
        String term = convertTermToString(termBytes); // Convert binary term to readable format

        // Extract coefficient
        byte[] coeffBytes = new byte[coeffSizeBytes];
        System.arraycopy(recordBytes, termLengthBytes, coeffBytes, 0, coeffSizeBytes);

        // Split coefficient into numerator and denominator
        int fractionPartSize = coeffSizeBytes / 2;  // Since numerator and denominator are equal in size
        BigInteger numerator = convertToBigInteger(coeffBytes, 0, fractionPartSize,sign);
        BigInteger denominator = convertToBigInteger(coeffBytes, fractionPartSize, fractionPartSize, 1);

        // Store extracted values
        currentKey.set(term);
        currentValue = new FractionWritable(numerator, denominator);

        pos += totalRecordSizeBytes;
        return true;
    }

    @Override
    public Text getCurrentKey() {
        return currentKey;
    }

    @Override
    public FractionWritable getCurrentValue() {
        return currentValue;
    }

    @Override
    public float getProgress() {
        return (pos - start) / (float) (end - start);
    }

    @Override
    public void close() throws IOException {
        inputStream.close();
    }

    // Helper function: Convert binary term to readable format
    private String convertTermToString(byte[] termBytes) {
        StringBuilder term = new StringBuilder();
        for (int i = 0; i < termBytes.length; i += 4) {
            int word = ByteBuffer.wrap(termBytes, i, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
            term.append(Integer.toHexString(word)).append("*");
        }
        return term.toString();
    }

    // Helper function: Convert coefficient to BigInteger
    private BigInteger convertToBigInteger(byte[] data, int start, int length, int sign) {
        int numWords = length / 4;
    byte[] magnitude = new byte[length];  // This will hold the big-endian representation

    // For each word in the little-endian data, copy it into the output in reverse order,
    // and reverse the bytes within each word.
    for (int i = 0; i < numWords; i++) {
        // Compute source index in little-endian data
        int srcPos = start + i * 4;
        // Compute destination index in the output so that the most-significant word comes first.
        int destPos = (numWords - 1 - i) * 4;
        // Reverse the 4 bytes of the current word:
        magnitude[destPos]     = data[srcPos + 3];
        magnitude[destPos + 1] = data[srcPos + 2];
        magnitude[destPos + 2] = data[srcPos + 1];
        magnitude[destPos + 3] = data[srcPos];
    }
    
    // Create the BigInteger using the sign and the big-endian magnitude.
    // Note: The constructor BigInteger(int signum, byte[] magnitude) expects the magnitude in big-endian.
    BigInteger result = new BigInteger(sign, magnitude);
    return result;
    }
}
