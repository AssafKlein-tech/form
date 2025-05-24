
# Run the WordCount job
hadoop jar ./form/MapRed/Hadoop/fraction_processing/ComplexTermProcessing.jar \
	FractionDriver -D mapreduce.task.io.file.buffer.size=131072  \
    -D mapreduce.map.memory.mb=3800 \
    -D mapreduce.map.java.opts=-Xmx3276m\
    -D mapreduce.task.io.sort.mb=608 \
    -D mapreduce.map.sort.spill.percent=0.9 \
    -D mapreduce.task.io.sort.factor=100 \
    -D mapreduce.reduce.memory.mb=4096 \
    -D mapreduce.reduce.java.opts=-Xmx3276m \
    -D mapreduce.reduce.shuffle.input.buffer.percent=0.9 \
	-D mapreduce.map.output.compress.codec=org.apache.hadoop.io.compress.SnappyCodec \
    -D mapreduce.map.output.compress=true \
    -D mapreduce.reduce.shuffle.merge.percent=0.8\
    -D mapred.job.reduce.input.buffer.percent=0.75\
	-D mapreduce.job.reduces=16\
	-D mapreduce.job.maps=16\
    ./input /output
