// corpus: classes fields transient volatile static final constructors overloading this-super-delegation instance-init static-init nested-static inner local anonymous native strictfp outer-this

class Classes {

    static int instanceCount;
    final String id;
    transient int cache;
    volatile boolean running;
    static final String PREFIX = "C-";

    static {
        instanceCount = 0;
    }

    {
        cache = -1;
        running = false;
    }

    Classes() {
        this("default");
    }

    Classes(String id) {
        super();
        this.id = PREFIX + id;
        instanceCount++;
    }

    static int count() {
        return instanceCount;
    }

    final String label() {
        return id;
    }

    strictfp double average(double a, double b) {
        return (a + b) / 2.0;
    }

    native void hook();

    String describe(String id) {
        return this.id + ":" + id;
    }

    void toggle() {
        running = !running;
    }

    static class Nested {
        int nestedValue = 1;

        int nested() {
            return nestedValue + count();
        }
    }

    class Inner {
        int inner() {
            return Classes.this.cache + Classes.this.label().length();
        }
    }

    Runnable localTask() {
        class LocalTask implements Runnable {
            @Override
            public void run() {
                System.out.println("local task " + PREFIX);
            }
        }
        return new LocalTask();
    }

    final Runnable anonymousTask = new Runnable() {
        @Override
        public void run() {
            System.out.println("anonymous task " + PREFIX);
        }
    };

    void demo() {
        Nested nested = new Nested();
        Inner inner = new Inner();
        Runnable local = localTask();
        local.run();
        anonymousTask.run();
        System.out.println(nested.nested() + " " + inner.inner() + " "
                + describe("x") + " " + average(1, 2));
    }
}

abstract class AbstractShape {
    abstract double area();

    double doubleArea() {
        return 2 * area();
    }
}
