// try/catch with the builtin hierarchy, finally on normal and exceptional
// paths, nested try, user-thrown program-class objects, rethrow through
// finally, catch across union of paths.

public class exceptions {
    static int freed = 0;

    public static void main(String[] args) {
        try {
            int z = 10 / 0;
            System.out.println(z);
        } catch (ArithmeticException e) {
            System.out.println("caught");
        } finally {
            System.out.println("finally1");
        }

        try {
            int[] bad = new int[2];
            bad[5] = 1;
        } catch (Exception e) {
            System.out.println("bounds");
        }

        try {
            throwObj();
        } catch (exceptions e) {
            System.out.println("thrown");
            System.out.println(e.freed);
        }

        try {
            try {
                int[] tiny = new int[1];
                tiny[9] = 3;
            } finally {
                System.out.println("inner finally");
            }
        } catch (Exception ex) {
            System.out.println("outer caught");
        }

        try {
            try {
                throw new exceptions();
            } finally {
                freed = 7;
                System.out.println("assign finally");
            }
        } catch (exceptions e) {
            System.out.println(freed);
        }

        System.out.println(cleanup());
        System.out.println(freed);
    }

    static void throwObj() {
        exceptions e = new exceptions();
        e.freed = 3;
        throw e;
    }

    static int cleanup() {
        try {
            return 1;
        } finally {
            freed = 99;
            System.out.println("return finally");
        }
    }
}
