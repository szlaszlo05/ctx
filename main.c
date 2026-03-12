/**
 * @file main.c
 * @brief Context Generator (CTX) - A utility to aggregate source code for AI prompting.
 * @version 1.4
 * @date 2025-10-22
 * * This tool recursively scans directories, filters by extension, ignores specified
 * folders, and produces chunked text files suitable for LLM context windows.
 * -i flag for ignoring hidden files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdarg.h>
#include <windows.h>

/* --- Configuration Constants --- */
#define CTX_MAX_EXTENSIONS 128
#define CTX_MAX_EXT_LEN 16
#define CTX_MAX_IGNORED_DIRS 128
#define CTX_MAX_IGNORED_LEN 64
#define CTX_MAX_PATH_LEN 2048
#define CTX_BUFFER_SIZE 4096
#define CTX_MAX_FILE_SIZE (100 * 1024 * 1024) // 100 MB

/* --- Data Structures --- */

/**
 * @brief Application State Context
 * Encapsulates all runtime configuration and state to avoid global variables.
 */
typedef struct
{
    // Configuration Lists
    char allowed_extensions[CTX_MAX_EXTENSIONS][CTX_MAX_EXT_LEN];
    int ext_count;

    char ignored_dirs[CTX_MAX_IGNORED_DIRS][CTX_MAX_IGNORED_LEN];
    int ignored_count;

    // Runtime Flags
    bool ignore_hidden;

    // File Handles & Counters
    FILE *log_file;
    FILE *output_file;
    int output_index;
    long long current_output_size;
    int total_files_processed;

    // Paths
    char exe_path[CTX_MAX_PATH_LEN];
} AppContext;

/* --- Function Prototypes --- */
static void ctx_log(AppContext *ctx, const char *level, const char *fmt, ...);
static void get_executable_path(char *buffer, size_t size);
static void trim_whitespace(char *str);
static void load_config_extensions(AppContext *ctx, const char *path);
static void load_config_ignore(AppContext *ctx, const char *path);
static void ensure_output_file(AppContext *ctx, long long bytes_needed);
static bool is_valid_extension(AppContext *ctx, const char *filename);
static bool is_ignored_directory(AppContext *ctx, const char *dirname);
static void scan_directory(AppContext *ctx, const char *dir_path);

/* --- Implementation --- */

/**
 * @brief Centralized logging function.
 * Handles writing to stdout and the log file simultaneously.
 */
static void ctx_log(AppContext *ctx, const char *level, const char *fmt, ...)
{
    va_list args;

    // Write to Log File
    if (ctx->log_file)
    {
        fprintf(ctx->log_file, "[%s] ", level);
        va_start(args, fmt);
        vfprintf(ctx->log_file, fmt, args);
        va_end(args);
        fprintf(ctx->log_file, "\n");
        fflush(ctx->log_file);
    }

    // Write to Console
    if (strcmp(level, "DEBUG") != 0)
    {
        // Optional: Add color codes here if desired
        printf("[%s] ", level);
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
    }
}

/**
 * @brief A function to get rid of white spaces in a string.
 */
static void trim_whitespace(char *str)
{
    str[strcspn(str, "\r\n")] = 0;
}

/**
 * @brief A function to get the home directory of the tool exe.
 * It is used to look for the config and log files, since the
 * tool will usually not be run from its home directory.
 */
static void get_executable_path(char *buffer, size_t size)
{
    if (GetModuleFileName(NULL, buffer, (DWORD)size) == 0)
    {
        buffer[0] = '\0';
        return;
    }

    char *last_slash = strrchr(buffer, '\\');
    if (last_slash)
    {
        *(last_slash + 1) = '\0';
    }
}

/**
 * @brief Loads allowed extensions from config, or sets safe defaults.
 */
static void load_config_extensions(AppContext *ctx, const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file)
    {
        ctx_log(ctx, "WARN", "Extensions config not found at '%s'. Using defaults.", path);

        const char *defaults[] = {
            ".c", ".cpp", ".h", ".hpp", ".cs", ".java", ".py",
            ".js", ".ts", ".json", ".xml", ".sql", ".txt", ".md", ".go", ".rs"};

        ctx->ext_count = 0;
        for (int i = 0; i < (int)(sizeof(defaults) / sizeof(defaults[0])); i++)
        {
            strncpy(ctx->allowed_extensions[i], defaults[i], CTX_MAX_EXT_LEN - 1);
            ctx->ext_count++;
        }
        return;
    }

    char line[CTX_MAX_EXT_LEN];
    while (fgets(line, sizeof(line), file) && ctx->ext_count < CTX_MAX_EXTENSIONS)
    {
        trim_whitespace(line);
        if (strlen(line) > 0)
        {
            strncpy(ctx->allowed_extensions[ctx->ext_count++], line, CTX_MAX_EXT_LEN - 1);
        }
    }
    fclose(file);
    ctx_log(ctx, "INFO", "Loaded %d allowed extensions.", ctx->ext_count);
}

/**
 * @brief Loads ignored directories from config, or sets safe defaults.
 */
static void load_config_ignore(AppContext *ctx, const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file)
    {
        ctx_log(ctx, "WARN", "Ignore config not found at '%s'. Using defaults.", path);

        const char *defaults[] = {
            ".git", ".vs", ".vscode", ".idea", "node_modules", "bin", "obj", "build", "dist", "__pycache__"};

        ctx->ignored_count = 0;
        for (int i = 0; i < (int)(sizeof(defaults) / sizeof(defaults[0])); i++)
        {
            strncpy(ctx->ignored_dirs[i], defaults[i], CTX_MAX_IGNORED_LEN - 1);
            ctx->ignored_count++;
        }
        return;
    }

    char line[CTX_MAX_IGNORED_LEN];
    while (fgets(line, sizeof(line), file) && ctx->ignored_count < CTX_MAX_IGNORED_DIRS)
    {
        trim_whitespace(line);
        if (strlen(line) > 0)
        {
            strncpy(ctx->ignored_dirs[ctx->ignored_count++], line, CTX_MAX_IGNORED_LEN - 1);
        }
    }
    fclose(file);
    ctx_log(ctx, "INFO", "Loaded %d ignored directories.", ctx->ignored_count);
}

/**
 * @brief File validator function.
 * Checks if a file should be ignored or not.
 */
static bool is_valid_extension(AppContext *ctx, const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot)
        return false;

    for (int i = 0; i < ctx->ext_count; i++)
    {
        // Case-insensitive comparison
        if (_stricmp(dot, ctx->allowed_extensions[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Directory validator function.
 * Checks if a directory should be ignored or not.
 */
static bool is_ignored_directory(AppContext *ctx, const char *dirname)
{
    for (int i = 0; i < ctx->ignored_count; i++)
    {
        if (_stricmp(dirname, ctx->ignored_dirs[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Output file handler function.
 * It ensures the context file doesn't exceed the maximum filezize.
 */
static void ensure_output_file(AppContext *ctx, long long bytes_needed)
{
    if (ctx->output_file == NULL || (ctx->current_output_size + bytes_needed) > CTX_MAX_FILE_SIZE)
    {
        if (ctx->output_file)
        {
            fclose(ctx->output_file);
            ctx_log(ctx, "INFO", "File limit reached. Closed output_%d.txt", ctx->output_index);
            ctx->output_index++;
        }

        char filename[64];
        snprintf(filename, sizeof(filename), "output_%d.txt", ctx->output_index);

        ctx->output_file = fopen(filename, "wb");
        if (!ctx->output_file)
        {
            ctx_log(ctx, "ERROR", "Failed to create output file: %s", filename);
            exit(EXIT_FAILURE); // Critical failure
        }

        ctx->current_output_size = 0;
        ctx_log(ctx, "INFO", "Created new output file: %s", filename);
    }
}

/**
 * @brief Driver function.
 * Scans the directory the user wants to be contextualized.
 */
static void scan_directory(AppContext *ctx, const char *dir_path)
{
    ctx_log(ctx, "DEBUG", "Scanning directory: %s", dir_path);

    DIR *d = opendir(dir_path);
    if (!d)
    {
        ctx_log(ctx, "ERROR", "Could not open directory: %s", dir_path);
        return;
    }

    struct dirent *entry;
    struct stat path_stat;
    char full_path[CTX_MAX_PATH_LEN];

    while ((entry = readdir(d)) != NULL)
    {
        // Skip system dots
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Skip output files (so it doesn't reference its own output)
        if (strncmp(entry->d_name, "output_", 7) == 0)
            continue;

        // Skip ignored directories
        if (is_ignored_directory(ctx, entry->d_name))
        {
            ctx_log(ctx, "DEBUG", "Skipping ignored directory: %s", entry->d_name);
            continue;
        }

        // Build full path safely
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        if (stat(full_path, &path_stat) == 0)
        {
            if (S_ISDIR(path_stat.st_mode))
            {
                scan_directory(ctx, full_path); // Recurse
            }
            else if (S_ISREG(path_stat.st_mode))
            {
                // Handle -i flag (ignore hidden files)
                if (ctx->ignore_hidden && entry->d_name[0] == '.')
                    continue;

                if (is_valid_extension(ctx, entry->d_name))
                {
                    // Prepare Header
                    char header[CTX_MAX_PATH_LEN + 128];
                    int header_len = snprintf(header, sizeof(header), "\n\n//-----[ %s ]-----\n\n", full_path);

                    long long total_needed = header_len + path_stat.st_size;
                    ensure_output_file(ctx, total_needed);

                    if (ctx->output_file)
                    {
                        // Write Header
                        fwrite(header, 1, header_len, ctx->output_file);
                        ctx->current_output_size += header_len;

                        // Write Content
                        FILE *src = fopen(full_path, "rb");
                        if (src)
                        {
                            char buffer[CTX_BUFFER_SIZE];
                            size_t bytes_read;
                            while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0)
                            {
                                fwrite(buffer, 1, bytes_read, ctx->output_file);
                            }
                            fclose(src);

                            ctx->current_output_size += path_stat.st_size;
                            ctx->total_files_processed++;
                            ctx_log(ctx, "INFO", "Processed: %s", full_path);
                        }
                        else
                        {
                            ctx_log(ctx, "ERROR", "Could not read file: %s", full_path);
                        }
                    }
                }
            }
        }
    }
    closedir(d);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: ctx.exe [directory] [-i (ignore hidden)]\n");
        return EXIT_FAILURE;
    }

    // Initialize Context (0)
    AppContext ctx = {0};
    ctx.output_index = 1;

    // Parse Arguments
    const char *target_dir = ".";
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-i") == 0)
        {
            ctx.ignore_hidden = true;
        }
        else if (argv[i][0] != '-')
        {
            target_dir = argv[i];
        }
    }

    // Setup Paths & Logging
    get_executable_path(ctx.exe_path, sizeof(ctx.exe_path));

    char log_path[CTX_MAX_PATH_LEN];
    snprintf(log_path, sizeof(log_path), "%slog.LOG", ctx.exe_path);
    ctx.log_file = fopen(log_path, "w");

    ctx_log(&ctx, "INFO", "CTX Started. Target: %s", target_dir);

    // Load Configurations
    char config_path[CTX_MAX_PATH_LEN];

    snprintf(config_path, sizeof(config_path), "%sextensions.cfg", ctx.exe_path);
    load_config_extensions(&ctx, config_path);

    snprintf(config_path, sizeof(config_path), "%signore.cfg", ctx.exe_path);
    load_config_ignore(&ctx, config_path);

    // Run Execution
    scan_directory(&ctx, target_dir);

    // Cleanup
    if (ctx.output_file)
        fclose(ctx.output_file);
    if (ctx.log_file)
        fclose(ctx.log_file);

    printf("\n[SUCCESS] Generated context from %d files.\n", ctx.total_files_processed);

    return EXIT_SUCCESS;
}