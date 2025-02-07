import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Reducer;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;


import javax.naming.Context;

public class FractionReducer extends Reducer<Text, FractionWritable, Text, FractionWritable> {

    private static final long UINT32_MAX = 0xFFFFFFFFL;

    @Override
protected void reduce(Text key, Iterable<FractionWritable> values, Context context) 
        throws IOException, InterruptedException {
    
    BigInteger commonDenominator = BigInteger.ZERO;
    BigInteger summedNumerator =BigInteger.ZERO;
    BigInteger oldCommonDenominator = BigInteger.ZERO;

    for (FractionWritable value : values) {
        BigInteger numerator = value.getNumerator();
        BigInteger denominator = value.getDenominator();

        if (commonDenominator == BigInteger.ZERO) {
            // First fraction: Use it as the initial sum
            commonDenominator = denominator;
            summedNumerator = numerator;
        } else {
            // Compute new Least Common Denominator (LCD)
            oldCommonDenominator = commonDenominator;
            commonDenominator = lcm(commonDenominator, denominator);

            // Convert both numerators to the new common denominator
            BigInteger convertedSummedNumerator = convertToCommonDenominator(summedNumerator,oldCommonDenominator, commonDenominator);
            BigInteger convertedCurrentNumerator = convertToCommonDenominator(numerator, denominator, commonDenominator);

            // Add numerators
            summedNumerator = convertedSummedNumerator.add(convertedCurrentNumerator);
        }
    }

    // Simplify the fraction
    BigInteger gcdValue = summedNumerator.gcd(commonDenominator);
    summedNumerator = summedNumerator.divide(gcdValue);
    commonDenominator = commonDenominator.divide(gcdValue);

    // Emit optimized result
    context.write(key, new FractionWritable(summedNumerator, commonDenominator));
}


    // Compute Least Common Multiple (LCM) of two denominators
    private BigInteger lcm(BigInteger a, BigInteger b) {
        BigInteger product = a.multiply(b);
        BigInteger gcdValue = a.gcd(b);
        return product.divide(gcdValue);
    }

    // Convert a fraction to a common denominator
    private BigInteger convertToCommonDenominator(BigInteger numerator, BigInteger oldDenominator, BigInteger newDenominator) {
        BigInteger factor = newDenominator.divide(oldDenominator);
        return numerator.multiply(factor);
    }
}

