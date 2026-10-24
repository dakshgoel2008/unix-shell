CC      = gcc
CFLAGS  = -Wall -Wextra -g
SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:src/%.c=build/%.o)
TARGET  = shell

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

build/%.o: src/%.c src/shell.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

test: $(TARGET)
	./tests/run_tests.sh

# feeds the test commands through the shell under valgrind and fails on any leak
valgrind: $(TARGET)
	valgrind --leak-check=full --error-exitcode=1 ./$(TARGET) < tests/valgrind_input.txt > /dev/null

clean:
	rm -rf build $(TARGET)

.PHONY: all test valgrind clean
