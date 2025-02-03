import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

public class IntArrayTest {


    
    public static void main(String[] args) {
        int[] a = {0xFFFFFFFF,0xFFFFFFFF,0x00000000};
        int[] b = {0xFFFFFFFF,0xFFFFFFFF};

        
        long startTime = System.nanoTime();
        int[] result = multiplyUnsignedArrays(a, b);
        long endTime = System.nanoTime();

        
        System.out.println("Int[] Time: " + (endTime - startTime) + " ns");
        System.out.println("Result: " + Arrays.toString(result));
        
        BigInteger A = toBigInteger(a, 1);
        BigInteger B = toBigInteger(b, -1);
        startTime = System.nanoTime();
        BigInteger result1 = A.multiply(B);
        endTime = System.nanoTime();
        
        System.out.println("BigInteger Time: " + (endTime - startTime) + " ns");
        System.out.println("Result: " + result1.toString(16));
        int[] c = toUnsignedIntArray(result1);
        System.out.println("Result: " + Arrays.toString(c));
    }

    // Convert int[] to BigInteger (unsigned representation)
    private static BigInteger toBigInteger(int[] arr,int sign) {
        byte[] byteArray = new byte[arr.length * 4];

        // Fill the byte array in little-endian order
        ByteBuffer buffer = ByteBuffer.wrap(byteArray).order(ByteOrder.LITTLE_ENDIAN);
        for (int value : arr) {
            buffer.putInt(value);
        }

        // Convert to BigInteger (uses BigInteger's built-in two's complement handling)
        return new BigInteger(sign, byteArray); // "1" ensures a positive number
    }
    

    private static int[] divideArray(int[] num, int[] divisor) {
        BigInteger bigNum = toBigInteger(num,1);
        BigInteger bigDiv = toBigInteger(divisor,1);
        BigInteger product = bigNum.divide(bigDiv);
        return toUnsignedIntArray(product);
    }

    // Convert BigInteger back to int[] (unsigned representation)
    private static int[] toUnsignedIntArray(BigInteger bigInt) {

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


    private static int[] multiplyUnsignedArrays(int[] a, int[] b) {
        int alen = a.length;
        int blen = b.length;
        int[] result = new int[alen + blen];

        for (int i = 0; i < alen; i++) {
            long ai = a[i] & 0xFFFFFFFFL;
            long carry = 0;

            for (int j = 0; j < blen; j++) {
                long bi = b[j] & 0xFFFFFFFFL;
                long product = ai * bi + (result[i + j] & 0xFFFFFFFFL) + carry;

                result[i + j] = (int) (product & 0xFFFFFFFFL);
                carry = product >>> 32;
            }

            result[i + blen] = (int) carry;
        }
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
}