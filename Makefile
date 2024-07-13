# Compiler to use
CC=gcc

# Compiler flags
CFLAGS=-Wall

# Target executable name
TARGET=shell

all: $(TARGET)

$(TARGET): shell.c
	$(CC) $(CFLAGS) shell.c -o $(TARGET)

clean:
	rm -f $(TARGET)