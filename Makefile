# Compiler to use
CC=gcc

# Compiler flags
CFLAGS=-Wall

# Target executable name
TARGET=cpsh

# Directory for binaries
BINDIR=bin

# Source directory
SRCDIR=src

# Default target
all: $(BINDIR)/$(TARGET)

$(BINDIR)/$(TARGET): $(SRCDIR)/shell.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(BINDIR)/$(TARGET)