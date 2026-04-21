// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename),
// not on binary format gymnastics.
//
// PROVIDED functions: index_find, index_remove
// TODO functions:     index_load, index_save, index_add, index_status

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// Steps:
//   1. If .pes/index does not exist, set index->count = 0 and return 0
//   2. Open the file for reading (fopen with "r")
//   3. For each line, parse the fields:
//      - Use fscanf or sscanf to read: mode, hex-hash, mtime, size, path
//      - Convert the 64-char hex hash to an ObjectID using hex_to_hash()
//   4. Populate index->entries and index->count
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(".pes/index", "r");
    if (!f) {
        // File doesn't exist → empty index
        return 0;
    }

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];

        char hex[65];

        int ret = fscanf(f, "%o %64s %lu %u %511[^\n]\n",
                         &e->mode,
                         hex,
                         &e->mtime_sec,
                         &e->size,
                         e->path);

        if (ret == EOF) break;
        if (ret != 5) {
            fclose(f);
            return -1;
        }

        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
//
// Steps:
//   1. Sort entries by path (use qsort with strcmp on the path field)
//   2. Open a temporary file for writing (.pes/index.tmp)
//   3. For each entry, write one line:
//      "<mode> <64-char-hex-hash> <mtime_sec> <size> <path>\n"
//      - Convert ObjectID to hex using hash_to_hex()
//   4. fflush() and fsync() the temp file to ensure data reaches disk
//   5. fclose() the temp file
//   6. rename(".pes/index.tmp", ".pes/index") — atomic replacement
//
// The rename() call is the key filesystem concept here: it is atomic
// on POSIX systems, meaning the index file is never in a half-written
// state even if the system crashes.
//
// Returns 0 on success, -1 on error.
int cmp_index_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

int index_save(const Index *index) {
    // 1. Make a copy so we can sort
    Index temp = *index;

    qsort(temp.entries, temp.count, sizeof(IndexEntry), cmp_index_entries);

    // 2. Open temp file
    FILE *f = fopen(".pes/index.tmp", "w");
    if (!f) return -1;

    // 3. Write entries
    for (int i = 0; i < temp.count; i++) {
        char hex[65];
        hash_to_hex(&temp.entries[i].hash, hex);

        fprintf(f, "%o %s %lu %u %s\n",
                temp.entries[i].mode,
                hex,
                temp.entries[i].mtime_sec,
                temp.entries[i].size,
                temp.entries[i].path);
    }

    // 4. Flush + sync
    fflush(f);
    fsync(fileno(f));

    // 5. Close
    fclose(f);

    // 6. Atomic rename
    if (rename(".pes/index.tmp", ".pes/index") != 0) {
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
//
// Steps:
//   1. Open and read the file at `path`
//   2. Write the file contents as a blob: object_write(OBJ_BLOB, ...)
//   3. Stat the file to get mode, mtime, and size
//      - Use stat() or lstat()
//      - mtime_sec = st.st_mtime
//      - size = st.st_size
//      - mode: use 0100755 if executable (st_mode & S_IXUSR), else 0100644
//   4. Search the index for an existing entry with this path (index_find)
//      - If found: update its hash, mode, mtime, and size
//      - If not found: append a new entry (check count < MAX_INDEX_ENTRIES)
//   5. Save the index to disk (index_save)
//
// Returns 0 on success, -1 on error (file not found, etc.).
int index_add(Index *index, const char *path) {
    // 1. Open file
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        return -1;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    // Read file content
    void *buf = malloc(size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 2. Write blob
    ObjectID oid;
    if (object_write(OBJ_BLOB, buf, size, &oid) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    // 3. Stat file
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }

    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    // 4. Find existing entry
    IndexEntry *e = index_find(index, path);

    if (e) {
        // Update existing
        e->hash = oid;
        e->mode = mode;
        e->mtime_sec = st.st_mtime;
        e->size = st.st_size;
    } else {
        // Add new
        if (index->count >= MAX_INDEX_ENTRIES) {
            return -1;
        }

        e = &index->entries[index->count++];
        e->hash = oid;
        e->mode = mode;
        e->mtime_sec = st.st_mtime;
        e->size = st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    // 5. Save index
    return index_save(index);
}

// Print the status of the working directory.
//
// This involves THREE comparisons:
//
// 1. Index vs HEAD (staged changes):
//    - Load the HEAD commit's tree (if any commits exist)
//    - For each index entry, check if it exists in HEAD's tree with the same hash
//    - New in index but not in HEAD:       "new file:   <path>"
//    - In both but different hash:          "modified:   <path>"
//
// 2. Working directory vs index (unstaged changes):
//    - For each index entry, check the working directory file
//    - If file is missing:                  "deleted:    <path>"
//    - If file's mtime or size changed, recompute its hash:
//      - If hash differs from index:        "modified:   <path>"
//    - (If mtime+size unchanged, skip — assume file is unmodified)
//
// 3. Untracked files:
//    - Scan the working directory (skip .pes/)
//    - Any file not in the index:           "<path>"
//
// Expected output:
//   Staged changes:
//       new file:   hello.txt
//
//   Unstaged changes:
//       modified:   README.md
//
//   Untracked files:
//       notes.txt
//
// If a section has no entries, print the header followed by
//   (nothing to show)
//
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");

    if (index->count == 0) {
        printf("  (nothing to show)\n");
    } else {
        for (int i = 0; i < index->count; i++) {
            printf("  new file:   %s\n", index->entries[i].path);
        }
    }

    printf("\nUnstaged changes:\n");

    int unstaged = 0;

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = &index->entries[i];

        struct stat st;
        if (stat(e->path, &st) != 0) {
            printf("  deleted:    %s\n", e->path);
            unstaged = 1;
            continue;
        }

        // Quick check using mtime + size
        if (st.st_mtime != e->mtime_sec || st.st_size != e->size) {
            // Recompute hash
            FILE *f = fopen(e->path, "rb");
            if (!f) continue;

            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            rewind(f);

            void *buf = malloc(size);
            if (!buf) {
                fclose(f);
                continue;
            }

            fread(buf, 1, size, f);
            fclose(f);

            ObjectID oid;
            object_write(OBJ_BLOB, buf, size, &oid);
            free(buf);

            if (memcmp(oid.hash, e->hash.hash, HASH_SIZE) != 0) {
                printf("  modified:   %s\n", e->path);
                unstaged = 1;
            }
        }
    }

    if (!unstaged && index->count > 0) {
        printf("  (nothing to show)\n");
    }

    printf("\nUntracked files:\n");

    DIR *d = opendir(".");
    if (!d) return 0;

    struct dirent *ent;
    int untracked = 0;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0 ||
            strcmp(ent->d_name, ".pes") == 0) {
            continue;
        }

        int found = 0;
        for (int i = 0; i < index->count; i++) {
            if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            printf("  %s\n", ent->d_name);
            untracked = 1;
        }
    }

    closedir(d);

    if (!untracked) {
        printf("  (nothing to show)\n");
    }

    return 0;
}
