// corpus: literals decimal hex octal binary underscores long float double char escapes unicode text-block line-continuation

class Literals {

    void allLiterals() {
        int dec = 42;
        int big = 1_000_000;
        int hex = 0xFF;
        int hexUnderscored = 0xFF_FF;
        int octal = 0755;
        int binary = 0b1010_1010;
        long longDec = 123L;
        long longHex = 0xFFL;
        long longBig = 1_000_000L;

        double d1 = 1.5;
        double d2 = 1.;
        double d3 = 1e10;
        double d4 = 1.5E-3;
        double d5 = 0x1.8p3;
        float f1 = 1e-3f;
        float f2 = 2.5F;
        double d6 = 2.5d;
        double d7 = 1.0D;

        char c1 = 'a';
        char c2 = '\n';
        char c3 = '\t';
        char c4 = '\\';
        char c5 = '\'';
        char c6 = '\u0041';
        char c7 = '\101';
        char c8 = '\s';

        String escaped = "tab\t newline\n quote\" backslash\\ apos\' space\s octal\101";
        String unicodeStr = "\u0041BC";

        String text = """
                first line
                second line \
                continues here
                """;

        System.out.println(dec + " " + big + " " + hex + " " + hexUnderscored + " "
                + octal + " " + binary);
        System.out.println(longDec + " " + longHex + " " + longBig);
        System.out.println(d1 + " " + d2 + " " + d3 + " " + d4 + " " + d5);
        System.out.println(f1 + " " + f2 + " " + d6 + " " + d7);
        System.out.println("" + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8);
        System.out.println(escaped + " " + unicodeStr);
        System.out.println(text);
    }
}
