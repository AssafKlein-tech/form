import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Job;
import org.apache.hadoop.mapreduce.lib.input.FileInputFormat;
import org.apache.hadoop.mapreduce.lib.output.FileOutputFormat;
import org.apache.hadoop.conf.Configured;
import org.apache.hadoop.util.Tool;
import org.apache.hadoop.util.ToolRunner;
import org.apache.hadoop.io.IntWritable;

public class ComplexTermFractionDriver extends Configured implements Tool{

    public static void main(String[] args) throws Exception {
        int res = ToolRunner.run(new Configuration(), new ComplexTermFractionDriver(), args);
        System.exit(res);
    }

    public int run(String[] args) throws Exception {
        //this is done to get the configuration from the ToolMapReduce
        Configuration conf = getConf();
        // Adding profiling configuration to the conf object
        //conf.setProfileEnabled(true);
        //create the job with the conf configuration
        Job job = Job.getInstance(conf, "Complex Term Fraction Summation");
        
        //setting the classes usiing the java class we created for map and reduce (and this driver)
        job.setJarByClass(ComplexTermFractionDriver.class);
        job.setMapperClass(ComplexTermFractionMapper.class);
        job.setReducerClass(ComplexTermFractionReducer.class);

        job.setOutputKeyClass(Text.class);
        job.setOutputValueClass(IntWritable.class);

        FileInputFormat.addInputPath(job, new Path(args[0]));
        FileOutputFormat.setOutputPath(job, new Path(args[1]));

        return job.waitForCompletion(true) ? 0 : 1;
    }
}
