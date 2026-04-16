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
// PROVIDED functions: commit_parse, commit_serialize, commit_walk, head_read, head_update
// TODO functions:     commit_create

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
    memset(commit_out, 0, sizeof(Commit));

    const char *ptr = (const char *)data;
    const char *end = ptr + len;

    char line[512];

    while (ptr < end) {
        int i = 0;

        // read one line
        while (ptr < end && *ptr != '\n' && i < sizeof(line) - 1) {
            line[i++] = *ptr++;
        }
        line[i] = '\0';

        ptr++; // skip '\n'

        // blank line → next is message
        if (i == 0) {
            size_t msg_len = end - ptr;
            if (msg_len >= sizeof(commit_out->message))
                msg_len = sizeof(commit_out->message) - 1;

            memcpy(commit_out->message, ptr, msg_len);
            commit_out->message[msg_len] = '\0';
            break;
        }

        if (strncmp(line, "tree ", 5) == 0) {
            hex_to_hash(line + 5, &commit_out->tree);
        }
        else if (strncmp(line, "parent ", 7) == 0) {
            hex_to_hash(line + 7, &commit_out->parent);
            commit_out->has_parent = 1;
        }
        else if (strncmp(line, "author ", 7) == 0) {
            snprintf(commit_out->author, sizeof(commit_out->author), "%s", line + 7);
        }
        else if (strncmp(line, "time ", 5) == 0) {
            commit_out->timestamp = strtoull(line + 5, NULL, 10);
        }
    }

    return 0;
}
// Serialize a Commit struct to the text format.
// Caller must free(*data_out).
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char *buffer = malloc(1024);
    if (!buffer) return -1;

    char tree_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit->tree, tree_hex);

    char parent_hex[HASH_HEX_SIZE + 1];

    int offset = 0;

    // tree
    offset += sprintf(buffer + offset, "tree %s\n", tree_hex);

    // parent
    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
        offset += sprintf(buffer + offset, "parent %s\n", parent_hex);
    }

    // author
    offset += sprintf(buffer + offset, "author %s\n", commit->author);

    // 🔥 THIS LINE WAS MISSING OR WRONG
    offset += sprintf(buffer + offset, "time %llu\n",
                      (unsigned long long)commit->timestamp);

    // message
    offset += sprintf(buffer + offset, "\n%s\n", commit->message);

    *data_out = buffer;
    *len_out = offset;
    return 0;
}
// Walk commit history from HEAD to the root.
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

// Read the current HEAD commit hash.
int head_read(ObjectID *id_out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0'; // strip newline

    char ref_path[512];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(ref_path, sizeof(ref_path), "%s/%s", PES_DIR, line + 5);
        f = fopen(ref_path, "r");
        if (!f) return -1; // Branch exists but has no commits yet
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
        fclose(f);
        line[strcspn(line, "\r\n")] = '\0';
    }
    return hex_to_hash(line, id_out);
}

// Update the current branch ref to point to a new commit atomically.
int head_update(const ObjectID *new_commit) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';

    char target_path[520];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(target_path, sizeof(target_path), "%s/%s", PES_DIR, line + 5);
    } else {
        snprintf(target_path, sizeof(target_path), "%s", HEAD_FILE); // Detached HEAD
    }

    char tmp_path[528];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target_path);
    
    f = fopen(tmp_path, "w");
    if (!f) return -1;
    
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);
    fprintf(f, "%s\n", hex);
    
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    return rename(tmp_path, target_path);
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Create a new commit from the current staging area.
//
// HINTS - Useful functions to call:
//   - tree_from_index   : writes the directory tree and gets the root hash
//   - head_read         : gets the parent commit hash (if any)
//   - pes_author        : retrieves the author name string (from pes.h)
//   - time(NULL)        : gets the current unix timestamp
//   - commit_serialize  : converts the filled Commit struct to a text buffer
//   - object_write      : saves the serialized text as OBJ_COMMIT
//   - head_update       : moves the branch pointer to your new commit
//
// Returns 0 on success, -1 on error.

int commit_create(const char *message, ObjectID *commit_id_out) {
    ObjectID tree_id;

    // Step 1: Build tree from index
    if (tree_from_index(&tree_id) != 0) return -1;

    // Step 2: Read parent from HEAD
    ObjectID parent_id;
    int has_parent = (head_read(&parent_id) == 0);

    // Step 3: Get author
    const char *author = pes_author();

    // Step 4: Prepare commit struct
    Commit commit;
    memset(&commit, 0, sizeof(Commit));

    commit.tree = tree_id;

    if (has_parent) {
        commit.parent = parent_id;
        commit.has_parent = 1;
    } else {
        commit.has_parent = 0;
    }

    // SAFE copy
    snprintf(commit.author, sizeof(commit.author), "%s", author);
    snprintf(commit.message, sizeof(commit.message), "%s", message);

    // IMPORTANT: correct timestamp
    commit.timestamp = (uint64_t)time(NULL);

    // Step 5: Serialize
    void *data;
    size_t len;
    if (commit_serialize(&commit, &data, &len) != 0) return -1;

    // Step 6: Write object
    if (object_write(OBJ_COMMIT, data, len, commit_id_out) != 0) {
        free(data);
        return -1;
    }

    free(data);

    // Step 7: Update HEAD
    if (head_update(commit_id_out) != 0) return -1;

    return 0;
}
