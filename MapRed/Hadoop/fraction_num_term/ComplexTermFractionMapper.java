import org.apache.hadoop.io.Text;
import org.apache.hadoop.io.IntWritable;
import org.apache.hadoop.mapreduce.Mapper;

import java.io.IOException;
import java.math.BigInteger;

import javax.naming.Context;

public class ComplexTermFractionMapper extends Mapper<Object, Text, Text, IntWritable> {

    private Text term = new Text();

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
            String termPart = remaining.substring(splitIndex).trim();

            // Remove leading * if it exists
            if (termPart.startsWith("*")) {
                termPart = termPart.substring(1).trim();
            }

            term.set(termPart);
            context.write(term, new IntWritable(1));
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
