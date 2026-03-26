#include <assert.h>
#include <stdio.h>
#include "nodeinternal.hpp"

int main(void) {
    NodeInternal node = NodeInternal(1);
    printf("Did something\n");

    off_t size = node.get_file_size("abc.txt");
    printf("Size was %lld\n", size);
}