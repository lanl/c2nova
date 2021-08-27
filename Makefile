###################################
# Build c2nova                    #
# By Scott Pakin <pakin@lanl.gov> #
###################################

prefix = /usr/local
bindir = $(prefix)/bin
INSTALL = install
CXX = clang++
CXXFLAGS = -g -Wall
LLVM_CXXFLAGS = $(shell llvm-config --cxxflags)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs --system-libs)
LIBS = \
	-Wl,--start-group \
	-lclangTooling \
	-lclangASTMatchers \
	-lclangAST \
	-lclangBasic \
	-lclangFrontend \
	-lclangRewrite \
	-lclangLex \
	-lclangSerialization \
	-lclangDriver \
	-lclangSema \
	-lclangParse \
	-lclangEdit \
	-lclangAnalysis \
	-Wl,--end-group

all: c2nova

c2nova.o: c2nova.cpp
	$(CXX) -c -o c2nova.o $(LLVM_CXXFLAGS) $(CPPFLAGS) $(CXXFLAGS) c2nova.cpp

c2nova: c2nova.o
	$(CXX) -o c2nova c2nova.o $(LLVM_LDFLAGS) $(LIBS)

install: c2nova
	$(INSTALL) -m 0755 -d $(DESTDIR)$(bindir)
	$(INSTALL) -m 0755 c2nova $(DESTDIR)$(bindir)

clean:
	$(RM) c2nova.o c2nova

.PHONY: all install clean
