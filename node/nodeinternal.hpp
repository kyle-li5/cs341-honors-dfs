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
        * Removes any stored files or metadata
        * 
        * After calling this, the NodeInternal object will behave as though no node had previously been created in its location with the same node id.
        */
        void clear_existing_data(void);

        /*
        * Returns the total size of the files stored in the node in bytes.
        */
        off_t get_node_size();

        /*
        * !!! Use contains instead. (contains_file now just calls contains.)
        *
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
        * Returns whether or not the node is storing a file or directory with the given filename
        *
        * @param path
        *     Name of the path to check
        * 
        * @return
        *     0 : Item not found
        *     1 : Item found
        */
        int contains(const char *path);

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
        * Creates a new directory in the node with the given filename
        * 
        * Returns whether or not the directory creation was successful
        * 
        * Will return 1 if the directory already exists
        * 
        * @param dirname
        *     Name of the directory to create
        * 
        * @return
        *     0 : No errors were encountered
        *     1 : An error occurred
        */
        int create_directory(const char *dirname);

        /*
        * Deletes the directory with the given name from the node
        * 
        * Fails if the directory is not found
        * 
        * @param dirname
        *     Name of the directory to delete
        * 
        * @param delete_contents
        *     Whether or not to delete all contents of the directory
        *     If this is 0, delete_directory will fail if the directory is not empty
        *     0 : False
        *     1 : True
        * 
        * @return
        *     0 : No errors were encountered
        *     1 : An error occurred
        */
        int delete_directory(const char *dirname, int delete_contents);

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

        /*
        * Checks if there is an error in with the node
        *
        * @return
        *       A nonzero value corresponding to the error detected, or zero if no error is detected
        *       0 : No Error
        *       1 : Unknown Error
        *       2 : An old node operating in the same location was interrupted while manipulating a file. (The data corresponding to this error will be cleared if a file is manipulated, so this should be checked and dealt with immediately after a node has been created.)
        *       3 : No status information is present. (This will occur if the node has just been created, no files have been manipulated, and there was not preexisting data for the node to find.)
        */
       int check_error(void);

       /*
       * Returns additional information corresponding to the error number passed in
       *
       * This behavior is undefined if the error number is not one listed below, or if the passed error number is not a current error.
       * 
       * @return
       *        Information corresponding to the passed error number
       *        2 : The path (for the node's functions) of the file whose manipulation was interrupted
       */
       char *get_error_info(int error);

    private:
        char *get_stored_filename(const char *filename);

        std::filesystem::path get_fs_path(const char *filepath);

        void indicate_start_modifying(const char *filename);
        void indicate_end_modifying(void);

    private:
        int node_id;
        char *directory_path;
        size_t storage_skip_amt;

        // Stores whether a file was being manipulated
        char *status_path;

        // Stores what file was being manipulated
        char *modifying_path;

        // Quick check for whether a file is being manipulated
        int cur_modifying;

};