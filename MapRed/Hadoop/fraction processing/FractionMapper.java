import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;
import java.io.IOException;

public class FractionMapper extends Mapper<Text, FractionWritable, Text, FractionWritable> {

    @Override
    protected void map(Text key, FractionWritable value, Context context) throws IOException, InterruptedException {
        // Directly pass the key-value pair to the reducer
        context.write(key, value);
    }
}
