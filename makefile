CC=gcc
CFLAGS=-g -Wall
TARGET = main
LIBS = `pkg-config --libs libuv zlib`

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c $(LIBS)

run: $(TARGET)
	./$(TARGET)
	
clean:
	rm $(TARGET)

.PHONY: run clean