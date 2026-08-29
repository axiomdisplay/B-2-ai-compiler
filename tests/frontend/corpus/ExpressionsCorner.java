// corpus: expressions nested-parens chained-ternary assignment-chain conditional-lambda-methodref shift-chain instanceof-cast switch-expression-operand builder-chain

class ExpressionsCorner {

    private int count;

    void noop() {
        count++;
    }

    int evaluate(int a, int b, boolean flag) {
        int parenthesized = ((((a)) + (b)) * ((a) - (b)));

        int chainedTernary = a > b ? a : b > 0 ? a + b : a - b;

        int i = 0;
        int j = 0;
        int k = 9;
        i = j = k;

        Runnable r = flag ? this::noop : () -> noop();
        if (flag) {
            r.run();
        }

        int shifted = a << 2 >> 1 >>> 3;

        Object o = "corner";
        int length = 0;
        if (o instanceof String s) {
            length = ((String) o).length() + s.length();
        }

        String day = "TUE";
        int dayCode = 5 + switch (day) {
            case "MON" -> 1;
            case "TUE" -> 2;
            default -> 0;
        };

        String built = new StringBuilder()
                .append("a")
                .append("b")
                .append(Integer.valueOf(dayCode))
                .toString();

        return parenthesized + chainedTernary + i + shifted + length + built.length() + count;
    }
}
