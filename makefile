CC = gcc                      # The compiler to use
TARGET = server_loop               # The name of the final output file

SRC = server_loop.c             # The C source file(s)

all: $(TARGET)               # Default task is to build the target

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:                       # Clean up compiled files
	rm -f $(TARGET)
