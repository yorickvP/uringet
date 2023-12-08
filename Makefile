CXXFLAGS = -I include -std=c++20 -O1
LDLIBS = -lexecinfo -lfmt -luring
LINK.o = $(LINK.cc)

main: main.o

.PHONY: clean
clean:
	rm -f main main.o
