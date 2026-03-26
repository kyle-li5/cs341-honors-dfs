#include <assert.h>
#include <stdio.h>
#include "nodeinternal.hpp"

int main(void) {
    NodeInternal node = NodeInternal(1);
    printf("Did something\n");

    off_t size = node.get_file_size("abc.txt");
    printf("Size was %lld\n", size);

    char **file_list = node.list_files();
    for (char **it = file_list; *it != NULL; ++it) {
        printf("Listed file path: %s\n", *it);
        free(*it);
    }
    free(file_list);

    off_t total_size = node.get_node_size();
    printf("total stored size is %llu\n", total_size);
}