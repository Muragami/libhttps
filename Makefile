CC = clang
CFLAGS = -std=c99 -m64 -Wall
OPTFLAGS ?= -o3
OBJS = ./obj/
SRCS = ./src/

WLIBS = -lwinhttp ./lib/lua51.dll
MLIBS = -framework Foundation ./lib/libluajit.dylib
LLIBS = -lpthread -lcurl -lluajit-5.1

windows: $(OBJS)libhttps.dll

$(OBJS)libhttps.dll: $(OBJS)naett.w.o $(OBJS)xthread.w.o $(OBJS)https.w.o $(OBJS)memio.w.o
	clang $(CFLAGS) $(OPTFLAGS) -shared -o $(OBJS)libhttps.dll $(OBJS)https.w.o $(OBJS)xthread.w.o $(OBJS)naett.w.o $(OBJS)memio.w.o $(WLIBS)

$(OBJS)naett.w.o: $(SRCS)naett.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -c $(SRCS)naett.c -o $(OBJS)naett.w.o

$(OBJS)xthread.w.o: $(SRCS)xthread.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -c $(SRCS)xthread.c -o $(OBJS)xthread.w.o

$(OBJS)https.w.o: https.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -c https.c -o $(OBJS)https.w.o

$(OBJS)memio.w.o: $(SRCS)memio.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -c $(SRCS)memio.c -o $(OBJS)memio.w.o	

linux: $(OBJS)libhttps.so

$(OBJS)libhttps.so: $(OBJS)naett.l.o $(OBJS)xthread.l.o $(OBJS)https.l.o $(OBJS)memio.l.o
	clang $(CFLAGS) $(OPTFLAGS) -fPIC -shared -o $(OBJS)libhttps.so $(OBJS)https.l.o $(OBJS)xthread.l.o $(OBJS)naett.l.o $(OBJS)memio.l.o $(LLIBS)

$(OBJS)naett.l.o: $(SRCS)naett.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -fPIC -c $(SRCS)naett.c -o $(OBJS)naett.l.o

$(OBJS)xthread.l.o: $(SRCS)xthread.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -fPIC -c $(SRCS)xthread.c -o $(OBJS)xthread.l.o

$(OBJS)https.l.o: https.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -fPIC -c https.c -o $(OBJS)https.l.o

$(OBJS)memio.l.o: $(SRCS)memio.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -c $(SRCS)memio.c -o $(OBJS)memio.l.o

macos: $(OBJS)libhttps.dylib

$(OBJS)libhttps.dylib: $(OBJS)naett.m.o $(OBJS)xthread.m.o $(OBJS)https.m.o $(OBJS)memio.m.o
	clang $(CFLAGS) $(OPTFLAGS) -fPIC -shared -o $(OBJS)libhttps.dylib $(OBJS)https.m.o $(OBJS)xthread.m.o $(OBJS)memio.m.o $(OBJS)naett.m.o $(MLIBS)

$(OBJS)naett.m.o: $(SRCS)naett.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -fPIC -c $(SRCS)naett.c -o $(OBJS)naett.m.o

$(OBJS)xthread.m.o: $(SRCS)xthread.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -fPIC -c $(SRCS)xthread.c -o $(OBJS)xthread.m.o

$(OBJS)https.m.o: https.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -fPIC -c https.c -o $(OBJS)https.m.o

$(OBJS)memio.m.o: $(SRCS)memio.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -c $(SRCS)memio.c -o $(OBJS)memio.m.o

clean:
	rm $(OBJS)*
