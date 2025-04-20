import org.apache.hadoop.mapreduce.lib.output.FileOutputFormat;
import org.apache.hadoop.mapreduce.RecordWriter;
import org.apache.hadoop.mapreduce.TaskAttemptContext;
import org.apache.hadoop.fs.FSDataOutputStream;
import org.apache.hadoop.io.BytesWritable;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.io.Text;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;


public class BinaryOutputFormat extends FileOutputFormat<BytesWritable, FractionWritable> {

    public static final int BYTESINWORD = 4;

    @Override
    public RecordWriter<BytesWritable, FractionWritable> getRecordWriter(TaskAttemptContext context) 
            throws IOException {
        Path file = getDefaultWorkFile(context, "");
        FileSystem fs = file.getFileSystem(context.getConfiguration());
        FSDataOutputStream fileOut = fs.create(file, false);
        return new BinaryRecordWriter(fileOut);
    }

    public static class BinaryRecordWriter extends RecordWriter<BytesWritable, FractionWritable> {
        private FSDataOutputStream out;

        public BinaryRecordWriter(FSDataOutputStream out) {
            this.out = out;
        }

        @Override
        public void write(BytesWritable key, FractionWritable value) throws IOException {

            // Convert fraction (numerator & denominator) to binary format
            byte[] fractionBytes = convertFractionToBinary(value);

            // Calculate total record size
            int totalSizeWords = (key.getLength() + fractionBytes.length) / BYTESINWORD + 2;
            int coeffSizeWords = (fractionBytes.length / BYTESINWORD + 1)*value.getNumerator().signum();

            // Write the first WORD (total length)
            out.write(ByteBuffer.allocate(BYTESINWORD).order(ByteOrder.LITTLE_ENDIAN).putInt(totalSizeWords).array());

            // Write term binary data
            out.write(key.getBytes(), 0, key.getLength());

            // Write fraction binary data
            out.write(fractionBytes);

            // Write the last WORD (coefficient length)
            out.write(ByteBuffer.allocate(BYTESINWORD).order(ByteOrder.LITTLE_ENDIAN).putInt(coeffSizeWords).array());
        }

        @Override
        public void close(TaskAttemptContext context) throws IOException {
            out.write(ByteBuffer.allocate(BYTESINWORD).order(ByteOrder.LITTLE_ENDIAN).putInt(0).array());
            out.close();
        }

            // Helper function: Convert coefficient to BigInteger
        private byte[] convertFractionToBinary(FractionWritable fraction) {
            // The sign is represented in the length word
            byte[] numerator = fraction.getNumerator().abs().toByteArray();
            byte[] denominator = fraction.getDenominator().toByteArray();
            // Ceil to multiplication of 4
            int num_len = (numerator.length+BYTESINWORD-1)/BYTESINWORD;
            int denom_len = (denominator.length+BYTESINWORD-1)/BYTESINWORD;
            // The record length of each is the same and is the largest of the two
            int rec_len = (num_len > denom_len) ? num_len : denom_len;
            // Allocate the all record buffer for both numbers
            byte[] magnitude = new byte[rec_len*BYTESINWORD*2];

            // reverse the data
            for (int i = 0; i < numerator.length ; i++) {
                magnitude[i]     = numerator[numerator.length - 1 - i];
            }
            for (int i = 0; i < denominator.length ; i++) {
                magnitude[i+ rec_len*BYTESINWORD]     = denominator[denominator.length - 1 - i];
            }
            return magnitude;
            
        }
    }
}
