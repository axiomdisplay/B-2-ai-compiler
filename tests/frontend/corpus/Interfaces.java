// corpus: interface constants abstract default-method static-method private-method nested-interface nested-class multiple-implements

interface Widget {

    String KIND = "widget";

    String label();

    default String loudLabel() {
        return label().toUpperCase();
    }

    default String bracketed() {
        return "[" + quietPrefix() + loudLabel() + "]";
    }

    static Widget simple(String name) {
        return () -> name;
    }

    private String quietPrefix() {
        return "widget:";
    }

    interface Audit {
        long id();
    }

    class BasicAudit implements Audit {
        @Override
        public long id() {
            return 42L;
        }
    }
}

interface Printable {
    void print();
}

class Panel implements Widget, Printable {

    private final String title;

    Panel(String title) {
        this.title = title;
    }

    @Override
    public String label() {
        return title;
    }

    @Override
    public void print() {
        System.out.println(bracketed());
    }
}

class InterfacesDemo {

    void run() {
        Widget widget = Widget.simple("lambda");
        System.out.println(widget.bracketed());
        Widget.BasicAudit audit = new Widget.BasicAudit();
        System.out.println(Widget.KIND + " " + audit.id());
        Panel panel = new Panel("main");
        panel.print();
    }
}
