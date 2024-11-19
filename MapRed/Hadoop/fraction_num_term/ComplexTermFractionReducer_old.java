import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Reducer;

import java.io.IOException;
import java.math.BigInteger;

import javax.naming.Context;

public class ComplexTermFractionReducer extends Reducer<Text, FractionWritable, Text, FractionWritable> {
    private long reduceStartTime;
    
    @Override
    protected void setup(Context context) throws IOException, InterruptedException {
        reduceStartTime = System.currentTimeMillis();
    }

    @Override
    protected void cleanup(Context context) throws IOException, InterruptedException {
        long reduceEndTime = System.currentTimeMillis();
        System.out.println("Reducer execution time: " + (reduceEndTime - reduceStartTime) + " ms");
    }

    @Override
    protected void reduce(Text key, Iterable<FractionWritable> values, Context context) throws IOException, InterruptedException {
        FractionWritable sum = new FractionWritable(BigInteger.ZERO, BigInteger.ONE);
        for (FractionWritable val : values) {
            sum.add(val);
        }

        // Check if the resulting sum is 0/1 before writing it to the context
        if (!(sum.getNumerator().equals(BigInteger.ZERO) && sum.getDenominator().equals(BigInteger.ONE))) {
            context.write(key, sum);
        }
    }
}
