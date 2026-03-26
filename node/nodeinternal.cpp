#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <filesystem>
#include <sys/stat.h>
#include <vector>
#include <queue>

#include "nodeinternal.hpp"

namespace fs = std::filesystem;



NodeInternal::NodeInternal() {
    node_id = -1;
    directory_path = realpath("./node-data/default", nullptr);

    fs::create_directory(fs::path("./node-data"));
    fs::create_directory(fs::path("./node-data/default"));
    fs::create_directory(fs::path("./node-data/default/storage"));
}

NodeInternal::NodeInternal(int node_id) {
    this->node_id = node_id;

    char buf[64];
    snprintf(buf, 64, "./node-data/%d", node_id);
    directory_path = realpath(buf, nullptr);

    fs::create_directory(fs::path("./node-data"));
    fs::create_directory(fs::path(buf));

    snprintf(buf, 64, "./node-data/%d/storage", node_id);
    fs::create_directory(fs::path(buf));
}

off_t NodeInternal::get_node_size() {
    return 0; // TODO
}

int NodeInternal::contains_file(const char *filename) {
    return fs::exists(get_fs_path(filename));
}

char **NodeInternal::list_files() {
    std::vector<fs::path> file_paths;
    std::queue<fs::directory_iterator> to_parse;

    // Add storage directory iterator to queue to parse later.
    char storage_path[PATH_MAX];
    snprintf(storage_path, PATH_MAX, "%s/storage", directory_path);
    fs::path storage_fs_path = fs::path(storage_path);
    fs::directory_iterator top_it(storage_fs_path);

    to_parse.push(top_it);

    // Define end iterator. This end is shared among all directory_iterators.
    fs::directory_iterator end;

    // Collect file paths or add new directory iterators to queue for all iterators
    while (!to_parse.empty()) {
        fs::directory_iterator it = to_parse.front();
        to_parse.pop();

        // TODO
    }



    return NULL; // TODO
}

off_t NodeInternal::get_file_size(const char *filename) {
    std::uintmax_t size = fs::file_size(get_fs_path(filename));
    return (off_t) size;
}

int NodeInternal::create_file(const char *filename, int input) {
    return -1; // TODO
}

int NodeInternal::replace_file(const char *filename, int input) {
    return -1; // TODO
}

int NodeInternal::delete_file(const char *filename) {
    return !fs::remove(get_fs_path(filename));
}

int NodeInternal::read_file(const char *filename) {
    return -1; // TODO
}

NodeInternal::~NodeInternal() {
    free(directory_path);
}





char *NodeInternal::get_stored_filename(const char *filename) {
    char *path = (char*) malloc(PATH_MAX);
    snprintf(path, PATH_MAX, "%s/storage/%s", directory_path, filename);
    return path;
}

fs::path NodeInternal::get_fs_path(const char *filename) {
    char *str_path = get_stored_filename(filename);
    fs::path fs_path = fs::path(str_path);
    free(str_path);
    return fs_path;
}