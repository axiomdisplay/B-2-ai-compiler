// corpus: lambda expression block-body explicit-param-types no-parens method-reference constructor-reference array-constructor this-reference cast-lambda

import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.IntFunction;
import java.util.function.Supplier;

class Lambdas {

    interface IntOp {
        int apply(int a, int b);
    }

    int helperMethod() {
        return 21;
    }

    void run() {
        Supplier<Integer> answer = () -> 42;
        Function<Integer, Integer> doubled = x -> x * 2;
        IntOp add = (int a, int b) -> a + b;

        Supplier<Integer> computed = () -> {
            int base = 10;
            int bonus = 5;
            return base + bonus;
        };

        List<String> names = new ArrayList<>();
        names.add("alpha");
        names.add("beta");
        names.forEach(name -> System.out.println(name.length()));

        Function<String, String> stringify = String::valueOf;
        Consumer<String> printer = System.out::println;
        Function<String, Integer> lengthOf = String::length;
        Supplier<ArrayList<String>> listMaker = ArrayList::new;
        IntFunction<int[]> arrayMaker = int[]::new;
        Supplier<Integer> bound = this::helperMethod;

        Runnable cast = (Runnable) () -> {
        };

        int total = answer.get() + doubled.apply(4) + computed.get() + add.apply(2, 3)
                + bound.get() + stringify.apply("7").length() + lengthOf.apply("hello")
                + listMaker.get().size() + arrayMaker.apply(3).length;
        printer.accept("total: " + total);
        cast.run();
    }
}
