import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Reducer;
import org.apache.hadoop.io.BytesWritable;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;


import javax.naming.Context;

public class FractionReducer extends Reducer<BytesWritable, FractionWritable, BytesWritable, FractionWritable> {

    private static final long UINT32_MAX = 0xFFFFFFFFL;
    private final FractionWritable result = new FractionWritable();

    @Override
protected void reduce(BytesWritable key, Iterable<FractionWritable> values, Context context) 
        throws IOException, InterruptedException {
    
    BigInteger commonDenominator = BigInteger.ZERO;
    BigInteger summedNumerator =BigInteger.ZERO;
    BigInteger oldCommonDenominator = BigInteger.ZERO;

    for (FractionWritable value : values) {
        BigInteger numerator = value.getNumerator();
        BigInteger denominator = value.getDenominator();
        BigInteger convertedSummedNumerator;
        BigInteger convertedCurrentNumerator;

        if (commonDenominator.equals(BigInteger.ZERO)) {
            // First fraction: Use it as the initial sum
            //System.out.println("new key: " + key);
            //System.out.println("initial value: " + value.getNumerator() + " / " + value.getDenominator());
            commonDenominator = denominator;
            summedNumerator = numerator;
        } else {
            // Compute new Least Common Denominator (LCD)
            //System.out.println("next value: " + value.getNumerator() + " / " + value.getDenominator());
            oldCommonDenominator = commonDenominator;
            if (!commonDenominator.equals(denominator))
            {
                commonDenominator = lcm(commonDenominator, denominator);
            }

            // Convert both numerators to the new common denominator
            convertedSummedNumerator = convertToCommonDenominator(summedNumerator,oldCommonDenominator, commonDenominator);
            convertedCurrentNumerator = convertToCommonDenominator(numerator, denominator, commonDenominator);

            // Add numerators
            summedNumerator = convertedSummedNumerator.add(convertedCurrentNumerator);
            //System.out.println("after add: " + summedNumerator + " / " + commonDenominator);
        }
    }
    if (summedNumerator != BigInteger.ZERO )
    {
        // Simplify the fraction
        BigInteger gcdValue = summedNumerator.gcd(commonDenominator);
        summedNumerator = summedNumerator.divide(gcdValue);
        commonDenominator = commonDenominator.divide(gcdValue);
        //System.out.println("final value: " + summedNumerator + " / " + commonDenominator);
        // Emit optimized result
        result.set(summedNumerator, commonDenominator);
        context.write(key, result);
    }
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

