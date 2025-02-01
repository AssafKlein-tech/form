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


import org.apache.hadoop.mapreduce.Job;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.lib.input.FileInputFormat;

public class FractionProcessingDriver extends Configured implements Tool{

    public static void main(String[] args) throws Exception {
        int res = ToolRunner.run(new Configuration(), new FractionProcessingDriver(), args);
        System.exit(res);
    }
    public int run(String[] args) throws Exception {
        Configuration conf = getConf();
        // Adding profiling configuration to the conf object
        //conf.setProfileEnabled(true);
        //create the job with the conf configuration
        Job job = Job.getInstance(conf, "Fraction sum");
        job.setJarByClass(FractionProcessingDriver.class);

        // Set Mapper & Reducer
        job.setMapperClass(BinaryProcessingMapper.class);
        job.setReducerClass(FractionReducer.class);

        // Use custom Writable classes
        job.setMapOutputKeyClass(Text.class);
        job.setMapOutputValueClass(FractionWritable.class);
        job.setOutputKeyClass(Text.class);
        job.setOutputValueClass(FractionWritable.class);

        // Use custom InputFormat and OutputFormat
        job.setInputFormatClass(BinaryInputFormat.class);
        job.setOutputFormatClass(BinaryOutputFormat.class);

        // Set paths
        FileInputFormat.addInputPath(job, new Path(args[0]));
        FileOutputFormat.setOutputPath(job, new Path(args[1]));

        System.exit(job.waitForCompletion(true) ? 0 : 1);
    }
}


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
        
        // Set Mapper & Reducer
        job.setMapperClass(BinaryProcessingMapper.class);
        job.setReducerClass(FractionReducer.class);

        // Use custom Writable classes
        job.setMapOutputKeyClass(Text.class);
        job.setMapOutputValueClass(FractionWritable.class);
        job.setOutputKeyClass(Text.class);
        job.setOutputValueClass(FractionWritable.class);

        // Use custom InputFormat and OutputFormat
        job.setInputFormatClass(BinaryInputFormat.class);
        //job.setOutputFormatClass(BinaryOutputFormat.class);

        FileInputFormat.addInputPath(job, new Path(args[0]));
        FileOutputFormat.setOutputPath(job, new Path(args[1]));

        return job.waitForCompletion(true) ? 0 : 1;
    }
}
