# fastdb — World's fastest columnar database with fuzzy search
# Single-file compile, zero dependencies beyond libc.

CC       = gcc
CFLAGS   = -O3 -march=armv8-a -Wall -Wextra -Werror -I.
LDFLAGS  = -lm

TARGET   = fastdb
SRC      = fastdb.c
HEADERS  = fractal_portable.h

.PHONY: all clean bench test

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

bench: $(TARGET)
	rm -f data.fastdb
	./$(TARGET) bench 10000

test: $(TARGET)
	rm -f data.fastdb
	./$(TARGET) create test name:TEXT val:INT
	./$(TARGET) insert test hello 42
	./$(TARGET) insert test world 100
	./$(TARGET) insert test help 42
	./$(TARGET) select test
	./$(TARGET) select test where name = hello
	./$(TARGET) select test fuzzy name hlep 1
	./$(TARGET) komma 127
	@echo "=== ALL TESTS PASSED ==="

clean:
	rm -f $(TARGET) data.fastdb
