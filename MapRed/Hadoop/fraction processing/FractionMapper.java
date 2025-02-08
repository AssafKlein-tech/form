import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;
import org.apache.hadoop.io.BytesWritable;
import java.io.IOException;

import javax.naming.Context;

public class FractionMapper extends Mapper<BytesWritable, FractionWritable, BytesWritable, FractionWritable> {

    @Override
    protected void map(BytesWritable key, FractionWritable value, Context context) throws IOException, InterruptedException {
        // Directly pass the key-value pair to the reducer
        context.write(key, value);
    }
}
