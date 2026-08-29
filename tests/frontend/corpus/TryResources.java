// corpus: try-catch-finally nesting multi-catch try-with-resources two-resources final-variable-resource throw custom-exception auto-closeable

import java.io.IOException;
import java.sql.SQLException;

class TryResources {

    static class Channel implements AutoCloseable {
        private final String name;

        Channel(String name) {
            this.name = name;
        }

        @Override
        public void close() throws IOException {
            System.out.println("closing " + name);
        }

        void send(String message) throws IOException {
            if (message == null) {
                throw new IOException("null message on " + name);
            }
            System.out.println(name + " sends " + message);
        }

        void store(String message) throws SQLException {
            if (message.isEmpty()) {
                throw new SQLException("empty message on " + name);
            }
            System.out.println(name + " stores " + message);
        }
    }

    static class AppException extends Exception {
        AppException(String message) {
            super(message);
        }
    }

    static class FatalAppException extends AppException {
        FatalAppException(String message) {
            super(message);
        }
    }

    void nested(String kind) {
        try {
            try {
                throw new AppException("inner");
            } catch (AppException inner) {
                System.out.println("inner catch: " + inner.getMessage());
            } finally {
                System.out.println("inner finally");
            }
            if (kind.isEmpty()) {
                throw new AppException("outer");
            }
        } catch (AppException e) {
            System.out.println("outer catch: " + e.getMessage());
        } finally {
            System.out.println("outer finally");
        }
    }

    String sendMessage(String text) {
        try (Channel channel = new Channel("main")) {
            channel.send(text);
            channel.store(text);
            return "sent";
        } catch (IOException | SQLException e) {
            return "failed: " + e.getMessage();
        } finally {
            System.out.println("sendMessage done");
        }
    }

    String doubleChannel(String first, String second) throws SQLException {
        try (Channel a = new Channel(first); Channel b = new Channel(second)) {
            a.send(first);
            b.send(second);
            return "both sent";
        } catch (IOException e) {
            return "io failure: " + e.getMessage();
        }
    }

    String reuseChannel() throws IOException {
        final Channel shared = new Channel("shared");
        try (shared) {
            shared.send("ping");
        }
        return "reused";
    }

    void rethrow(String kind) throws AppException {
        try {
            if ("fatal".equals(kind)) {
                throw new FatalAppException(kind);
            }
            throw new AppException(kind);
        } catch (AppException e) {
            throw e;
        }
    }
}
