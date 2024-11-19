import org.apache.hadoop.io.WritableComparable;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;

public class FractionWritable implements WritableComparable<FractionWritable> {
    private int numerator;
    private int denominator;

    public FractionWritable() {
        this(0, 1);
    }

    public FractionWritable(int numerator, int denominator) {
        this.numerator = numerator;
        this.denominator = denominator;
        reduceFraction();
    }

    public int getNumerator() {
        return numerator;
    }

    public int getDenominator() {
        return denominator;
    }

    @Override
    public void write(DataOutput out) throws IOException {
        out.writeInt(numerator);
        out.writeInt(denominator);
    }

    @Override
    public void readFields(DataInput in) throws IOException {
        numerator = in.readInt();
        denominator = in.readInt();
        reduceFraction();
    }

    @Override
    public int compareTo(FractionWritable o) {
        long thisFraction = this.numerator * (long) o.denominator;
        long otherFraction = o.numerator * (long) this.denominator;
        return Long.compare(thisFraction, otherFraction);
    }

    @Override
    public String toString() {
        return numerator + "/" + denominator;
    }

    public void add(FractionWritable other) {
        this.numerator = this.numerator * other.denominator + other.numerator * this.denominator;
        this.denominator = this.denominator * other.denominator;
        reduceFraction();
    }

    private void reduceFraction() {
        int gcd = gcd(Math.abs(numerator), denominator);
        numerator /= gcd;
        denominator /= gcd;
    }

    private int gcd(int a, int b) {
        while (b != 0) {
            int temp = b;
            b = a % b;
            a = temp;
        }
        return a;
    }
}
