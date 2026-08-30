// Array creation with sizes and initializers, element stores/loads with
// family-specific ops, arraylength, enhanced-for over arrays, var with
// array initializers.

public class arrays {
    public static void main(String[] args) {
        int[] nums = {5, 10, 15, 20};
        int total = 0;
        for (int n : nums) {
            total += n;
        }
        System.out.println(total);
        System.out.println(nums.length);

        int[] built = new int[3];
        built[0] = 7;
        built[2] = 9;
        System.out.println(built[0] + built[1] + built[2]);
        System.out.println(built[1]);

        long[] bigs = {1L, 2L, 3L};
        System.out.println(bigs[1] + bigs[2]);

        double[] ds = new double[2];
        ds[0] = 0.25;
        System.out.println(ds[0] + ds[1]);

        boolean[] flags = new boolean[2];
        System.out.println(flags[0]);
        flags[1] = true;
        System.out.println(flags[1]);

        char[] chars = {'x', 'y'};
        System.out.println(chars[0]);

        byte[] bytes = new byte[1];
        bytes[0] = (byte) 300;
        System.out.println(bytes[0]);

        String[] words = {"a", "b", "c"};
        System.out.println(words.length);
        System.out.println(words[1]);

        var inferred = new int[]{2, 4, 6};
        System.out.println(inferred[1]);

        int idx = 0;
        nums[idx] = 50;
        System.out.println(nums[0]);

        int i = 0;
        int sum = 0;
        while (i < nums.length) {
            sum += nums[i];
            i++;
        }
        System.out.println(sum);
    }
}
