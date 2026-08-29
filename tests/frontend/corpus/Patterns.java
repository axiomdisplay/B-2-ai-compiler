// corpus: pattern-matching instanceof-pattern record-pattern switch-expression when-guard null-case yield block-body multi-label arrow-switch-statement classic-switch-statement

record Point(int x, int y) {
}

class Patterns {

    String describeObject(Object obj) {
        if (obj instanceof String s) {
            return "string of length " + s.length();
        }
        if (obj instanceof Integer) {
            return "plain integer";
        }
        return "unknown";
    }

    String describePoint(Object value) {
        if (value instanceof Point(int x, int y)) {
            return "point " + x + "," + y;
        }
        return "not a point";
    }

    String categorize(Object value) {
        return switch (value) {
            case null -> "null";
            case Integer i when i > 100 -> "big int " + i;
            case Integer i -> {
                String label = "int " + i;
                yield label;
            }
            case String s when s.startsWith("#") -> "hash " + s;
            case String s -> "text " + s;
            case Point(int x, int y) when x == y -> "diagonal";
            case Point(int x, int y) -> "point " + x + "," + y;
            default -> {
                yield "other";
            }
        };
    }

    int scoreGrade(String grade) {
        return switch (grade) {
            case "A", "B" -> 2;
            case "C" -> 1;
            default -> 0;
        };
    }

    void report(int code) {
        switch (code) {
            case 0 -> System.out.println("zero");
            case 1, 2 -> System.out.println("small");
            default -> System.out.println("large");
        }
    }

    int classify(int code) {
        int bucket = 0;
        switch (code) {
            case 1:
                bucket += 1;
            case 2:
                bucket += 2;
                break;
            case 3:
            case 4:
                bucket += 4;
                break;
            default:
                bucket = -1;
        }
        return bucket;
    }
}
