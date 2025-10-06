# Cross-compile support:
#   Native build:  make
#   Cross build:   make CROSS_COMPILE=aarch64-none-linux-gnu-
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra 
LDLIBS ?=
LDLIBS += -pthread
RM ?= rm -f

TARGET ?= aesdsocket
SRCS := aesdsocket.c
OBJS := $(SRCS:.c=.o)

# Default target builds the aesdsocket application 
all: $(TARGET)
default: all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target removes the aesdsocket and all .o files 
clean:
	$(RM) $(TARGET) $(OBJS)
