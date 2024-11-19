import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;

import java.io.IOException;

public class MultiLineTermMapper extends Mapper<Object, Text, Text, FractionWritable> {

    private Text term = new Text();
    private FractionWritable fraction = new FractionWritable();
    private StringBuilder termBuilder = new StringBuilder();

    @Override
    protected void map(Object key, Text value, Context context) throws IOException, InterruptedException {
        String line = value.toString().trim();

        if (line.isEmpty()) return;  // Skip empty lines

        if (line.startsWith("+") || line.startsWith("-")) {
            // If we have accumulated a term, process it before starting a new one
            if (termBuilder.length() > 0) {
                processTerm(termBuilder.toString(), context);
                termBuilder.setLength(0);  // Clear the StringBuilder for the new term
            }
        }

        // Accumulate the current line
        termBuilder.append(line).append(" ");
    }

    @Override
    protected void cleanup(Context context) throws IOException, InterruptedException {
        // Process any remaining term in the builder at the end of the input
        if (termBuilder.length() > 0) {
            processTerm(termBuilder.toString().trim(), context);
        }
    }
    
    private void processTerm(String fullTerm, Context context) throws IOException, InterruptedException {
        char sign = fullTerm.charAt(0);  // The first character is the sign
        String remaining = fullTerm.substring(1).trim();
    
        int splitIndex = findSplitIndex(remaining);
    
        String numberPart;
        String termPart;
    
        if (splitIndex == -1) {
            // No explicit number; treat it as 1 or -1 based on the sign
            numberPart = "1";
            termPart = remaining;
        } else {
            numberPart = remaining.substring(0, splitIndex).trim();
            termPart = remaining.substring(splitIndex).trim();
        }
    
        int numerator;
        int denominator = 1;
    
        if (numberPart.isEmpty()) {
            numerator = 1;  // Default to 1 if no number is present
        } else if (numberPart.contains("/")) {
            String[] fractionParts = numberPart.split("/");
            numerator = Integer.parseInt(fractionParts[0]);
            denominator = Integer.parseInt(fractionParts[1]);
        } else {
            numerator = Integer.parseInt(numberPart);
        }
    
        if (sign == '-') {
            numerator = -numerator;  // Negate the numerator if the sign is '-'
        }
    
        fraction = new FractionWritable(numerator, denominator);
        term.set(termPart);
        context.write(term, fraction);
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
