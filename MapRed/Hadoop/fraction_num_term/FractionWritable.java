import org.apache.hadoop.io.WritableComparable;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.math.BigInteger;

public class FractionWritable implements WritableComparable<FractionWritable> {
    private int num;


    public FractionWritable() {
        this(1);
    }

    public FractionWritable(BigInteger numerator, BigInteger denominator) {
        this.numerator = numerator;
        this.denominator = denominator;
        reduceFraction();
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
        reduceFraction();
    }

    @Override
    public int compareTo(FractionWritable o) {
        BigInteger thisFraction = this.numerator.multiply(o.denominator);
        BigInteger otherFraction = o.numerator.multiply(this.denominator);
        return thisFraction.compareTo(otherFraction);
    }

    @Override
    public String toString() {
        return numerator + "/" + denominator;
    }

    public void add(FractionWritable other) {
        if (this.denominator == BigInteger.ONE && other.denominator == BigInteger.ONE)
            this.numerator = this.numerator.add(other.numerator);
        else if (this.denominator == BigInteger.ONE) {
            this.numerator = this.numerator.multiply(other.denominator).add(other.numerator);
            this.denominator = other.denominator;
        }
        else if (other.denominator == BigInteger.ONE) {
            this.numerator = this.numerator.add(other.numerator.multiply(this.denominator));
        }
        else{
        this.numerator = this.numerator.multiply(other.denominator).add(other.numerator.multiply(this.denominator));
        this.denominator = this.denominator.multiply(other.denominator);
        reduceFraction();
        }
    }

    private void reduceFraction() {
        BigInteger gcd = numerator.gcd(denominator);
        numerator = numerator.divide(gcd);
        denominator = denominator.divide(gcd);
    }
}
