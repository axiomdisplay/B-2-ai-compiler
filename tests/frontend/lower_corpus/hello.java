// Literals of every family through println, plus reference equality.

public class hello {
    public static void main(String[] args) {
        System.out.println("Hello, B-2!");
        System.out.println(42);
        System.out.println(-7);
        System.out.println(10000000000L);
        System.out.println(1.5);
        System.out.println(0.5f);
        System.out.println(true);
        System.out.println('A');
        String s = "same";
        String t = "same";
        System.out.println(s == t);
        System.out.println(s != t);
    }
}
