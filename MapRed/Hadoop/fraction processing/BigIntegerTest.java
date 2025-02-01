import java.math.BigInteger;

public class BigIntegerTest {
    public static void main(String[] args) {
        BigInteger a = new BigInteger("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", 16);
        BigInteger b = new BigInteger("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", 16);
        
        long startTime = System.nanoTime();
        BigInteger result = a.multiply(b);
        long endTime = System.nanoTime();
        
        System.out.println("BigInteger Time: " + (endTime - startTime) + " ns");
        System.out.println("Result: " + result.toString(16));
    }
}
