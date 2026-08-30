// Numeric promotions, conversions, int overflow, compound assignment
// narrowing, shifts, and unary operators.

public class arith {
    public static void main(String[] args) {
        System.out.println(6 * 7);
        System.out.println(7 / 2);
        System.out.println(7 % 3);
        System.out.println(1 << 10);
        System.out.println(-16 >> 2);
        System.out.println(-16 >>> 28);
        System.out.println(0x7fffffff + 1);
        System.out.println(5L + 5);
        System.out.println(3 + 0.5);
        System.out.println(2 * 0.5f);
        byte b = 127;
        b++;
        System.out.println(b);
        byte c = (byte) 200;
        System.out.println(c);
        short s = 1000;
        s += 5000;
        System.out.println(s);
        char ch = 'z';
        System.out.println(ch - 'a');
        int n = -7;
        System.out.println(-n);
        System.out.println(~n);
        System.out.println(n / 2);
        System.out.println(n % 3);
        System.out.println((int) 3.99);
        System.out.println((int) -3.99);
        System.out.println((long) 1e18);
        System.out.println((int) 5000000000L);
        System.out.println((char) 66);
    }
}
