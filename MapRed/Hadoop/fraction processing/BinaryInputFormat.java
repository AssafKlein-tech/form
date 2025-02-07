import org.apache.hadoop.fs.Path;
import org.apache.hadoop.mapreduce.InputFormat;
import org.apache.hadoop.mapreduce.InputSplit;
import org.apache.hadoop.mapreduce.JobContext;
import org.apache.hadoop.mapreduce.RecordReader;
import org.apache.hadoop.mapreduce.TaskAttemptContext;
import org.apache.hadoop.mapreduce.lib.input.FileInputFormat;
import org.apache.hadoop.io.Text;

import java.io.IOException;
import java.util.List;

public class BinaryInputFormat extends FileInputFormat<BytesWritable, FractionWritable> {

    @Override
    public RecordReader<BytesWritable, FractionWritable> createRecordReader(InputSplit split, TaskAttemptContext context) 
            throws IOException {
        return new BinaryRecordReader();  // Use your custom RecordReader
    }

    @Override
    protected boolean isSplitable(JobContext context, Path file) {
        return false; // Prevents splitting (ensures full records are read properly)
    }
}
