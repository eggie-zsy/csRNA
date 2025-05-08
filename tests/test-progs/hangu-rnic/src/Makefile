

# all: server client test

# test: libhgrnic.c test.c
# 	gcc $^ -o ../bin/test -static

# client: libhgrnic.c librdma.c client.c
# 	gcc $^ -o ../bin/client -static

# server: libhgrnic.c librdma.c server.c
# 	gcc -D SERVER=1 -o ../bin/server -static $^


# clean:
# 	rm ../bin/test ../bin/server ../bin/client

ifdef THREAD_ID
SUFFIX := $(THREAD_ID)
all: server_thread$(SUFFIX) client_thread$(SUFFIX)

client_thread$(SUFFIX): libhgrnic.c librdma_thread$(SUFFIX).c client_thread$(SUFFIX).c
	gcc $^ -o ../bin/client_thread$(SUFFIX) -static

server_thread$(SUFFIX): libhgrnic.c librdma_thread$(SUFFIX).c server_thread$(SUFFIX).c
	gcc -D SERVER=1 -o ../bin/server_thread$(SUFFIX) -static $^

clean:
	rm -f  ../bin/server* ../bin/client*

.PHONY: all clean
else
SUFFIX :=
all: server client

client: libhgrnic.c librdma.c client.c
	gcc $^ -o ../bin/client -static

server: libhgrnic.c librdma.c server.c
	gcc -D SERVER=1 -o ../bin/server -static $^

clean:
	rm -f  ../bin/server* ../bin/client*

.PHONY: all clean
endif

