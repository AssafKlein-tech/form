import org.apache.hadoop.io.Writable;
import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.Arrays;

public class FractionWritable implements Writable {
    private int[] numerator;
    private int[] denominator;
    private int sign;

    public FractionWritable() {}

    public FractionWritable(int[] numerator, int[] denominator, int sign) {
        if (numerator.length != denominator.length)
        {
            throw new IOException("numerator.length != denominator.length");
        }    
        this.numerator = numerator;
        this.denominator = denominator;
        this.sign = sign;
    }

    public int[] getNumerator() {
        return numerator;
    }

    public int[] getDenominator() {
        return denominator;
    }

    public int getSign() {
        return sign;
    }

    @Override
    public void write(DataOutput out) throws IOException {
        out.writeInt(numerator.length);
        for (int num : numerator) {
            out.writeInt(num);
        }
        out.writeInt(denominator.length);
        for (int denom : denominator) {
            out.writeInt(denom);
        }
    }

    @Override
    public void readFields(DataInput in) throws IOException {
        int numSize = in.length;
        numerator = new int[numSize/2];
        for (int i = 0; i < numSize/2; i++) {
            numerator[i] = in.readInt();
        }

        denominator = new int[numSize/2];
        for (int i = 0; i < denomSize/2; i++) {
            denominator[i] = in.readInt();
        }
    }

    @Override
    public String toString() {
        return "Numerator: " + Arrays.toString(numerator) + ", Denominator: " + Arrays.toString(denominator);
    }
}
