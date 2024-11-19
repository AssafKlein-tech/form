import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;

import java.io.IOException;
import java.math.BigInteger;

import javax.naming.Context;

public class ComplexTermFractionMapper extends Mapper<Object, Text, Text, FractionWritable> {

    private Text term = new Text();
    private FractionWritable fraction = new FractionWritable();
    private long mapStartTime;

    @Override
    protected void setup(Context context) throws IOException, InterruptedException {
        mapStartTime = System.currentTimeMillis();
    }

    @Override
    protected void cleanup(Context context) throws IOException, InterruptedException {
        long mapEndTime = System.currentTimeMillis();
        System.out.println("Mapper execution time: " + (mapEndTime - mapStartTime) + " ms");
    }

    @Override
    protected void map(Object key, Text value, Context context) throws IOException, InterruptedException {
        String line = value.toString().trim();
        if (line.isEmpty()) return;  // Skip empty lines

        // Check if the line starts with a '+' or '-' sign; if not, ignore the line
        char sign = line.charAt(0);
        if (sign != '+' && sign != '-') return;

        String remaining = line.substring(1).trim();
        int splitIndex = findSplitIndex(remaining);

        if (splitIndex != -1) {
            String numberPart = remaining.substring(0, splitIndex).trim();
            String termPart = remaining.substring(splitIndex).trim();

            // Remove leading * if it exists
            if (termPart.startsWith("*")) {
                termPart = termPart.substring(1).trim();
            }

            BigInteger numerator;
            BigInteger denominator = BigInteger.ONE;

            if (numberPart.isEmpty()) {
                numerator = BigInteger.ONE;
            } else if (numberPart.contains("/")) {
                String[] fractionParts = numberPart.split("/");
                numerator = new BigInteger(fractionParts[0]);
                denominator = new BigInteger(fractionParts[1]);
            } else {
                numerator = new BigInteger(numberPart);
            }

            if (sign == '-') {
                numerator = numerator.negate();  // Negate the numerator if the sign is '-'
            }

            fraction = new FractionWritable(numerator, denominator);
            term.set(termPart);
            context.write(term, fraction);
        }
    }

    // Function to find the split index where the number ends and the term begins
    private int findSplitIndex(String str) {
        for (int i = 0; i < str.length(); i++) {
            if (!Character.isDigit(str.charAt(i)) && str.charAt(i) != '/') {
                return i;
            }
        }
        return -1;  // If the string is all digits or slash, return -1
    }
}
