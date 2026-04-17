// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Get the string version of the type (blob, tree, or commit)
    const char *type_str = object_type_to_string(type);

    // 2. Build the header: "type size\0"
    char header[64];
    int header_len = sprintf(header, "%s %zu", type_str, len) + 1;

    // 3. Compute SHA-256 hash of the FULL object (header + data)
    // We use a helper that hashes two buffers together
    compute_hash_double(header, header_len, data, len, id_out);

    // 4. Check if object already exists (deduplication)
    if (object_exists(id_out)) {
    
        return 0;
    }
    // 5. Create path strings
    char hash_str[65];
    object_id_to_string(id_out, hash_str);

    char dir_path[PATH_MAX];
    char final_path[PATH_MAX];
    
    // Construct the shard directory path (.pes/objects/XX)
    snprintf(dir_path, sizeof(dir_path), ".pes/objects/%.2s", hash_str);
    
    // Construct the full object path (.pes/objects/XX/rest_of_hash)
    snprintf(final_path, sizeof(final_path), "%s/%s", dir_path, hash_str + 2);

    // 6. Create the directories
    mkdir(".pes/objects", 0755); // Ensure root exists
    mkdir(dir_path, 0755);       // Create the shard folder
    // 7. Write to a temporary file (Step 5 in your hints)
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s/tmp_XXXXXX", dir_path);

    int fd = mkstemp(temp_path);
    if (fd < 0) return -1;

    // Write the header first, then the data
    if (write(fd, header, header_len) != header_len) {
        close(fd); unlink(temp_path); return -1;
    }
    if (write(fd, data, len) != (ssize_t)len) {
        close(fd); unlink(temp_path); return -1;
    }

    // 8. Flush to disk and close (Step 6 in your hints)
    fsync(fd);
    close(fd);

    // 9. Rename temp file to final path (Step 7 in your hints)
    if (rename(temp_path, final_path) < 0) {
        unlink(temp_path);
        return -1;
    }

    return 0; 
}
//hashing logic
    // (We will add the writing logic in the next commit)
    


// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, void **data_out, sze_t *size_out, ObjectType *type_out) {
    char hash_str[65];
    object_id_to_string(id, hash_str);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), ".pes/objects/%.2s/%s", hash_str, hash_str + 2);

    // 1. Open and read the whole file
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    fstat(fd, &st);
    
    char *buf = malloc(st.st_size);
    read(fd, buf, st.st_size);
    close(fd);

    // 2. Parse the header: "type size\0data"
    // The type is at the start of the buffer
    *type_out = string_to_object_type(buf);
    
    // Find the null terminator to know where the data starts
    size_t header_len = strlen(buf) + 1;
    *size_out = st.st_size - header_len;

    // 3. Extract the actual data
    *data_out = malloc(*size_out);
    memcpy(*data_out, buf + header_len, *size_out);

    free(buf);
    return 0;
}
