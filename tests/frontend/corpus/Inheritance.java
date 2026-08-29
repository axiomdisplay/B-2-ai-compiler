// corpus: inheritance abstract protected super constructor-delegation override covariant-return final-class final-method upcast downcast instanceof three-level

abstract class Vehicle {

    protected final String name;
    protected int speed;

    protected Vehicle(String name, int speed) {
        this.name = name;
        this.speed = speed;
    }

    protected Vehicle(String name) {
        this(name, 0);
    }

    public String describe() {
        return name + " at " + speed;
    }

    public abstract double fuelFactor();

    public abstract Vehicle renamed(String newName);
}

class Car extends Vehicle {

    private final int doors;

    Car(String name, int speed, int doors) {
        super(name, speed);
        this.doors = doors;
    }

    @Override
    public double fuelFactor() {
        return 1.0 + doors * 0.1;
    }

    @Override
    public String describe() {
        return super.describe() + " with " + doors + " doors";
    }

    @Override
    public Car renamed(String newName) {
        return new Car(newName, speed, doors);
    }

    public final int doorCount() {
        return doors;
    }
}

final class SportsCar extends Car {

    SportsCar(String name, int speed) {
        super(name, speed, 2);
    }

    @Override
    public double fuelFactor() {
        return 2.0 + super.fuelFactor();
    }

    @Override
    public SportsCar renamed(String newName) {
        return new SportsCar(newName, speed);
    }
}

class Garage {

    Vehicle upcast(Car car) {
        return car;
    }

    Car downcast(Vehicle vehicle) {
        return (Car) vehicle;
    }

    String inspect(Vehicle[] fleet) {
        StringBuilder report = new StringBuilder();
        for (Vehicle v : fleet) {
            report.append(v.describe()).append("; ");
            if (v instanceof Car) {
                Car car = (Car) v;
                report.append(car.doorCount()).append(" doors; ");
            }
            if (v instanceof SportsCar sports) {
                report.append("sports fuel=").append(sports.fuelFactor()).append("; ");
            }
        }
        return report.toString();
    }

    void run() {
        Car car = new Car("sedan", 90, 4);
        SportsCar sportsCar = new SportsCar("speedy", 200);
        Vehicle v1 = upcast(car);
        Vehicle v2 = upcast(sportsCar);
        Car back = downcast(v1);
        System.out.println(inspect(new Vehicle[]{v1, v2, back.renamed("clone")}));
    }
}
