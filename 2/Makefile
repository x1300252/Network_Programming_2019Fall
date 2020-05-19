CC = g++
CFLAGS = -Wall
OBJS = np_simple np_single_proc np_multi_proc

all: $(OBJS)

%: %.cpp
	$(CC) $(CFLAGS) $^ -o $@ -pthread

.PHONY: clean
clean:
	rm $(OBJS)