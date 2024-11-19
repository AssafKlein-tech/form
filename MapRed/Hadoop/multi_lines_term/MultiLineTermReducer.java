import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Reducer;

import java.io.IOException;

public class MultiLineTermReducer extends Reducer<Text, FractionWritable, Text, FractionWritable> {

    @Override
    protected void reduce(Text key, Iterable<FractionWritable> values, Context context) throws IOException, InterruptedException {
        FractionWritable sum = new FractionWritable(0, 1);
        for (FractionWritable val : values) {
            sum.add(val);
        }
        context.write(key, sum);
    }
}
