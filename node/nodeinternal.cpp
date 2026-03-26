#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <filesystem>

#include "nodeinternal.hpp"



NodeInternal::NodeInternal() {
    node_id = -1;
    directory_name = realpath("./node-data/default", nullptr);
    total_file_size = 0;
}

NodeInternal::NodeInternal(int node_id) {
    this->node_id = node_id;

    char buf[64];
    snprintf(buf, 64, "./node-data/%d", node_id);
    directory_name = realpath("./node-data/default", nullptr);

    total_file_size = 0;
}

size_t NodeInternal::get_node_size() {
    return total_file_size;
}

int NodeInternal::contains_file(char *filename) {
    return std::filesystem::exists(get_fs_path(filename));
}

char **NodeInternal::list_files() {
    return NULL; // TODO
}

size_t NodeInternal::get_file_size(char *filename) {
    return 0; // TODO
}

int NodeInternal::create_file(char *filename, int input) {
    return -1; // TODO
}

int NodeInternal::replace_file(char *filename, int input) {
    return -1; // TODO
}

int NodeInternal::delete_file(char *filename) {
    return -1; // TODO
}

int NodeInternal::read_file(char *filename) {
    return -1; // TODO
}

NodeInternal::~NodeInternal() {
    free(directory_name);
}





char *NodeInternal::get_stored_filename(char *filename) {
    char *path = (char*) malloc(PATH_MAX);
    snprintf(path, PATH_MAX, "%s/storage/%s", directory_name, filename);
    return path;
}

std::filesystem::path NodeInternal::get_fs_path(char *filename) {
    char *str_path = get_stored_filename(filename);
    std::filesystem::path fs_path = std::filesystem::path(str_path);
    free(str_path);
    return fs_path;
}