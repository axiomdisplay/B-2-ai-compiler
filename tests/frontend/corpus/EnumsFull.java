// corpus: enum constructor fields methods constant-specific-bodies interface values valueOf classic-switch arrow-switch

interface Describable {
    String describe();
}

enum Planet {
    MERCURY(3.303e+23, 2.4396e6),
    VENUS(4.869e+24, 6.0518e6),
    EARTH(5.976e+24, 6.37814e6);

    private final double mass;
    private final double radius;

    Planet(double mass, double radius) {
        this.mass = mass;
        this.radius = radius;
    }

    double surfaceGravity() {
        final double G = 6.67300E-11;
        return G * mass / (radius * radius);
    }
}

enum Operation {
    PLUS {
        @Override
        public double apply(double x, double y) {
            return x + y;
        }
    },
    MINUS {
        @Override
        public double apply(double x, double y) {
            return x - y;
        }
    },
    TIMES {
        @Override
        public double apply(double x, double y) {
            return x * y;
        }
    };

    public abstract double apply(double x, double y);
}

enum Status implements Describable {
    ACTIVE,
    PENDING,
    CLOSED;

    @Override
    public String describe() {
        return "status:" + name().toLowerCase();
    }
}

class EnumUser {

    double operate(Operation op, double x, double y) {
        return op.apply(x, y);
    }

    int classicSwitch(Operation op) {
        switch (op) {
            case PLUS:
                return 1;
            case MINUS:
                return 2;
            case TIMES:
                return 3;
            default:
                return 0;
        }
    }

    int arrowSwitch(Operation op) {
        return switch (op) {
            case PLUS -> 1;
            case MINUS -> 2;
            case TIMES -> 3;
        };
    }

    void iterate() {
        for (Planet planet : Planet.values()) {
            System.out.println(planet + " " + planet.surfaceGravity());
        }
        Planet earth = Planet.valueOf("EARTH");
        Status current = Status.valueOf("ACTIVE");
        System.out.println(earth + " " + current.describe() + " " + current.ordinal());
    }
}
