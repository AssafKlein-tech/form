import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.naming.Context;

import java.io.IOException;
import java.math.BigInteger;

public class ComplexTermFractionMapper extends Mapper<Object, Text, Text, FractionWritable> {

    private Text term = new Text();
    private FractionWritable fraction = new FractionWritable();
     // Compile the regex pattern once to avoid re-compilation overhead
    private static final Pattern noCoPattern = Pattern.compile("^([+-]?)\\s*(.+)$");
    private static final Pattern coPattern = Pattern.compile("^([+-]?)\\s*(\\d+)(?:/(\\d+))?\\*(.+)$");


    @Override
    protected void map(Object key, Text value, Context context) throws IOException, InterruptedException {
        String line = value.toString().trim();
        if (line.isEmpty()) return;  // Skip empty lines
        Matcher matcher;
        BigInteger numerator = BigInteger.ONE;
        BigInteger denominator = BigInteger.ONE;
        String termPart = "";

        if (Character.isDigit(line.charAt(2)))
        {
            matcher = coPattern.matcher(line);
            if (matcher.matches()) {
                // Extract sign
                int sign = "+".equals(matcher.group(1)) || matcher.group(1).isEmpty() ? 1 : -1;
    
                // Extract numerator and apply sign if present, default to 1
                numerator =  new BigInteger(matcher.group(2)) ;
                numerator = numerator.multiply(BigInteger.valueOf(sign));
    
                // Extract denominator if present, default to 1
                denominator = matcher.group(3) != null ? new BigInteger(matcher.group(3)) : denominator;
    
                // Extract term part
                termPart = matcher.group(4).trim();
            }
        }
        else
        {
            matcher = noCoPattern.matcher(line);
            if (matcher.matches()) {
                // Extract sign
                int sign = "+".equals(matcher.group(1)) || matcher.group(1).isEmpty() ? 1 : -1;

                // Extract numerator and apply sign if present, default to 1
                numerator = numerator.multiply(BigInteger.valueOf(sign));

                // Extract term part
                termPart = matcher.group(2).trim();
            }
            else return;
        }

        fraction = new FractionWritable(numerator, denominator);
        term.set(termPart);
        context.write(term, fraction);

        System.out.println("Numerator: " + numerator);
        System.out.println("Denominator: " + denominator);
        System.out.println("Term: " + term);

    }    
}
