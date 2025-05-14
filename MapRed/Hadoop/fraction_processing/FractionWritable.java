import org.apache.hadoop.io.Writable;
import java.io.DataInput;
import java.math.BigInteger;
import java.io.DataOutput;
import java.io.IOException;
import java.util.Arrays;

public class FractionWritable implements Writable {
    private BigInteger numerator;
    private BigInteger denominator;

    public FractionWritable() {}

    public FractionWritable(BigInteger numerator, BigInteger denominator) {
        this.numerator = numerator;
        this.denominator = denominator;
    }

    public void set(BigInteger numerator, BigInteger denominator) {
        this.numerator = numerator;
        this.denominator = denominator;
    }

    public BigInteger getNumerator() {
        return numerator;
    }

    public BigInteger getDenominator() {
        return denominator;
    }



    @Override
    public void write(DataOutput out) throws IOException {
        out.writeUTF(numerator.toString());
        out.writeUTF(denominator.toString());
    }

    @Override
    public void readFields(DataInput in) throws IOException {
        numerator = new BigInteger(in.readUTF());
        denominator = new BigInteger(in.readUTF());
    }

    @Override
    public String toString() {
        return numerator + "/" + denominator;
    }
}
