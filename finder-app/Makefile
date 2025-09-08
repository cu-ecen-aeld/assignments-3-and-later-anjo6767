
# Cross-compile support:
#   Native build:  make
#   Cross build:   make CROSS_COMPILE=aarch64-none-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Wextra 

TARGET  := writer
SRCS    := writer.c
OBJS    := $(SRCS:.c=.o)

# Default target builds the writer application
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target removes the writer app and all .o files
clean:
	$(RM) $(TARGET) $(OBJS)

.PHONY: all clean
