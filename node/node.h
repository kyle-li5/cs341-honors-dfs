/*
 * Interface for nodes
 * 
*/
#include <stdlib.h>

struct node;
typedef struct node node;



/*
 * Returns the total size of the files stored in the node in bytes.
*/
size_t node_size(node *this);

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
int contains_file(node *this, char *filename);

/*
 * Returns the names of the files stored in the node
 *
 * @return
 *     An array of the filenames of the files in the node
*/
char **list_files(node *this);

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
size_t file_size(node *this, char *filename);

/*
 * Stores a new file in the node with the given filename
 * Reads the data from the given stream into the file
 * 
 * Requires that the file is not already stored in the node
 * 
 * Returns whether or not the file creation was successful
 * 
 * @param filename
 *     Name of the file to create
 * 
 * @param input
 *     A stream of data to write to the new file
 * 
 * @return
 *     0 : No errors were encountered
 *     1 : An error occurred
*/
int create_file(node *this, char *filename, FILE* input);

/*
 * Replaces the contents of the stored file with the given filename
 * Reads the data from the given stream into the file
 * 
 * Requires that the file is currently stored in the node
 * 
 * Returns whether or not the write was successful
 * 
 * @param filename
 *     Name of the file to replace
 * 
 * @param input
 *     A stream of data to write to the file
 * 
 * @return
 *     0 : No errors were encountered
 *     1 : An error occurred
*/
int replace_file(node *this, char *filename, FILE* input);

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
int delete_file(node *this, char *filename);