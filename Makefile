CXXFLAGS = -I include -std=c++20 -O1
LDFLAGS = -luring -lfmt
LINK.o = $(LINK.cc)

main: main.o

.PHONY: clean
clean:
	rm -f main main.o
