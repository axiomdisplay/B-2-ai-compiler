// corpus: arrays creation multidimensional jagged initializer nested-initializer c-style-declarator length array-access enhanced-for 2d-iteration

class Arrays {

    void demo() {
        int[] sizes = new int[10];
        int[][] grid = new int[3][4];
        int[][] jagged = new int[3][];
        jagged[0] = new int[1];
        jagged[1] = new int[2];
        jagged[2] = new int[]{7, 8, 9};

        int[] init = {1, 2, 3};
        int[][] nested = {{1, 2}, {3}};
        String[] words = {"alpha", "beta"};
        int[] more = new int[]{4, 5, 6};

        int a[];
        int b[], c;
        a = init;
        b = more;
        c = 9;

        sizes[0] = a.length + b.length + c;
        sizes[1] = words.length + nested.length + jagged.length;

        int total = 0;
        for (int i = 0; i < sizes.length; i++) {
            total += sizes[i];
        }
        for (int value : init) {
            total += value;
        }
        for (String word : words) {
            total += word.length();
        }
        for (int row = 0; row < grid.length; row++) {
            for (int col = 0; col < grid[row].length; col++) {
                grid[row][col] = row + col;
                total += grid[row][col] + nested[row % 2][0] + jagged[row][row % 3];
            }
        }
        System.out.println(total + " " + sizes[0] + " " + sizes[1]);
    }
}
