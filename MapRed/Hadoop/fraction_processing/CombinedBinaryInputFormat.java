import org.apache.hadoop.fs.Path;
import org.apache.hadoop.mapreduce.InputFormat;
import org.apache.hadoop.mapreduce.InputSplit;
import org.apache.hadoop.mapreduce.JobContext;
import org.apache.hadoop.mapreduce.RecordReader;
import org.apache.hadoop.mapreduce.TaskAttemptContext;
import org.apache.hadoop.io.BytesWritable;
import org.apache.hadoop.mapreduce.lib.input.FileInputFormat;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.lib.input.CombineFileInputFormat;
import org.apache.hadoop.mapreduce.lib.input.CombineFileSplit;
import org.apache.hadoop.mapreduce.lib.input.CombineFileRecordReader;
import org.apache.hadoop.mapreduce.lib.input.CombineFileRecordReaderWrapper;

import java.io.IOException;
import java.util.List;

public class CombinedBinaryInputFormat extends CombinedFileInputFormat<BytesWritable, FractionWritable> {

    @Override
    public RecordReader<BytesWritable, FractionWritable> createRecordReader(InputSplit split, TaskAttemptContext context) throws IOException {
    // CombineFileRecordReaderWrapper will take care of instantiating
    // your existing RecordReader for each file in the split.
    return new CombineFileRecordReader<>(
        (CombineFileSplit) split,
        context,
        BinaryRecordReader.class);
    }
    //    return new BinaryRecordReader();  // Use your custom RecordReader
   //}

    @Override
    protected boolean isSplitable(JobContext context, Path file) {
        return false; // Prevents splitting (ensures full records are read properly)
    }
}
