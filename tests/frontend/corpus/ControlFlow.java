// corpus: control-flow if-else while do-while for comma-init empty-for enhanced-for labeled-break labeled-continue switch-fallthrough return

class ControlFlow {

    int classify(int n) {
        if (n < 0) {
            return -1;
        } else if (n == 0) {
            return 0;
        } else if (n < 10) {
            return 1;
        } else {
            return 2;
        }
    }

    int findFirst(int[] data, int target) {
        for (int i = 0; i < data.length; i++) {
            if (data[i] == target) {
                return i;
            }
        }
        return -1;
    }

    int loops(int[] data) {
        int total = 0;

        int i = 0;
        while (i < data.length) {
            total += data[i];
            i++;
        }

        int j = 0;
        do {
            j++;
            if (j == 3) {
                continue;
            }
        } while (j < 5);

        for (int k = 0, m = data.length - 1; k < m; k++, m--) {
            total += k + m;
        }

        for (;;) {
            total++;
            if (total > 100) {
                break;
            }
        }

        for (int value : data) {
            total += value;
        }

        for (var value : data) {
            total += value;
        }

        return total + j;
    }

    int search(int[][] grid, int target) {
        int hits = 0;
        outer:
        for (int row = 0; row < grid.length; row++) {
            inner:
            for (int col = 0; col < grid[row].length; col++) {
                if (grid[row][col] == 0) {
                    continue outer;
                }
                if (grid[row][col] == target) {
                    hits++;
                    break outer;
                }
            }
        }
        return hits;
    }

    String describeCode(int code) {
        String result = "";
        switch (code) {
            case 1:
                result += "one ";
            case 2:
                result += "two ";
                break;
            case 3:
                result += "three ";
            case 4:
            case 5:
                result += "many ";
                break;
            default:
                result += "none";
        }
        return result;
    }
}
