import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.InputFormat;
import org.apache.hadoop.mapreduce.InputSplit;
import org.apache.hadoop.mapreduce.Job;
import org.apache.hadoop.mapreduce.RecordReader;
import org.apache.hadoop.mapreduce.TaskAttemptContext;
import org.apache.hadoop.mapreduce.lib.input.FileInputFormat;
import org.apache.hadoop.mapreduce.task.TaskAttemptContextImpl;
import org.apache.hadoop.mapreduce.TaskAttemptID;
import org.apache.hadoop.mapreduce.lib.input.FileSplit;


import java.io.IOException;
import java.util.List;

public class ReaderTest {
    public static void main(String[] args) throws Exception {
        // Set up a Hadoop Configuration
        Configuration conf = new Configuration();
        Job job = Job.getInstance(conf);
        Path inputPath = new Path(args[0]); // Pass binary input file as argument

        // Get input format
        InputFormat<Text, FractionWritable> inputFormat = new BinaryInputFormat();  // Use your custom BinaryInputFormat
        List<InputSplit> splits = inputFormat.getSplits(job);

        for (InputSplit split : splits) {
            TaskAttemptContext context = new TaskAttemptContextImpl(conf, new TaskAttemptID());
            RecordReader<Text, FractionWritable> reader = inputFormat.createRecordReader(split, context);
            reader.initialize(split, context);

            while (reader.nextKeyValue()) {
                Text key = reader.getCurrentKey();
                FractionWritable value = reader.getCurrentValue();
                System.out.println("Key: " + key.toString() + " | Value: " + value.toString());
            }
            reader.close();
        }
    }
}
