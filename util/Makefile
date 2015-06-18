#!/usr/bin/make

OPTIMIZE:=-O3
INCLDIRS:=-I. -I..
DEBUG:=-g
DEPENDFILE:= .depend

CFLAGS:=$(DEBUG) -Wall -Wstrict-prototypes -Wno-trigraphs $(OPTIMIZE) $(INCLDIRS) -fno-strict-aliasing -fno-common -fomit-frame-pointer 
CXXFLAGS:=$(DEBUG) -Wall -Wno-trigraphs $(OPTIMIZE) $(INCLDIRS) -std=c++11 -fno-strict-aliasing

all: dep cloop_create cloop_suspend 

dep:
	$(CXX) $(CXXFLAGS) -MM *.cc *.cpp > $(DEPENDFILE)

-include $(DEPENDFILE)

cloop_util: cloop_util.o compression_helpers.o Pthread.o cloop_suspend.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -llz4 -lpthread -lz -llzma -llzo2

cloop_create: cloop_util
	[ -f cloop_util ] && ln -sf $< $@

cloop_suspend: cloop_util
	[ -f cloop_util ] && ln -sf $< $@

cloop_uncompress: cloop_util
	[ -f cloop_util ] && ln -sf $< $@

extract_compressed_fs: extract_compressed_fs.o
	$(CC) -Wall -O3 -s -o $@ $< -lz

clean:
	rm -rf *.o cloop_util cloop_create cloop_suspend cloop_uncompress $(DEPENDFILE)

dist: clean
	cd .. ; \
	tar -cf - cloop/{Makefile,*.[ch],CHANGELOG,README} | \
	bzip2 -9 > $(HOME)/redhat/SOURCES/cloop.tar.bz2
