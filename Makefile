
VERSION	= 0.0.4
DIST	= aacam-$(VERSION)
CC	= gcc
CFLAGS	= -g -O2 -D_REENTRANT
LD	= gcc
LDFLAGS	= 
BIN	= aacam
LIBS	= -lpthread -laa 
OBJS	= cam.o
DFILES	= README $(OBJS:.o=.c)

.c.o:
	$(CC) -c $(CFLAGS) -o $*.o $<

all: $(BIN)

$(BIN): $(OBJS)
	$(LD) $(LDFLAGS) -o$@ $+ $(LIBS)

clean:
	rm -f core $(OBJS) $(BIN)

distclean: clean

dist:
	rm -Rf $(DIST)
	mkdir $(DIST)
	cp $(DFILES) $(DIST)
	tar cvf - $(DIST) | gzip -c > $(DIST).tar.gz
	rm -Rf $(DIST)
	sync
	ls -l $(DIST).tar.gz
