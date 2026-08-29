// corpus: package-declaration imports single-type on-demand static-import var try-with-resources underscore-separator string-concat annotation comments

package corpus;

import java.io.IOException;
import java.util.*;
import static java.lang.Math.max;
import static java.lang.Math.*;

class VarAndMisc {

    /**
     * Javadoc comment for the demo method.
     */
    void demo() {
        /* block comment
           spanning multiple lines */
        // line comment
        var list = new ArrayList<String>();
        list.add("one");

        List<String> names = List.of("a", "bb", "ccc");
        for (var name : names) {
            list.add(name);
        }

        int big = 1_000_000;
        long bigger = 1_000_000_000L;

        String joined = "a" + "b" + "c" + big + bigger + list.size();

        List raw = list;
        @SuppressWarnings("unchecked")
        var typed = (List<String>) raw;

        var radius = 2.5;
        double area = PI * radius * radius;
        int larger = max(3, 4);

        try (var reader = new java.io.StringReader("payload")) {
            int first = reader.read();
            System.out.println(joined + " " + first + " " + area + " " + larger + " "
                    + typed.size());
        } catch (IOException e) {
            System.out.println("io error");
        }
    }
}
