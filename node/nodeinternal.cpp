#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <filesystem>
#include <sys/stat.h>
#include <vector>
#include <queue>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "nodeinternal.hpp"

namespace fs = std::filesystem;



NodeInternal::NodeInternal() {
    node_id = -1;

    fs::create_directory(fs::path("./node-data"));
    fs::create_directory(fs::path("./node-data/default"));
    fs::create_directory(fs::path("./node-data/default/storage"));

    directory_path = realpath("./node-data/default", nullptr);

    storage_skip_amt = strlen(directory_path) + 9;

    cur_modifying = 0;

    // init metadata files

    int fd_status = open("./node-data/default/status.dat", O_WRONLY | O_CREAT, 0b110110110);
    close(fd_status);
    status_path = realpath("./node-data/default/status.dat", nullptr);

    int fd_modifying = open("./node-data/default/modifying.dat", O_WRONLY | O_CREAT, 0b110110110);
    close(fd_modifying);
    modifying_path = realpath("./node-data/default/modifying.dat", nullptr);
}

NodeInternal::NodeInternal(int node_id) {
    this->node_id = node_id;

    char buf[64];
    snprintf(buf, 64, "./node-data/%d", node_id);

    fs::create_directory(fs::path("./node-data"));
    fs::create_directory(fs::path(buf));

    directory_path = realpath(buf, nullptr);

    snprintf(buf, 64, "./node-data/%d/storage", node_id);
    fs::create_directory(fs::path(buf));

    storage_skip_amt = strlen(directory_path) + 9;

    cur_modifying = 0;

    // init metadata files

    snprintf(buf, 64, "./node-data/%d/status.dat", node_id);
    int fd_status = open(buf, O_WRONLY | O_CREAT, 0b110110110);
    close(fd_status);
    status_path = realpath(buf, nullptr);

    snprintf(buf, 64, "./node-data/%d/modifying.dat", node_id);
    int fd_modifying = open(buf, O_WRONLY | O_CREAT, 0b110110110);
    close(fd_modifying);
    modifying_path = realpath(buf, nullptr);
}

void NodeInternal::clear_existing_data(void) {
    int fd_status = open(status_path, O_WRONLY | O_TRUNC);
    write(fd_status, "c", 1); // clearing node
    close(fd_status);

    int fd_modifying = open(modifying_path, O_WRONLY | O_TRUNC);
    close(fd_modifying);

    char buf[256];
    snprintf(buf, 256, "%s/storage", directory_path);
    fs::path storage_path = fs::path(buf);
    fs::remove_all(storage_path);
    fs::create_directory(storage_path);

    fd_status = open(status_path, O_WRONLY | O_TRUNC);
    write(fd_status, "i", 1); // idle
    close(fd_status);
}

off_t NodeInternal::get_node_size() {
    off_t size = 0;

    // Create iterator for files
    char storage_path[PATH_MAX];
    snprintf(storage_path, PATH_MAX, "%s/storage", directory_path);
    fs::path storage_fs_path = fs::path(storage_path);
    fs::recursive_directory_iterator it(storage_fs_path);

    // Define end iterator
    fs::recursive_directory_iterator end;
    
    // Sum sizes
    for (; it != end; ++it) {
        if (it->is_regular_file()) {
            size += it->file_size();
        }
    }

    return size;
}

int NodeInternal::contains_file(const char *filename) {
    return contains(filename);
}

int NodeInternal::contains(const char *path) {
    return fs::exists(get_fs_path(path));
}

char **NodeInternal::list_files() {
    std::vector<fs::path> file_paths;

    // Create iterator for files
    char storage_path[PATH_MAX];
    snprintf(storage_path, PATH_MAX, "%s/storage", directory_path);
    fs::path storage_fs_path = fs::path(storage_path);
    fs::recursive_directory_iterator it(storage_fs_path);

    // Define end iterator
    fs::recursive_directory_iterator end;
    
    // Collect all file paths
    for (; it != end; ++it) {
        if (it->is_regular_file()) {
            file_paths.push_back(it->path());
        }
    }

    // Convert paths into char*'s relative to the storage directory
    char **list = (char**) malloc(sizeof(char*) * (file_paths.size() + 1));
    list[file_paths.size()] = NULL;
    for (size_t i = 0; i < file_paths.size(); i++) {
        const char *orig = file_paths[i].c_str();
        size_t orig_len = strlen(orig);
        size_t amt_to_cpy = orig_len - storage_skip_amt;
        list[i] = (char*) malloc(amt_to_cpy + 1);
        strncpy(list[i], orig + storage_skip_amt, amt_to_cpy + 1);
    }

    return list;
}

off_t NodeInternal::get_file_size(const char *filename) {
    fs::path fs_path = get_fs_path(filename);
    if (fs::is_directory(fs_path)) {
        // sum size of contents
        off_t size = 0;

        // Create iterator for files
        fs::recursive_directory_iterator it(fs_path);

        // Define end iterator
        fs::recursive_directory_iterator end;
        
        // Sum sizes
        for (; it != end; ++it) {
            if (it->is_regular_file()) {
                size += it->file_size();
            }
        }

        return size;

    } else if (fs::is_regular_file(fs_path)) {
        // check size normally
        std::uintmax_t size = fs::file_size(get_fs_path(filename));
        return (off_t) size;

    } else {
        // Error
        return -1;
    }
}

int NodeInternal::create_file(const char *filename, int input) {
    indicate_start_modifying(filename);

    if (contains_file(filename)) {
        indicate_end_modifying();
        return 1;
    }

    char *path_str = get_stored_filename(filename);
    int output = open(path_str, O_WRONLY | O_CREAT, 0b110110110);
    free(path_str);

    char buf[4096];
    ssize_t read_bytes = 0;

    while ((read_bytes = read(input, buf, 2048)) > 0) {
        write(output, buf, read_bytes);
    }

    close(output);
    close(input);

    indicate_end_modifying();
    return 0;
}

int NodeInternal::replace_file(const char *filename, int input) {
    indicate_start_modifying(filename);

    if (!contains_file(filename)) {
        indicate_end_modifying();
        return 1;
    }

    delete_file(filename);
    create_file(filename, input);

    indicate_end_modifying();
    return 0;
}

int NodeInternal::delete_file(const char *filename) {
    indicate_start_modifying(filename);
    int retval = !fs::remove(get_fs_path(filename));
    indicate_end_modifying();
    return retval;
}

int NodeInternal::create_directory(const char *dirname) {
    indicate_start_modifying(dirname);
    if (contains(dirname)) {
        indicate_end_modifying();
        return 1;
    }
    
    if (!fs::create_directory(get_fs_path(dirname))) {
        indicate_end_modifying();
        return 1;
    }

    indicate_end_modifying();
    return 0;
}

int NodeInternal::delete_directory(const char *dirname, int delete_contents) {
    indicate_start_modifying(dirname);
    fs::path fs_path = get_fs_path(dirname);

    if (!fs::is_directory(fs_path)) {
        indicate_end_modifying();
        return 1;
    }

    if (delete_contents) {
        std::uintmax_t num_deleted = fs::remove_all(fs_path);
        if (num_deleted == 0 || num_deleted == (std::uintmax_t) -1) {
            indicate_end_modifying();
            return 1;
        }

        indicate_end_modifying();
        return 0;
    }

    if (!fs::is_empty(fs_path)) {
        indicate_end_modifying();
        return 1;
    }

    int retval = !fs::remove(fs_path);
    indicate_end_modifying();
    return retval;
}

int NodeInternal::read_file(const char *filename) {
    if (!contains_file(filename)) {
        return -1;
    }

    char *path_str = get_stored_filename(filename);
    int fd = open(path_str, O_RDONLY);
    free(path_str);

    return fd;
}

NodeInternal::~NodeInternal() {
    free(directory_path);
    free(status_path);
    free(modifying_path);
}



int NodeInternal::check_error(void) {
    int fd_status = open(status_path, O_RDONLY);
    char buf[1];
    ssize_t bytes_read = read(fd_status, buf, 1);

    if (bytes_read == 0) {
        return 3;
    }

    switch (buf[0]) {
        case 'i': {
            return 0;
        }

        case 'm': {
            return 2;
        }

        case 'c': {
            return 4;
        }

        case 't': {
            return 5;
        }

        default: {
            return 1;
        }
    }
}

char *NodeInternal::get_error_info(int error) {
    int fd_modifying;
    ssize_t bytes_read;

    switch (error) {
        case 2:
            fd_modifying = open(modifying_path, O_RDONLY);
            char buf[PATH_MAX];
            bytes_read = read(fd_modifying, buf, PATH_MAX);
            buf[bytes_read] = '\0';
            printf("%s\n", buf);
            return strdup(buf);

        default:
            return NULL;
    }
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

void NodeInternal::indicate_start_modifying(const char *filename) {
    if (cur_modifying == 0) {
        int fd_status = open(status_path, O_WRONLY | O_TRUNC);
        write(fd_status, "t", 1); // transitioning status
        close(fd_status);

        int fd_modifying = open(modifying_path, O_WRONLY | O_TRUNC);
        write(fd_modifying, filename, strlen(filename));
        close(fd_modifying);

        fd_status = open(status_path, O_WRONLY | O_TRUNC);
        write(fd_status, "m", 1); // modifying something
        close(fd_status);
    }

    ++cur_modifying;
}

void NodeInternal::indicate_end_modifying(void) {
    --cur_modifying;

    if (cur_modifying == 0) {
        int fd_status = open(status_path, O_WRONLY | O_TRUNC);
        write(fd_status, "t", 1); // transitioning status
        close(fd_status);

        int fd_modifying = open(modifying_path, O_WRONLY | O_TRUNC);
        close(fd_modifying);

        fd_status = open(status_path, O_WRONLY | O_TRUNC);
        write(fd_status, "i", 1); // idle
        close(fd_status);
    }
}