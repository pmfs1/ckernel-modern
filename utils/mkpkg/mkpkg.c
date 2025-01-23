// This utility creates package files for the krlean operating system.
// It reads package manifests, bundles files into tar archives, and maintains a package database.

// Required header files for file operations, directory handling, and system calls
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <utime.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include "inifile.h"

// TAR file block size (512 bytes - standard tar format)
#define STRLEN 1024
#define TAR_BLKSIZ 512
#define MAX_PATH_LEN 4096

// TAR header structure following the POSIX ustar format
struct tarhdr
{
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

// Package structure representing a single software package
struct pkg
{
    char *name;               // Package name
    char *description;        // Package description
    char *inffile;            // Path to package info file
    struct section *manifest; // Package manifest data
    int removed;              // Flag indicating if package is marked for removal
    int time;                 // Package timestamp
    struct pkg *next;         // Linked list pointer to next package
};

// Package database structure maintaining a linked list of packages
struct pkgdb
{
    struct pkg *head; // First package in the list
    struct pkg *tail; // Last package in the list
    char *repo;       // Repository path
    int dirty;        // Flag indicating if database needs saving
};

// Function prototypes
int add_file(FILE *archive, char *srcfn, char *dstfn, int *time, int prebuilt);
int add_file_internal(FILE *archive, char *srcfn, char *dstfn, int *time, int prebuilt);

// Global directory paths
char *srcdir; // Source directory containing package files
char *dstdir; // Destination directory for package output

// Adds a new package to the database
static struct pkg *add_package(struct pkgdb *db, char *name)
{
    struct pkg *pkg = calloc(1, sizeof(struct pkg));
    pkg->name = strdup(name);
    if (!db->head)
        db->head = pkg;
    if (db->tail)
        db->tail->next = pkg;
    db->tail = pkg;
    db->dirty = 1;
    return pkg;
}

// Searches for a package by name in the database
static struct pkg *find_package(struct pkgdb *db, char *name)
{
    struct pkg *pkg;

    for (pkg = db->head; pkg; pkg = pkg->next)
    {
        if (strcmp(pkg->name, name) == 0)
            return pkg;
    }
    return NULL;
}

// Reads package database from file
static int read_pkgdb(char *dbfile, struct pkgdb *db)
{
    FILE *f;
    char line[STRLEN];

    f = fopen(dbfile, "r");
    if (!f)
        return 0;
    while (fgets(line, STRLEN, f))
    {
        char *p = line;
        char *name = NULL;
        char *inffile = NULL;
        char *description = NULL;
        char *time = NULL;
        name = strsep(&p, "\t\n");
        inffile = strsep(&p, "\t\n");
        description = strsep(&p, "\t\n");
        time = strsep(&p, "\t\n");

        if (name)
        {
            struct pkg *pkg = add_package(db, name);
            if (inffile)
                pkg->inffile = strdup(inffile);
            if (description)
                pkg->description = strdup(description);
            if (time)
                pkg->time = atoi(time);
        }
    }
    fclose(f);
    db->dirty = 0;
    return 0;
}

// Writes package database to file
static int write_pkgdb(char *dbfile, struct pkgdb *db)
{
    FILE *f;
    struct pkg *pkg;
    int fd;

    fd = open(dbfile, O_WRONLY | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR);
    if (fd < 0)
    {
        perror(dbfile);
        return 1;
    }
    f = fdopen(fd, "w");
    if (!f)
    {
        close(fd);
        perror(dbfile);
        return 1;
    }
    for (pkg = db->head; pkg; pkg = pkg->next)
    {
        if (!pkg->removed)
        {
            fprintf(f, "%s\t%s\t%s\t%d\n",
                    pkg->name,
                    pkg->inffile ? pkg->inffile : "",
                    pkg->description ? pkg->description : "",
                    pkg->time);
        }
    }
    fclose(f);
    db->dirty = 0;

    return 0;
}

// Checks for directory traversal attempts in paths (e.g., "../")
static int is_path_traversal(const char *path)
{
    const char *p = path;
    const char *component = path;

    while (*p)
    {
        if (*p == '/')
        {
            size_t len = p - component;
            if (len == 2 && component[0] == '.' && component[1] == '.')
            {
                return 1; // Found path traversal attempt
            }
            component = p + 1;
        }
        p++;
    }

    // Check last component
    if (strcmp(component, "..") == 0)
    {
        return 1;
    }

    return 0;
}

// Validates filename characters and checks for path traversal
int is_valid_filename(const char *filename)
{
    if (!filename || !*filename)
        return 0;

    // Check for basic invalid characters
    for (const char *p = filename; *p; p++)
    {
        if (*p < 32 || *p == '\\' || *p > 126)
            return 0;
    }

    // Check for path traversal attempts
    return !is_path_traversal(filename);
}

// Normalizes file paths by removing redundant separators
static char *canonicalize_path(const char *path)
{
    char *canon = malloc(MAX_PATH_LEN);
    if (!canon)
        return NULL;

    char *dst = canon;
    const char *src = path;
    size_t len = 0;

    while (*src && len < MAX_PATH_LEN - 1)
    {
        if (*src == '/' && *(src + 1) == '/')
            src++;
        else
            *dst++ = *src++, len++;
    }
    *dst = '\0';
    return canon;
}

// Comprehensive path validation including length and character checks
static int is_valid_path(const char *path)
{
    if (!path || !*path)
        return 0;
    if (strlen(path) >= MAX_PATH_LEN)
        return 0;
    if (strstr(path, ".."))
        return 0;
    if (strstr(path, "//"))
        return 0;

    // Check for absolute paths
    if (path[0] == '/')
        return 0;

    // Basic character validation
    for (const char *p = path; *p; p++)
    {
        if (*p < 32 || *p == '\\' || *p == ':' || *p > 126)
            return 0;
    }
    return 1;
}

// Enhanced path validation including absolute path and special character checks
static int is_path_safe(const char *path) {
    if (!path || !*path || !is_valid_path(path)) {
        return 0;
    }

    // No absolute paths
    if (path[0] == '/') {
        return 0;
    }

    // No drive letters (Windows)
    if (isalpha(path[0]) && path[1] == ':') {
        return 0;
    }

    // Check each component
    const char *p = path;
    const char *start = path;
    size_t component_len = 0;

    while (*p) {
        if (*p == '/') {
            // Check component length
            component_len = p - start;
            if (component_len == 0) {
                return 0;  // Empty component
            }
            if (component_len > 255) {
                return 0;  // Component too long
            }
            
            // Validate component
            char component[256];
            strncpy(component, start, component_len);
            component[component_len] = '\0';
            
            if (strcmp(component, ".") == 0 || 
                strcmp(component, "..") == 0) {
                return 0;
            }
            
            start = p + 1;
        }
        p++;
    }

    // Check final component
    component_len = p - start;
    if (component_len > 0) {
        char component[256];
        strncpy(component, start, component_len);
        component[component_len] = '\0';
        
        if (strcmp(component, ".") == 0 || 
            strcmp(component, "..") == 0) {
            return 0;
        }
    }

    return 1;
}

// Safely joins directory and filename paths
char *joinpath(char *dir, char *filename, char *path)
{
    if (strlen(dir) + strlen(filename) + 2 > MAX_PATH_LEN)
    {
        path[0] = '\0';
        return path;
    }

    char *canon_filename = canonicalize_path(filename);
    if (!canon_filename)
    {
        path[0] = '\0';
        return path;
    }

    int dirlen = strlen(dir);
    while (dirlen > 0 && dir[dirlen - 1] == '/')
        dirlen--;
    while (*filename == '/')
        filename++;

    // Check the filename for path traversal before joining
    if (!is_valid_filename(filename))
    {
        path[0] = '\0'; // Return empty string on invalid input
        return path;
    }

    memcpy(path, dir, dirlen);
    path[dirlen] = '/';
    strcpy(path + dirlen + 1, filename);

    free(canon_filename);
    return path;
}

// Internal function to add files and directories to the package archive
// Parameters:
//   archive: Target TAR archive file pointer
//   srcfn: Source file/directory path
//   dstfn: Destination path within the archive
//   time: Pointer to timestamp for tracking newest file
//   prebuilt: Flag indicating if file is prebuilt (uses provided timestamp)
int add_file_internal(FILE *archive, char *srcfn, char *dstfn, int *time, int prebuilt)
{
    struct stat st;
    int fd;
    // Open source file and get its status
    if ((fd = open(srcfn, O_RDONLY)) < 0)
    {
        perror(srcfn);
        return 1;
    }
    if (fstat(fd, &st) < 0)
    {
        perror(srcfn);
        close(fd);
        return 1;
    }
    // Handle directory case
    if (S_ISDIR(st.st_mode))
    {
        close(fd); // Close directory file descriptor
        struct dirent *dp;
        DIR *dirp;
        char subsrcfn[STRLEN];
        char subdstfn[STRLEN];
        int rc;
        // Open and process directory contents
        dirp = opendir(srcfn);
        if (!dirp)
        {
            perror(srcfn);
            return 1;
        }
        // Recursively process directory entries
        while ((dp = readdir(dirp)))
        {
            // Skip . and .. entries
            if (strcmp(dp->d_name, ".") == 0)
                continue;
            if (strcmp(dp->d_name, "..") == 0)
                continue;
            // Create paths for subdirectory/file
            joinpath(srcfn, dp->d_name, subsrcfn);
            joinpath(dstfn, dp->d_name, subdstfn);
            // Recursive call to process entry
            rc = add_file(archive, subsrcfn, subdstfn, time, prebuilt);
            if (rc != 0)
            {
                closedir(dirp);
                return 1;
            }
        }
        closedir(dirp);
    }
    // Handle regular file case
    else
    {
        unsigned int chksum;
        int n;
        FILE *f;
        unsigned char blk[TAR_BLKSIZ];
        struct tarhdr *hdr = (struct tarhdr *)blk;
        // Initialize TAR header block
        memset(blk, 0, sizeof(blk));
        // Remove leading slashes from destination path
        while (*dstfn == '/')
            dstfn++;
        // Fill TAR header fields
        strcpy(hdr->name, dstfn);                                     // File path in archive
        snprintf(hdr->mode, sizeof(hdr->mode), "%07o", st.st_mode);   // File permissions
        snprintf(hdr->uid, sizeof(hdr->uid), "%07o", 0);              // User ID (set to 0)
        snprintf(hdr->gid, sizeof(hdr->gid), "%07o", 0);              // Group ID (set to 0)
        sprintf(hdr->size, "%011o", st.st_size);                      // File size in octal
        sprintf(hdr->mtime, "%011o", prebuilt ? *time : st.st_mtime); // Modification time
        memcpy(hdr->chksum, "        ", 8);                           // Initialize checksum field
        hdr->typeflag = '0';                                          // Regular file type
        strncpy(hdr->magic, "ustar  ", sizeof(hdr->magic) - 1);       // ustar format identifier
        hdr->magic[sizeof(hdr->magic) - 1] = '\0';
        strcpy(hdr->uname, "krlean"); // Owner username
        strcpy(hdr->gname, "krlean"); // Owner group name
        // Calculate header checksum (sum of all bytes in header)
        chksum = 0;
        for (n = 0; n < TAR_BLKSIZ; n++)
            chksum += blk[n];
        snprintf(hdr->chksum, sizeof(hdr->chksum), "%06o", chksum);
        // Write TAR header block
        if (fwrite(blk, 1, TAR_BLKSIZ, archive) != TAR_BLKSIZ)
        {
            perror("write");
            return 1;
        }
        // Open source file for reading content
        f = fdopen(fd, "r");
        if (!f)
        {
            perror(srcfn);
            close(fd);
            return 1;
        }
        // Copy file content in TAR_BLKSIZ chunks
        while ((n = fread(blk, 1, TAR_BLKSIZ, f)) > 0)
        {
            // Pad last block with zeros if needed
            if (n < TAR_BLKSIZ)
                memset(blk + n, 0, TAR_BLKSIZ - n);
            // Write content block
            if (fwrite(blk, 1, TAR_BLKSIZ, archive) != TAR_BLKSIZ)
            {
                perror("write");
                fclose(f);
                return 1;
            }
        }
        fclose(f);
        // Update package timestamp if not a prebuilt file
        if (!prebuilt)
        {
            if (*time == 0 || st.st_mtime > *time)
                *time = st.st_mtime;
        }
    }
    return 0;
}

// Wrapper function for adding files with path safety checks
int add_file(FILE *archive, char *srcfn, char *dstfn, int *time, int prebuilt)
{
    char *canon_src = canonicalize_path(srcfn);
    char *canon_dst = canonicalize_path(dstfn);
    if (!canon_src || !canon_dst)
    {
        free(canon_src);
        free(canon_dst);
        fprintf(stderr, "Path canonicalization failed\n");
        return 1;
    }

    if (!is_valid_path(canon_src) || !is_valid_path(canon_dst))
    {
        free(canon_src);
        free(canon_dst);
        fprintf(stderr, "Invalid path detected\n");
        return 1;
    }

    // Continue with canonicalized paths
    int ret = add_file_internal(archive, canon_src, canon_dst, time, prebuilt);

    free(canon_src);
    free(canon_dst);
    return ret;
}

// Main package creation function
int make_package(struct pkgdb *db, char *inffn)
{
    // Strict path validation
    if (!is_path_safe(inffn)) {
        fprintf(stderr, "Invalid or unsafe input file path\n");
        return 1;
    }

    struct section *manifest;
    struct section *source;
    struct section *prebuilt;
    struct pkg *pkg;
    char *pkgname;
    char *description;
    char dstinffn[STRLEN];
    char dstpkgfn[STRLEN];
    char srcfn[STRLEN];
    unsigned char zero[TAR_BLKSIZ];
    struct utimbuf times;
    FILE *archive;
    int rc;

    manifest = read_properties(inffn);
    if (!manifest)
    {
        fprintf(stderr, "Error reading manifest from %s\n", inffn);
        return 1;
    }

    pkgname = get_property(manifest, "package", "name", NULL);
    description = get_property(manifest, "package", "description", NULL);

    strcpy(dstinffn, "/usr/share/pkg/");
    strncat(dstinffn, pkgname, STRLEN - strlen(dstinffn) - 1);
    strncat(dstinffn, ".inf", STRLEN - strlen(dstinffn) - 1);

    pkg = find_package(db, pkgname);
    if (!pkg)
        pkg = add_package(db, pkgname);
    free(pkg->description);
    free(pkg->inffile);
    pkg->description = description ? strdup(description) : NULL;
    pkg->manifest = manifest;
    pkg->inffile = strdup(dstinffn);
    pkg->time = 0;
    db->dirty = 1;

    if (strcmp(dstdir, "-") == 0)
        return 0;

    joinpath(dstdir, pkgname, dstpkgfn);
    strcat(dstpkgfn, ".pkg");

    int fd = open(dstpkgfn, O_WRONLY | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd < 0)
    {
        // handle error
        return 1;
    }
    archive = fdopen(fd, "w");
    if (archive == NULL)
    {
        close(fd);
        // handle error
        return 1;
    }
    if (add_file(archive, inffn, pkg->inffile, &pkg->time, 0) != 0)
    {
        fclose(archive);
        unlink(dstpkgfn);
        return 1;
    }

    source = find_section(pkg->manifest, "source");
    if (source)
    {
        struct property *p;
        for (p = source->properties; p; p = p->next)
        {
            if (!is_path_safe(p->name)) {
                fprintf(stderr, "Invalid or unsafe source path in manifest: %s\n", p->name);
                fclose(archive);
                unlink(dstpkgfn);
                return 1;
            }
            
            char *safe_path = canonicalize_path(p->name);
            if (!safe_path) {
                fprintf(stderr, "Path canonicalization failed\n");
                fclose(archive);
                unlink(dstpkgfn);
                return 1;
            }
            
            joinpath(srcdir, safe_path, srcfn);
            rc = add_file(archive, srcfn, safe_path, &pkg->time, 0);
            free(safe_path);
            
            if (rc != 0) {
                fclose(archive);
                unlink(dstpkgfn);
                return 1;
            }
        }
    }

    prebuilt = find_section(pkg->manifest, "prebuilt");
    if (prebuilt)
    {
        struct property *p;
        for (p = prebuilt->properties; p; p = p->next)
        {
            if (!is_path_safe(p->name)) {
                fprintf(stderr, "Invalid or unsafe prebuilt path in manifest: %s\n", p->name);
                fclose(archive);
                unlink(dstpkgfn);
                return 1;
            }
            
            char *safe_path = canonicalize_path(p->name);
            if (!safe_path) {
                fprintf(stderr, "Path canonicalization failed\n");
                fclose(archive);
                unlink(dstpkgfn);
                return 1;
            }
            
            joinpath(srcdir, safe_path, srcfn);
            rc = add_file(archive, srcfn, safe_path, &pkg->time, 1);
            free(safe_path);
            
            if (rc != 0) {
                fclose(archive);
                unlink(dstpkgfn);
                return 1;
            }
        }
    }

    memset(zero, 0, TAR_BLKSIZ);
    fwrite(zero, 1, TAR_BLKSIZ, archive);
    fwrite(zero, 1, TAR_BLKSIZ, archive);
    fclose(archive);

    times.actime = times.modtime = pkg->time;
    utime(dstpkgfn, &times);

    return 0;
}

// Main function: Processes command line arguments and creates packages
int main(int argc, char *argv[])
{
    int i;
    char dbfile[STRLEN];
    struct pkgdb db;

    if (argc < 3)
    {
        fprintf(stderr, "usage: mkpkg SRCDIR DSTDIR INFFILE...\n");
        return 1;
    }

    memset(&db, 0, sizeof(struct pkgdb));
    srcdir = argv[1];
    dstdir = argv[2];

    // Validate input paths with stricter checks
    if (!is_path_safe(argv[1]) || 
        (strcmp(argv[2], "-") != 0 && !is_path_safe(argv[2]))) {
        fprintf(stderr, "Invalid or unsafe source/destination directory\n");
        return 1;
    }

    if (strcmp(dstdir, "-") == 0)
    {
        strcpy(dbfile, "db");
    }
    else
    {
        joinpath(dstdir, "db", dbfile);
    }
    read_pkgdb(dbfile, &db);

    for (i = 3; i < argc; i++)
    {
        if (!is_path_safe(argv[i])) {
            fprintf(stderr, "Invalid or unsafe input file path: %s\n", argv[i]);
            return 1;
        }
        if (make_package(&db, argv[i]) != 0)
            return 1;
    }
    write_pkgdb(dbfile, &db);

    return 0;
}
