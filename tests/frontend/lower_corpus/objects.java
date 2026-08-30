// Fields (instance/static), constructors with this(...) delegation,
// instance field initializer prepending, <clinit> synthesis, static and
// instance calls, synchronized blocks and methods.

public class objects {
    static int counter = 0;

    int x;
    int y;
    String tag;

    objects(int x, int y, String tag) {
        this.x = x;
        this.y = y;
        this.tag = tag;
        counter++;
    }

    objects(int x) {
        this(x, 0, "compact");
    }

    synchronized static int doubled(int v) {
        return v * 2;
    }

    int area() {
        return x * y;
    }

    int scale(int f) {
        return area() * f;
    }

    public static void main(String[] args) {
        objects a = new objects(3, 4, "rect");
        System.out.println(a.area());
        System.out.println(a.scale(10));
        System.out.println(a.tag);

        objects b = new objects(5);
        System.out.println(b.x);
        System.out.println(b.y);
        System.out.println(b.tag);

        System.out.println(counter);
        System.out.println(objects.doubled(21));
        System.out.println(objects.counter);

        a.x = 10;
        System.out.println(a.area());

        synchronized (a) {
            System.out.println(a.x);
        }

        objects[] pair = {a, b};
        System.out.println(pair[0].x + pair[1].x);
    }
}
