import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;

import java.io.IOException;
import java.math.BigInteger;

public class ComplexTermMapper extends Mapper<Object, Text, Text, Text> {

    private Text term = new Text();
    private Text number = new Text();

    @Override
    protected void map(Object key, Text value, Context context) throws IOException, InterruptedException {
        String line = value.toString().trim();
        if (line.isEmpty()) return;  // Skip empty lines

        char sign = line.charAt(0);  // The first character is the sign
        String remaining = line.substring(1).trim();
        BigInteger num;

        if (Character.isDigit(remaining.charAt(0))) {
            // Split into number and term
            String[] parts = remaining.split("(?<=\\d)(?=\\D)", 2);

            num = new BigInteger(parts[0]);
            if (sign == '-') {
                num = num.negate();  // Negate the number if the sign is '-'
            }

            String termPart = parts[1].trim();
            if (termPart.startsWith("*")) {
                termPart = termPart.substring(1).trim();  // Remove the leading '*' if present
            }

            number.set(num.toString());
            term.set(termPart);
        } else {
            // No number present, default to 1 or -1 based on the sign
            num = (sign == '-') ? BigInteger.ONE.negate() : BigInteger.ONE;

            String termPart = remaining;
            if (termPart.startsWith("*")) {
                termPart = termPart.substring(1).trim();  // Remove the leading '*' if present
            }

            number.set(num.toString());
            term.set(termPart);
        }

        // Write the term and its associated number to context
        context.write(term, number);
    }
}
