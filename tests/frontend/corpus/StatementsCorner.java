// corpus: statements empty-statement labeled-block dangling-else assert synchronized-method synchronized-block local-record local-enum local-interface

class StatementsCorner {

    private final Object lock = new Object();
    private int counter;

    synchronized void bump() {
        counter++;
    }

    void demo(int n) {
        ;

        outer:
        {
        }

        if (n > 0)
            if (n > 10)
                counter = 100;
            else
                counter = 1;

        assert n >= 0;
        assert n >= 0 : "negative input: " + n;

        synchronized (lock) {
            counter += 2;
        }

        record Point(int x, int y) {
        }

        enum Color {
            RED, GREEN, BLUE
        }

        interface Printable {
            void print();
        }

        Point p = new Point(n, n);
        Color color = Color.RED;
        Printable printer = () -> System.out.println(p + " " + color + " " + counter);
        printer.print();

        bump();
    }
}
