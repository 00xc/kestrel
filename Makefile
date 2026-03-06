LLVM ?= 0
ASAN ?= 0
V ?= 0

ifeq ($(V),0)
Q := @
else
Q :=
endif

ifeq ($(LLVM),0)
CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
LD = $(CROSS_COMPILE)ld
else
CC = clang
AR = llvm-ar
LD = ld.lld
endif

ALL_CFLAGS = -Wall -Wextra -Wpedantic -O2 -std=gnu11 -flto -Iinclude/ $(CFLAGS) -march=native
ALL_LDFLAGS = -lpthread -luring $(LDFLAGS)

ifneq ($(ASAN),0)
ALL_CFLAGS += -fsanitize=address,undefined
ALL_LDFLAGS += -fsanitize=address,undefined -static-libasan
endif

SRCS       := $(wildcard src/*.c)
OBJS       := $(SRCS:.c=.o)
OBJS_DEPS  := $(OBJS:.o=.d)
TARGET     := kestrel

all: $(TARGET)

%.o: %.c
	$(info CC       $@)
	$(Q)$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS_DEPS)

$(TARGET): $(OBJS)
	$(info LD       $@)
	$(Q)$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)

fmt:
	$(Q)find src/ -name "*.c" | xargs -I{} clang-format -i {}
	$(Q)find include/ -name "*.h" | xargs -I{} clang-format -i {}

clean:
	rm -f $(OBJS)
	rm -f $(OBJS_DEPS)
	rm -f $(TARGET)

.PHONY: all clean
