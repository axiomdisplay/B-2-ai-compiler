// corpus: negative missing-semicolon

class MissingSemi {

    void broken() {
        int x = 42
        int y = 43;
        System.out.println(x + y);
    }
}
