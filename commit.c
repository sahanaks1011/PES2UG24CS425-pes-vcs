// commit.c — Commit creation and history traversal
//
// Commit object format (stored as text, one field per line):
//
//   tree <64-char-hex-hash>
//   parent <64-char-hex-hash>        ← omitted for the first commit
//   author <name> <unix-timestamp>
//   committer <name> <unix-timestamp>
//
//   <commit message>
//
// Note: there is a blank line between the headers and the message.
//
// PROVIDED functions: commit_parse, commit_serialize, commit_walk
// TODO functions:     head_read, head_update, commit_create

#include "commit.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

// Forward declarations (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Parse raw commit data into a Commit struct.
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    (void)len;
    const char *p = (const char *)data;
    char hex[HASH_HEX_SIZE + 1];

    // "tree <hex>\n"
    if (sscanf(p, "tree %64s\n", hex) != 1) return -1;
    if (hex_to_hash(hex, &commit_out->tree) != 0) return -1;
    p = strchr(p, '\n') + 1;

    // optional "parent <hex>\n"
    if (strncmp(p, "parent ", 7) == 0) {
        if (sscanf(p, "parent %64s\n", hex) != 1) return -1;
        if (hex_to_hash(hex, &commit_out->parent) != 0) return -1;
        commit_out->has_parent = 1;
        p = strchr(p, '\n') + 1;
    } else {
        commit_out->has_parent = 0;
    }

    // "author <name> <timestamp>\n"
    char author_buf[256];
    uint64_t ts;
    if (sscanf(p, "author %255[^\n]\n", author_buf) != 1) return -1;
    // split off trailing timestamp
    char *last_space = strrchr(author_buf, ' ');
    if (!last_space) return -1;
    ts = (uint64_t)strtoull(last_space + 1, NULL, 10);
    *last_space = '\0';
    strncpy(commit_out->author, author_buf, sizeof(commit_out->author) - 1);
    commit_out->timestamp = ts;
    p = strchr(p, '\n') + 1;  // skip author line
    p = strchr(p, '\n') + 1;  // skip committer line
    p = strchr(p, '\n') + 1;  // skip blank line

    strncpy(commit_out->message, p, sizeof(commit_out->message) - 1);
    return 0;
}

// Serialize a Commit struct to the text format described at the top of this file.
// Caller must free(*data_out).
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1];
    char parent_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit->tree, tree_hex);

    char buf[8192];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "tree %s\n", tree_hex);
    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
        n += snprintf(buf + n, sizeof(buf) - n, "parent %s\n", parent_hex);
    }
    n += snprintf(buf + n, sizeof(buf) - n,
                  "author %s %" PRIu64 "\n"
                  "committer %s %" PRIu64 "\n"
                  "\n"
                  "%s",
                  commit->author, commit->timestamp,
                  commit->author, commit->timestamp,
                  commit->message);

    *data_out = malloc(n + 1);
    if (!*data_out) return -1;
    memcpy(*data_out, buf, n + 1);
    *len_out = (size_t)n;
    return 0;
}

// Walk commit history from HEAD to the root, calling `callback` per commit.
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1;

    while (1) {
        ObjectType type;
        void *raw;
        size_t raw_len;
        if (object_read(&id, &type, &raw, &raw_len) != 0) return -1;

        Commit c;
        int rc = commit_parse(raw, raw_len, &c);
        free(raw);
        if (rc != 0) return -1;

        callback(&id, &c, ctx);

        if (!c.has_parent) break;
        id = c.parent;
    }
    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Read the current HEAD commit hash.
//
// Steps:
//   1. Read the contents of .pes/HEAD
//   2. If it starts with "ref: " (symbolic reference):
//      a. Extract the ref path (e.g., "refs/heads/main")
//      b. Read .pes/<ref-path> to get the commit hash hex string
//      c. If that file doesn't exist, the branch has no commits → return -1
//   3. Otherwise, HEAD contains a raw commit hash (detached HEAD state)
//   4. Convert the 64-char hex string to an ObjectID using hex_to_hash()
//
// Returns 0 on success, -1 if no commits exist yet.
int head_read(ObjectID *id_out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    // Case 1: symbolic ref (ref: refs/heads/main)
    if (strncmp(buf, "ref: ", 5) == 0) {
        char ref_path[256];
        sscanf(buf + 5, "%s", ref_path);

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", PES_DIR, ref_path);

        FILE *rf = fopen(full_path, "r");
        if (!rf) return -1; // no commits yet

        char hex[HASH_HEX_SIZE + 1];
        if (!fgets(hex, sizeof(hex), rf)) {
            fclose(rf);
            return -1;
        }
        fclose(rf);

        return hex_to_hash(hex, id_out);
    }

    // Case 2: detached HEAD (raw hash)
    return hex_to_hash(buf, id_out);
}

// Update the current branch ref to point to a new commit.
//
// Steps:
//   1. Read .pes/HEAD to determine the current branch
//      (e.g., HEAD contains "ref: refs/heads/main" → update .pes/refs/heads/main)
//   2. Convert the new commit's ObjectID to hex
//   3. Write the hex hash to a temporary file (e.g., .pes/refs/heads/main.tmp)
//   4. fsync() the temp file
//   5. rename() the temp file to the ref file (atomic update)
//   6. fsync() the directory
//
// This is the "pointer swing" — the moment the commit becomes part of history.
//
// Returns 0 on success, -1 on error.
int head_update(const ObjectID *new_commit) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    // Must be symbolic ref
    if (strncmp(buf, "ref: ", 5) != 0) {
        return -1;
    }

    char ref_path[256];
    sscanf(buf + 5, "%s", ref_path);

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", PES_DIR, ref_path);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", full_path);

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    write(fd, hex, strlen(hex));
    write(fd, "\n", 1);
    fsync(fd);
    close(fd);

    if (rename(tmp_path, full_path) != 0) {
        return -1;
    }

    return 0;
}

// Create a new commit from the current staging area.
//
// Steps:
//   1. Build a tree from the index using tree_from_index()
//      (this writes all tree objects and returns the root tree hash)
//   2. Read current HEAD to get the parent commit hash
//      (head_read returns -1 if this is the first commit — that's OK)
//   3. Fill in a Commit struct:
//      - tree = root tree hash from step 1
//      - parent = HEAD commit from step 2 (set has_parent accordingly)
//      - author = pes_author() (from pes.h)
//      - timestamp = current time (use time(NULL))
//      - message = the provided message string
//   4. Serialize the commit using commit_serialize()
//   5. Write to object store using object_write(OBJ_COMMIT, ...)
//   6. Update HEAD to point to the new commit using head_update()
//   7. Store the new commit's hash in *commit_id_out
//
// Returns 0 on success, -1 on error.
int commit_create(const char *message, ObjectID *commit_id_out) {
    // 1. Build tree
    ObjectID tree_id;
    if (tree_from_index(&tree_id) != 0) {
        fprintf(stderr, "error: failed to build tree\n");
        return -1;
    }

    // 2. Read parent
    ObjectID parent_id;
    int has_parent = (head_read(&parent_id) == 0);

    // 3. Fill commit struct
    Commit commit;
    memset(&commit, 0, sizeof(commit));

    commit.tree = tree_id;

    if (has_parent) {
        commit.parent = parent_id;
        commit.has_parent = 1;
    } else {
        commit.has_parent = 0;
    }

    strncpy(commit.author, pes_author(), sizeof(commit.author) - 1);
    commit.timestamp = (uint64_t)time(NULL);
    strncpy(commit.message, message, sizeof(commit.message) - 1);

    // 4. Serialize
    void *data;
    size_t len;

    if (commit_serialize(&commit, &data, &len) != 0) {
        fprintf(stderr, "error: serialize failed\n");
        return -1;
    }

    // 5. Write object
    if (object_write(OBJ_COMMIT, data, len, commit_id_out) != 0) {
        free(data);
        fprintf(stderr, "error: write failed\n");
        return -1;
    }

    free(data);

    // 6. Update HEAD
    if (head_update(commit_id_out) != 0) {
        fprintf(stderr, "error: head update failed\n");
        return -1;
    }

    return 0;
}
