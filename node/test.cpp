#include <assert.h>
#include <stdio.h>
#include "nodeinternal.hpp"
#include <filesystem>
#include <fcntl.h>
#include <string.h>

void test1(void);
void test2(void);
void test3(void);
void test4(void);

void test1node(NodeInternal &node);
void clean_up_files(void);

// WARNING - this test will delete the node-data directory if it exists

int main(void) {
    printf("Starting tests.\n");

    clean_up_files();

    test1();
    test2();
    test3();
    test4();

    clean_up_files();

    printf("All tests passed.\n");
}

void test1node(NodeInternal &node) { // Assumes no content in node to start with
    assert(!node.contains_file("abc.txt"));
    int abc_fd = open("./node/test-files/abc-orig.txt", O_RDONLY);
    assert(node.create_file("abc.txt", abc_fd) == 0);
    assert(node.contains_file("abc.txt"));
    assert(node.get_file_size("abc.txt") == 4);
    assert(node.get_node_size() == 4);

    int abc2_fd = open("./node/test-files/abc2-orig.txt", O_RDONLY);
    assert(node.create_file("abc.txt", abc2_fd) == 1);
    assert(node.replace_file("abc.txt", abc2_fd) == 0);
    assert(node.get_file_size("abc.txt") == 24);
    assert(node.get_node_size() == 24);

    char **file_list = node.list_files();
    assert(strcmp(file_list[0], "abc.txt") == 0);
    assert(file_list[1] == NULL);
    free(file_list[0]);
    free(file_list);

    assert(node.delete_file("abc.txt") == 0);
    assert(node.delete_file("abc.txt") == 1);

    file_list = node.list_files();
    assert(file_list[0] == NULL);
    free(file_list);
}

void clean_up_files(void) {
    std::filesystem::path p("./node-data");
    std::filesystem::remove_all(p);
}

void test1(void) {
    // Start fresh
    std::filesystem::path p("./node-data");
    std::filesystem::remove_all(p);

    // Basic tests
    NodeInternal node = NodeInternal();

    test1node(node);
}

void test2(void) {
    // Start fresh
    std::filesystem::path p("./node-data");
    std::filesystem::remove_all(p);

    // Basic tests
    NodeInternal node = NodeInternal(1);

    test1node(node);
}

void test3(void) {
    // Start fresh
    std::filesystem::path p("./node-data");
    std::filesystem::remove_all(p);

    // Test with one node
    NodeInternal node1 = NodeInternal(1);
    test1node(node1);

    // Then test with another
    NodeInternal node2 = NodeInternal(2);
    test1node(node2);
}

void test4(void) {
    // Start fresh
    clean_up_files();

    // Test with many nodes
    for (int i = 0; i < 500; ++i) {
        NodeInternal node = NodeInternal(i);
        test1node(node);
    }
}