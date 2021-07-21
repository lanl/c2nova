###################################
# Build cpp2nova                  #
# By Scott Pakin <pakin@lanl.gov> #
###################################

CXX = clang++
CXXFLAGS = -g
LLVM_CXXFLAGS = $(shell llvm-config --cxxflags)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs --system-libs)
LIBS = \
	-Wl,--start-group \
	-lclangTooling \
	-lclangSerialization \
	-lclangBasic \
	-lclangFrontend \
	-lclangDriver \
	-lclangAST \
	-lclangLex \
	-lclangSema \
	-lclangParse \
	-lclangEdit \
	-lclangAnalysis \
	-Wl,--end-group

all: cpp2nova

cpp2nova.o: cpp2nova.cpp
	$(CXX) -c -o cpp2nova.o $(LLVM_CXXFLAGS) $(CXXFLAGS) cpp2nova.cpp

cpp2nova: cpp2nova.o
	$(CXX) -o cpp2nova cpp2nova.o $(LLVM_LDFLAGS) $(LIBS)

clean:
	$(RM) cpp2nova.o cpp2nova

.PHONY: all clean
