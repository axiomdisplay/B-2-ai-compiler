// corpus: hello-world class method main string-literal escapes

public class HelloWorld {

    public static void main(String[] args) {
        System.out.println("Hello, World!");
        System.out.println("line one\nline two");
        System.out.println("quoted: \"B-2\"");
        System.out.println("backslash: \\ done");
        System.out.println("tab\there");
        if (args.length == 0) {
            System.out.println("no args");
        } else {
            System.out.println("got " + args.length + " args");
        }
    }
}
