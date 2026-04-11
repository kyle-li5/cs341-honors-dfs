#include <assert.h>
#include <stdio.h>
#include "nodeinternal.hpp"
#include <filesystem>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

void test1(void);
void test2(void);
void test3(void);
void test4(void);

void test1node(NodeInternal &node);
void clean_up_files(void);
void test_duplicate_streams(int fd1, int fd2);
void print_stream(int fd);

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

    int abc_fd_orig = open("./node/test-files/abc-orig.txt", O_RDONLY);
    int abc_fd_node = node.read_file("abc.txt");
    test_duplicate_streams(abc_fd_orig, abc_fd_node);

    int abc2_fd = open("./node/test-files/abc2-orig.txt", O_RDONLY);
    assert(node.create_file("abc.txt", abc2_fd) == 1);
    assert(node.replace_file("abc.txt", abc2_fd) == 0);
    assert(node.get_file_size("abc.txt") == 24);
    assert(node.get_node_size() == 24);

    int abc2_fd_orig = open("./node/test-files/abc2-orig.txt", O_RDONLY);
    int abc2_fd_node = node.read_file("abc.txt");
    test_duplicate_streams(abc2_fd_orig, abc2_fd_node);

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

void test_duplicate_streams(int fd1, int fd2) {
    ssize_t num_read1 = 1;
    ssize_t num_read2 = 1;

    char buf1[256];
    char buf2[256];

    while (num_read1 != 0) {
        num_read1 = read(fd1, &buf1, 256);
        assert(num_read1 >= 0);

        num_read2 = read(fd2, &buf2, 256);
        assert(num_read2 >= 0);

        assert(num_read1 == num_read2);
        for (size_t i = 0; i < num_read1; ++i) {
            assert(buf1[i] == buf2[i]);
        }
    }

    assert(close(fd1) == 0);
    assert(close(fd2) == 0);
}

void print_stream(int fd) {
    char c;
    int result = read(fd, &c, 1);
    while (result > 0) {
        printf("%c", c);
        result = read(fd, &c, 1);
    }
    printf("\nfd: %d, result: %d\n", fd, result);
    if (result != 0) {
        perror(NULL);
    }
    close(fd);
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