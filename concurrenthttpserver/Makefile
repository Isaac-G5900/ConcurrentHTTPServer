CC = clang
CFLAGS = -Wall -Wextra -Werror -pedantic
TARGET = httpserver
SRCS = httpserver.c rwlock.c queue.c
OBJS = httpserver.o rwlock.o queue.o
LIBS = obfuscated_helper_funcs.a

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)