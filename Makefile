CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
CFLAGS += -pthread
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
CPPFLAGS += -I/opt/X11/include
LDFLAGS += -L/opt/X11/lib -Wl,-rpath,/opt/X11/lib
LDLIBS += -lX11 -pthread
else
CPPFLAGS += $(shell pkg-config --cflags x11 2>/dev/null)
LDLIBS += $(shell pkg-config --libs x11 2>/dev/null || echo -lX11) -pthread -lrt
endif

SOURCES = code/main.c code/gui.c code/shell.c code/job_manager.c code/history.c code/completion_scripts.c 
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean run process
all: terminator

terminator: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

code/%.o: code/%.c code/terminator.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: terminator
	./terminator

process: terminator
	./terminator useProcess=True

clean:
	rm -f $(OBJECTS) terminator
