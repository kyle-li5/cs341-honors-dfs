all: clean node-test

node-test:
	clang++ -Wall ./node/nodeinternal.cpp ./node/nodeinternal.hpp ./node/test.cpp -std=c++23

clean:
	rm -f a.out