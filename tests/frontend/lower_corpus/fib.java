// Recursion, static helpers with conversions at call boundaries, method
// overloading by arity, package-qualified internal name.

package demo;

public class fib {
    public static void main(String[] args) {
        System.out.println(fib(20));
        System.out.println(sumTo(100));
        System.out.println(wide(5));
        System.out.println(pick(1));
        System.out.println(pick(1, 2));
    }

    static int fib(int n) {
        if (n < 2) {
            return n;
        }
        return fib(n - 1) + fib(n - 2);
    }

    static int sumTo(int n) {
        int acc = 0;
        for (int i = 1; i <= n; i++) {
            acc += i;
        }
        return acc;
    }

    static long wide(int v) {
        return v;
    }

    static int pick(int a) {
        return a;
    }

    static int pick(int a, int b) {
        return a + b;
    }
}
