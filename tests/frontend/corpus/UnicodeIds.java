// corpus: unicode-identifiers non-ascii-letters dollar-sign keyword-prefixed-identifiers

class UnicodeIds {

    public static void main(String[] args) {
        int 名前 = 3;
        String über = "value";
        double λ = 2.5;
        int my$var = 1;
        String $str = "s";
        int recordX = 10;
        int yieldPoint = 20;
        String varName = "v";

        System.out.println(名前 + " " + über + " " + λ);
        System.out.println(my$var + " " + $str);
        System.out.println(recordX + " " + yieldPoint + " " + varName);
    }
}
