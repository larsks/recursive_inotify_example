OBJS = inotifytest.o
CFLAGS=-g

all: inotifytest

inotifytest: $(OBJS)
	$(CC) -o $@ $(OBJS)

clean:
	rm -f $(OBJS)
