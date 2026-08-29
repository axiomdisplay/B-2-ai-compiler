// corpus: records compact-constructor generic-record sealed permits non-sealed annotation-type default-value array-element

record Point(int x, int y) {

    Point {
        if (x < 0 || y < 0) {
            throw new IllegalArgumentException("negative coordinate");
        }
    }

    double magnitude() {
        return Math.hypot(x, y);
    }
}

record Holder<T>(T value) {

    T valueOrDefault(T fallback) {
        return value == null ? fallback : value;
    }
}

sealed interface Shape permits Circle, Square {
    double area();
}

record Circle(double radius) implements Shape {
    @Override
    public double area() {
        return Math.PI * radius * radius;
    }
}

non-sealed class Square implements Shape {
    private final double side;

    Square(double side) {
        this.side = side;
    }

    @Override
    public double area() {
        return side * side;
    }
}

@interface Review {
    int value();

    String reviewer() default "unknown";

    String[] tags() default {};
}

@Review(value = 3, reviewer = "ada", tags = {"parser", "corpus"})
class ReviewedClass {

    @Review(value = 5)
    void serve() {
    }
}

@Review(2)
class QuickReview {
}

@Review(value = 1, tags = "urgent")
class UrgentReview {
}

class RecordDemos {

    String describe(Shape shape) {
        return "shape area " + shape.area();
    }

    void run() {
        Point p = new Point(3, 4);
        Holder<String> holder = new Holder<>("data");
        Circle circle = new Circle(1.5);
        Square square = new Square(2.0);
        System.out.println(p.magnitude() + " " + holder.valueOrDefault("empty") + " "
                + describe(circle) + " " + describe(square));
    }
}
