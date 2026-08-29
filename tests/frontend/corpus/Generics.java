// corpus: generics bounded-type-parameter multiple-bounds wildcard diamond raw-type generic-method generic-return nested-generic-field

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

class Box<T extends Comparable<T> & java.io.Serializable> {
    private final T value;

    Box(T value) {
        this.value = value;
    }

    T value() {
        return value;
    }

    int compareTo(Box<T> other) {
        return value.compareTo(other.value);
    }
}

class GenericOps {

    Map<String, List<int[]>> arraysByName = new HashMap<>();

    static <U extends Comparable<? super U>> U max(List<U> items) {
        U best = items.get(0);
        for (U item : items) {
            if (item.compareTo(best) > 0) {
                best = item;
            }
        }
        return best;
    }

    static double sumAll(List<? extends Number> numbers) {
        double total = 0.0;
        for (Number number : numbers) {
            total += number.doubleValue();
        }
        return total;
    }

    static void fill(List<? super Integer> sink) {
        sink.add(Integer.valueOf(1));
        sink.add(2);
    }

    static List<String> names() {
        List<String> result = new ArrayList<>();
        result.add("alpha");
        result.add("beta");
        return result;
    }

    @SuppressWarnings("rawtypes")
    static int rawSize(ArrayList rawList) {
        return rawList.size();
    }

    void run() {
        Box<String> box = new Box<>("hello");
        String best = max(List.of("delta", "alpha", "echo"));
        List<Integer> ints = List.of(3, 1, 2);
        arraysByName.put("small", List.of(new int[]{1, 2}, new int[]{3}));
        arraysByName.put("empty", new ArrayList<int[]>());

        double total = sumAll(ints) + box.value().length() + best.length();
        fill(new ArrayList<Integer>());
        int raw = rawSize(new ArrayList<String>());
        List<String> all = names();

        System.out.println(total + " " + raw + " " + all.size() + " "
                + box.compareTo(new Box<>("world")));
    }
}
