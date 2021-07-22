###################################
# Build c2nova                    #
# By Scott Pakin <pakin@lanl.gov> #
###################################

CXX = clang++
CXXFLAGS = -g -std=c++17 -Wall
LLVM_CXXFLAGS = $(shell llvm-config --cxxflags)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs --system-libs)
LIBS = \
	-Wl,--start-group \
	-lclangTooling \
	-lclangASTMatchers \
	-lclangAST \
	-lclangBasic \
	-lclangToolingCore \
	-lclangLex \
	-lclangSerialization \
	-lclangFrontend \
	-lclangDriver \
	-lclangRewrite \
	-lclangFormat \
	-lclangSema \
	-lclangParse \
	-lclangEdit \
	-lclangToolingInclusions \
	-lclangAnalysis \
	-Wl,--end-group

all: c2nova

c2nova.o: c2nova.cpp
	$(CXX) -c -o c2nova.o $(LLVM_CXXFLAGS) $(CXXFLAGS) c2nova.cpp

c2nova: c2nova.o
	$(CXX) -o c2nova c2nova.o $(LLVM_LDFLAGS) $(LIBS)

clean:
	$(RM) c2nova.o c2nova

.PHONY: all clean
