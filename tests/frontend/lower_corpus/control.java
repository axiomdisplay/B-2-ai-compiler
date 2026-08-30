// If/else, while, do-while, basic for, labeled break/continue, switch with
// fall-through and default, conditional expressions, short-circuit.

public class control {
    public static void main(String[] args) {
        int sum = 0;
        for (int i = 1; i <= 100; i++) {
            if (i % 3 == 0) {
                continue;
            }
            if (i > 50) {
                break;
            }
            sum += i;
        }
        System.out.println(sum);

        int j = 0;
        while (j < 10) {
            j += 2;
        }
        System.out.println(j);

        int k = 10;
        do {
            k--;
        } while (k > 7);
        System.out.println(k);

        int acc = 0;
        outer:
        for (int a = 0; a < 4; a++) {
            for (int bb = 0; bb < 4; bb++) {
                if (bb > a) {
                    continue outer;
                }
                if (a == 3) {
                    break outer;
                }
                acc += 1;
            }
        }
        System.out.println(acc);

        int v = 3;
        switch (v) {
            case 1:
                System.out.println("one");
                break;
            case 3:
                System.out.println("three");
            case 4:
                System.out.println("fall");
                break;
            default:
                System.out.println("other");
        }
        switch (v * 100) {
            default:
                System.out.println("big");
        }

        System.out.println(v > 1 ? "yes" : "no");
        System.out.println(true && false);
        System.out.println(true || false);
        int side = 0;
        boolean r = false && ++side == 1;
        System.out.println(r);
        System.out.println(side);

        int w = 0;
        while (w < 100 && w != 30) {
            w += 6;
        }
        System.out.println(w);
    }
}
