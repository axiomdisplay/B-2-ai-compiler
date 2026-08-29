// corpus: operators arithmetic shift relational equality bitwise logical ternary compound-assign unary increment cast intersection-cast instanceof class-literal

class Operators {

    int arithmeticAndRelational(int a, int b) {
        int sum = a + b;
        int difference = a - b;
        int product = a * b;
        int quotient = a / b;
        int remainder = a % b;
        boolean lt = a < b;
        boolean gt = a > b;
        boolean le = a <= b;
        boolean ge = a >= b;
        boolean eq = a == b;
        boolean ne = a != b;
        int and = a & b;
        int or = a | b;
        int xor = a ^ b;
        boolean logic = (lt && gt) || (le && ge) || (eq && ne);
        int chosen = logic ? sum : difference;
        return sum + difference + product + quotient + remainder + and + or + xor + chosen;
    }

    int shifts(int a, int b) {
        int left = a << b;
        int right = a >> b;
        int unsigned = a >>> b;
        return left + right + unsigned;
    }

    int compoundAssignments() {
        int x = 100;
        x += 5;
        x -= 2;
        x *= 3;
        x /= 4;
        x %= 17;
        x &= 0xFF;
        x |= 0x10;
        x ^= 0x55;
        x <<= 1;
        x >>= 2;
        x >>>= 1;
        String s = "value=";
        s += x;
        return x + s.length();
    }

    int unaryAndIncrements() {
        int a = 7;
        int neg = -a;
        int pos = +a;
        int flipped = ~a;
        boolean not = !(a > 0);
        int pre = ++a;
        int post = a++;
        int preDec = --a;
        int postDec = a--;
        return neg + pos + flipped + pre + post + preDec + postDec + (not ? 0 : 1);
    }

    Object castsAndIntersection() {
        int truncated = (int) 3.99;
        double widened = (double) 7;
        Object obj = (Object) "text";
        Object holder = "hello";
        Object both = (CharSequence & Comparable<CharSequence>) holder;
        return "" + truncated + widened + obj + both;
    }

    boolean typeQueries(Object obj) {
        boolean isString = obj instanceof String;
        if (obj instanceof String s) {
            return isString && s.length() > 0;
        }
        Class<?> intClass = int.class;
        Class<?> arrayClass = String[].class;
        Class<?> voidClass = void.class;
        System.out.println(intClass + " " + arrayClass + " " + voidClass);
        return isString;
    }
}
