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
    
    int[] commonDenominator = null;
    int[] summedNumerator = null;
    int[] oldCommonDenominator = null;
    int commonSign = 1;

    for (FractionWritable value : values) {
        int[] numerator = value.getNumerator();
        int[] denominator = value.getDenominator();
        int sign = value.getSign();

        if (commonDenominator == null) {
            // First fraction: Use it as the initial sum
            commonDenominator = denominator;
            summedNumerator = numerator;
            commonSign = sign;
        } else {
            // Compute new Least Common Denominator (LCD)
            oldCommonDenominator = commonDenominator;
            commonDenominator = lcm(commonDenominator, denominator);

            // Convert both numerators to the new common denominator
            int[] convertedSummedNumerator = convertToCommonDenominator(summedNumerator,oldCommonDenominator, commonDenominator);
            int[] convertedCurrentNumerator = convertToCommonDenominator(numerator, denominator, commonDenominator);

            // Add numerators
            ArrayList<int[]> ret = addArrays(convertedSummedNumerator, convertedCurrentNumerator, commonSign,sign);
            summedNumerator = ret.get(0);
            commonSign = ret.get(1);

        }
    }

    // Simplify the fraction
    int[] gcdValue = gcdArray(summedNumerator, commonDenominator);
    summedNumerator = divideArray(summedNumerator, gcdValue);
    commonDenominator = divideArray(commonDenominator, gcdValue);

    // Emit optimized result
    context.write(key, new FractionWritable(summedNumerator, commonDenominator));
}


    // Compute Least Common Multiple (LCM) of two denominators
    private int[] lcm(int[] a, int[] b) {
        int[] product = multiplyArrays(a, b);
        int[] gcdValue = gcdArray(a, b);
        return divideArray(product, gcdValue);
    }

    // Convert a fraction to a common denominator
    private int[] convertToCommonDenominator(int[] numerator, int[] oldDenominator, int[] newDenominator) {
        int[] factor = divideArray(newDenominator, oldDenominator);
        return multiplyArrays(numerator, factor);
    }

    // Convert signed int to unsigned long
    private long toUnsignedLong(int value) {
        return value & UINT32_MAX;
    }

    // Convert unsigned long back to int (truncating)
    private int toUnsignedInt(long value) {
        return (int) (value & UINT32_MAX);
    }

    // Add two Unsigned integer arrays
    private int[] addArrays(int[] a, int[] b, int aSign, int bSign) {
        if (aSign != bSign)
        {
            return (aSign > bSign) ? subtractArrays(a,b) : subtractArrays(b, a);
        }
        int[] result = new int[Math.max(a.length, b.length)]; //without carry
        long carry = 0; // Carry for overflow

        for (int i = 0; i < result.length; i++) {
            //convert to intermidiate long and handle shorter array
            long ai = (i < a.length) ? toUnsignedLong(a[i]) : 0;
            long bi = (i < b.length) ? toUnsignedLong(b[i]) : 0;

            long sum = ai + bi + carry; // Add with carry

            result[i] = toUnsignedInt(sum); // Store 32-bit result
            carry = (sum > UINT32_MAX) ? 1 : 0; // Update carry
        }
        //if the MSB addition had a carry bit, store it in the next new int 
        if (carry > 0) {
            int[] expanded = new int[result.length + 1];
            System.arraycopy(result, 0, expanded, 0, result.length);
            expanded[result.length] = 1; // Store final carry bit
            return expanded;
        }
        ArrayList<int[]> arrays = new ArrayList<int[]>(2);
        arrays.add(result,aSign);
        return arrays;
    }

    private int[] subtractArrays(int[] a, int[] b) {
        int maxLen = Math.max(a.length, b.length);
        int[] result = new int[maxLen];
        long borrow = 0;
    
        for (int i = 0; i < maxLen; i++) {
            long ai = (i < a.length) ? toUnsignedLong(a[i]) : 0;
            long bi = (i < b.length) ? toUnsignedLong(b[i]) : 0;
    
            long diff = ai - bi - borrow;
            if (diff < 0) {
                diff += (1L << 32); // Add 2^32 to handle underflow
                borrow = 1; // Borrow from next digit
            } else {
                borrow = 0;
            }
    
            result[i] = toUnsignedInt(diff);
        }
    
        // Ensure there are no leading zeros in the result
        return trimLeadingZeros(result);
    }

    // Multiply two Unsigned integer arrays
    private int[] multiplyArrays(int[] a, int[] b) {
        int alen = a.length;
        int blen = b.length;
        int[] result = new int[alen + blen]; // Result array is at most a.length + b.length

        //Outter for
        for (int i = 0; i < alen; i++) {
            long ai = toUnsignedLong(a[i]); // Convert to unsigned
            long carry = 0; // Track overflow carry
            //Inner for
            for (int j = 0; j < blen; j++) {
                long bi = toUnsignedLong(b[j]); // Convert to unsigned
                long product = ai * bi + toUnsignedLong(result[i + j]) + carry;

                result[i + j] = toUnsignedInt(product); // Store low 32 bits
                carry = product >>> 32; // Carry = high 32 bits
            }

            // Store leftover carry
            result[i + blen] = toUnsignedInt(carry);
        }
        // Remove leading zeros (normalize)
        result = trimLeadingZeros(result);
        return result;
    }

    private static int[] trimLeadingZeros(int[] arr) {
        int newSize = arr.length;
        while (newSize > 1 && arr[newSize - 1] == 0) {
            newSize--; // Find last non-zero word
        }
        if (newSize == arr.length)
        {
            return arr;
        }
        int[] newArray = new int[newSize];
        System.arraycopy(arr, 0, newArray, 0, newSize);
        return newArray;
    }

    //Divide two Uint arrays
    private int[] divideArray(int[] num, int[] divisor) {
        BigInteger bigNum = toBigInteger(num);
        BigInteger bigDiv = toBigInteger(divisor);
        BigInteger product = bigNum.divide(bigDiv);
        return toUnsignedIntArray(product);
    }

    private int[] gcdArray(int[] a, int[] b) {
        BigInteger bigNum = toBigInteger(num);
        BigInteger bigDiv = toBigInteger(divisor);
        BigInteger product = bigNum.gcd(bigDiv);
        return toUnsignedIntArray(product);
    }


    // Convert int[] to BigInteger (unsigned representation)
    private BigInteger toBigInteger(int[] arr) {
         byte[] byteArray = new byte[arr.length * 4];

        // Fill the byte array in little-endian order
        ByteBuffer buffer = ByteBuffer.wrap(byteArray).order(ByteOrder.LITTLE_ENDIAN);
        for (int value : arr) {
            buffer.putInt(value);
        }

        // Convert to BigInteger (uses BigInteger's built-in two's complement handling)
        return new BigInteger(1, byteArray); // "1" ensures a positive number
    }

    // Convert BigInteger back to int[] (unsigned representation)
    private int[] toUnsignedIntArray(BigInteger bigInt) {
    
        String hex1 = bigInt.toString(16); // Convert BigInteger to hex string
        int len = hex1.length() / 8; // Number of 4-byte chunks (32 bits)
        if (hex1.length() % 8 != 0)
        {
            len++;
        }
        int[] result = new int[len];
    
        for (int i = 0; i < len; i++) {
            result[i] = bigInt.intValue();
            bigInt = bigInt.shiftRight(32);
        }
    
        return result;
    }
}

