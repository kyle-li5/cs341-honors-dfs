/*
 * Interface for nodes
 * 
*/

#include <stdlib.h>
#include <stdio.h>
#include <filesystem>

class NodeInternal {
    public:
        /*
        * Creates a default NodeInternal object
        * This node will store its information in the ./node-data/default directory
        * This constructor should be used if no other nodes are being used in the current directory
        */
        NodeInternal();

        /*
        * Creates a NodeInternal object
        * This node will store its information in the ./node-data/<node_id> directory
        * This constructor should be used if multiple nodes are being used in the current directory
        *
        * @param node_id
        *     The id of the node being created
        *     This id must be non-negative
        */
        NodeInternal(int node_id);

        /*
        * NodeInternal destructor
        */
        ~NodeInternal();

        /*
        * Returns the total size of the files stored in the node in bytes.
        */
        off_t get_node_size();

        /*
        * Returns whether or not the node is storing a file with the given filename
        *
        * @param filename
        *     Name of the file to look for
        * 
        * @return
        *     0 : File not found
        *     1 : File found
        */
        int contains_file(const char *filename);

        /*
        * Returns the names of the files stored in the node
        *
        * @return
        *     An array of the filenames of the files in the node
        */
        char **list_files();

        /*
        * Gets the size of the file with the given filename in bytes
        * Undefined for files not currently stored in the node
        * IMPORTANT - Does not work for directories right now
        *
        * @param filename
        *     Name of the file
        * 
        * @return
        *     Returns the size of the file in bytes
        */
        off_t get_file_size(const char *filename);

        /*
        * Stores a new file in the node with the given filename
        * Reads the data from the given file descriptor into the file
        * 
        * Requires that the file is not already stored in the node
        * 
        * Returns whether or not the file creation was successful
        * 
        * @param filename
        *     Name of the file to create
        * 
        * @param input
        *     A file descriptor for the data to write to the new file
        * 
        * @return
        *     0 : No errors were encountered
        *     1 : An error occurred
        */
        int create_file(const char *filename, int input);

        /*
        * Replaces the contents of the stored file with the given filename
        * Reads the data from the given file descriptor into the file
        * 
        * Requires that the file is currently stored in the node
        * 
        * Returns whether or not the write was successful
        * 
        * @param filename
        *     Name of the file to replace
        * 
        * @param input
        *     A file descriptor for the data to write to the specified file
        * 
        * @return
        *     0 : No errors were encountered
        *     1 : An error occurred
        */
        int replace_file(const char *filename, int input);

        /*
        * Deletes the file with the given filename from the node
        * 
        * Fails if the file is not found
        * 
        * @param filename
        *     Name of the file to delete
        * 
        * @return
        *     0 : No errors were encountered
        *     1 : An error occurred
        */
        int delete_file(const char *filename);

        /*
        * Returns a file descriptor for reading the contents of the file with the given filename
        * 
        * Requires that the file is currently stored in the node
        * The behavior of the descriptor is undefined if the given file is manipulated after read_file is called
        * 
        * @param filename
        *     Name of the file to read from
        * 
        * @return
        *     A file descriptor associated with the specified file
        *     -1 is returned if an error is encountered
        */
        int read_file(const char *filename);

    private:
        char *get_stored_filename(const char *filename);

        std::filesystem::path get_fs_path(const char *filepath);

    private:
        int node_id;
        char *directory_path;
        size_t storage_skip_amt;

};