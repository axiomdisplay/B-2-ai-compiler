// NaN discipline (every comparison operator), infinities, float vs double
// formatting, casts between floating families.

public class floats {
    public static void main(String[] args) {
        double nan = 0.0 / 0.0;
        System.out.println(nan < 1.0);
        System.out.println(nan > 1.0);
        System.out.println(nan <= 1.0);
        System.out.println(nan >= 1.0);
        System.out.println(nan == nan);
        System.out.println(nan != nan);
        System.out.println(1.0 / 0.0);
        System.out.println(-1.0 / 0.0);
        System.out.println(0.1 + 0.2);
        System.out.println(1.0f / 3.0f);
        float f = 1;
        System.out.println(f);
        System.out.println((double) f);
        System.out.println((float) 2.75);
        System.out.println((int) 2.99f);
        System.out.println(3.0 == 3.0f);
    }
}
