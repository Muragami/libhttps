CC = clang
CFLAGS = -std=c99 -m64 -Wall -o3
OBJS = ./obj/
SRCS = ./src/

WLIBS = -lwinhttp ./lib/lua51.dll
MLIBS = -lpthread -lcurl ./lib/lua51.dll
LLIBS = -lpthread -lcurl ./lib/lua51.dll

windows: $(OBJS)libhttps.dll

$(OBJS)libhttps.dll: $(OBJS)naett.w.o $(OBJS)xthread.w.o $(OBJS)https.w.o
	clang $(CFLAGS) -shared -o $(OBJS)libhttps.dll $(OBJS)https.w.o $(OBJS)xthread.w.o $(OBJS)naett.w.o $(WLIBS)

$(OBJS)naett.w.o: $(SRCS)naett.c
	$(CC) $(CFLAGS) -c $(SRCS)naett.c -o $(OBJS)naett.w.o

$(OBJS)xthread.w.o: $(SRCS)xthread.c
	$(CC) $(CFLAGS) -c $(SRCS)xthread.c -o $(OBJS)xthread.w.o

$(OBJS)https.w.o: https.c
	$(CC) $(CFLAGS) -c https.c -o $(OBJS)https.w.o

linux: $(OBJS)libhttps.so

$(OBJS)libhttps.so: $(OBJS)naett.l.o $(OBJS)xthread.l.o $(OBJS)https.l.o
	clang $(CFLAGS) -fPIC -shared -o $(OBJS)libhttps.so $(OBJS)https.l.o $(OBJS)xthread.l.o $(OBJS)naett.l.o $(LLIBS)

$(OBJS)naett.l.o: $(SRCS)naett.c
	$(CC) $(CFLAGS) -fPIC -c $(SRCS)naett.c -o $(OBJS)naett.l.o

$(OBJS)xthread.l.o: $(SRCS)xthread.c
	$(CC) $(CFLAGS) -fPIC -c $(SRCS)xthread.c -o $(OBJS)xthread.l.o

$(OBJS)https.l.o: https.c
	$(CC) $(CFLAGS) -fPIC -c https.c -o $(OBJS)https.l.o

macos: $(OBJS)libhttps.dylib

$(OBJS)libhttps.dylib: $(OBJS)naett.m.o $(OBJS)xthread.m.o $(OBJS)https.m.o
	clang $(CFLAGS) -fPIC -shared -o $(OBJS)libhttps.dylib $(OBJS)https.m.o $(OBJS)xthread.m.o $(OBJS)naett.m.o $(MLIBS)

$(OBJS)naett.m.o: $(SRCS)naett.c
	$(CC) $(CFLAGS) -fPIC -c $(SRCS)naett.c -o $(OBJS)naett.m.o

$(OBJS)xthread.m.o: $(SRCS)xthread.c
	$(CC) $(CFLAGS) -fPIC -c $(SRCS)xthread.c -o $(OBJS)xthread.m.o

$(OBJS)https.m.o: https.c
	$(CC) $(CFLAGS) -fPIC -c https.c -o $(OBJS)https.m.o

clean:
	rm $(OBJS)*
