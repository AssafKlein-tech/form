import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Reducer;

import java.io.IOException;
import java.math.BigInteger;

public class ComplexTermReducer extends Reducer<Text, Text, Text, Text> {

    @Override
    protected void reduce(Text key, Iterable<Text> values, Context context) throws IOException, InterruptedException {
        BigInteger sum = BigInteger.ZERO;
        for (Text val : values) {
            sum = sum.add(new BigInteger(val.toString()));
        }
        context.write(key, new Text(sum.toString()));
    }
}
