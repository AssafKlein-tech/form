import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Job;
import org.apache.hadoop.mapreduce.lib.input.FileInputFormat;
import org.apache.hadoop.mapreduce.lib.output.FileOutputFormat;

public class ComplexTermFractionDriver {

    public static void main(String[] args) throws Exception {
        Configuration conf = getConf();
        Configuration.set(conf.TASK_PROFILE, true);
        Job job = Job.getInstance(conf, "Complex Term Fraction Summation");

        job.setJarByClass(ComplexTermFractionDriver.class);
        job.setMapperClass(ComplexTermFractionMapper.class);
        job.setReducerClass(ComplexTermFractionReducer.class);

        job.setOutputKeyClass(Text.class);
        job.setOutputValueClass(FractionWritable.class);

        FileInputFormat.addInputPath(job, new Path(args[0]));
        FileOutputFormat.setOutputPath(job, new Path(args[1]));

        System.exit(job.waitForCompletion(true) ? 0 : 1);
    }
}
