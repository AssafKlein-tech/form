import org.apache.hadoop.mapreduce.RecordReader;
import org.apache.hadoop.mapreduce.InputSplit;
import org.apache.hadoop.mapreduce.TaskAttemptContext;
import org.apache.hadoop.io.ArrayWritable;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.fs.FSDataInputStream;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;

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
        Path file = new Path(split.toString());
        FileSystem fs = file.getFileSystem(context.getConfiguration());
        inputStream = fs.open(file);

        start = split.getStart();
        end = start + split.getLength();
        pos = start;
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
        int totalRecordSizeBytes = totalLengthWords * 4;
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
        if (length <= 0 || start < 0 || start + length > data.length) {
            throw new IllegalArgumentException("Invalid start or length");
        }
        int[] uintArray = new int[length / 4];
        for (int i = 0; i < uintArray.length; i++) {
            uintArray[i] = ByteBuffer.wrap(data, start + i * 4, 4)
                                     .order(ByteOrder.LITTLE_ENDIAN)
                                     .getInt();
        }                        
        byte[] byteArray = new byte[arr.length * 4];

        // Fill the byte array in little-endian order
        ByteBuffer buffer = ByteBuffer.wrap(byteArray).order(ByteOrder.LITTLE_ENDIAN);
        for (int value : uintArray) {
            buffer.putInt(value);
        }

        // Convert to BigInteger (uses BigInteger's built-in two's complement handling)
        return new BigInteger(sign, byteArray); // "1" ensures a positive number
    }
}
