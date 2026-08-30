OBJS   = log.o engine.o main.o
TARGET = engine
CFLAGS := `pkg-config sdl3 --cflags` -D_GNU_SOURCE -Wall -Wextra -fpermissive -ggdb -Og $(CFLAGS)
LDFLAGS = `pkg-config sdl3 --libs` -lm

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS)

all: $(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: clean
