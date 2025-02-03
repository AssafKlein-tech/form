import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class ComplexTermFractionMapper extends Mapper<Object, byte[], Text, FractionWritable> {

    private Text termKey = new Text();
    private Text fractionValue = new FractionWritable();

    @Override
    protected void map(Object key, FractionWritable value, Context context) throws IOException, InterruptedException {
        ByteBuffer buffer = ByteBuffer.wrap(value).order(ByteOrder.LITTLE_ENDIAN);
        
        // Read total record length (First DWORD)
        if (buffer.remaining() < 4) return;
        int totalLength = buffer.getInt();

        if (buffer.remaining() < (totalLength - 1) * 4) return;  // Ensure buffer contains full record

        // Read term (all DWORDs except last one, which is the coefficient length indicator)
        int termLength = totalLength - 1;
        StringBuilder termBuilder = new StringBuilder();
        for (int i = 0; i < termLength - 1; i++) { // Exclude last DWORD
            termBuilder.append(Integer.toHexString(buffer.getInt())).append("*");
        }

        // Read coefficient length indicator (last DWORD of term)
        int coefficientLength = buffer.getInt();
        boolean isNegative = coefficientLength < 0; // If negative, the coefficient is negative
        coefficientLength = Math.abs(coefficientLength);

        // Read numerator & denominator
        int fractionSize = coefficientLength / 2;
        BigInteger numerator = BigInteger.ZERO;
        BigInteger denominator = BigInteger.ZERO;
        BigInteger base = BigInteger.valueOf(2).pow(32);

        // Read numerator
        for (int i = 0; i < fractionSize; i++) {
            numerator = numerator.add(BigInteger.valueOf(Integer.toUnsignedLong(buffer.getInt())).multiply(base.pow(i)));
        }

        // Read denominator
        for (int i = 0; i < fractionSize; i++) {
            denominator = denominator.add(BigInteger.valueOf(Integer.toUnsignedLong(buffer.getInt())).multiply(base.pow(i)));
        }

        // Apply sign to the numerator if needed
        if (isNegative) {
            numerator = numerator.negate();
        }

        // Construct output key-value pair
        termKey.set(termBuilder.toString().replaceAll("\\*$", "")); // Remove trailing "*"
        fractionValue.set(numerator.toString() + "/" + denominator.toString());

        // Emit (key, value)
        context.write(termKey, fractionValue);
    }
}
