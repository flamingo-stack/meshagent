/*
OpenFrame File Logger - Duplicates printf to both console and file
Usage: Call enable_file_logging() at the start of main()

Features:
- Single log file: meshagent.log
- Auto-rotation at 10MB
- Keeps only 1 archive (meshagent.log.old.gz)
*/

#ifndef OPENFRAME_FILE_LOGGER_H
#define OPENFRAME_FILE_LOGGER_H

/* Feature test macros for POSIX functions */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#if defined(_POSIX) || defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <zlib.h>
#endif

#ifdef WIN32
#include <Windows.h>
#include <process.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

/* Macro to ignore return values */
#ifndef ignore_result
#define ignore_result(x) do { if(x) {} } while(0)
#endif

/* Constants */
#define LOG_BUFFER_SIZE 4096
#define LOG_PATH_SIZE 512
#define LOG_MAX_SIZE (10 * 1024 * 1024)  /* 10 MB */
#define LOG_FILENAME "meshcentral-agent.log"
#define LOG_ARCHIVE_FILENAME "meshcentral-agent.log.old.gz"

/* Global file handle for logging */
static FILE* g_log_file = NULL;
static char g_log_directory[LOG_PATH_SIZE] = {0};
static long g_current_log_size = 0;

#ifdef WIN32
static int g_original_stdout_fd = -1;
static HANDLE g_tee_thread = INVALID_HANDLE_VALUE;
static int g_pipe_fds[2] = {-1, -1};
static volatile int g_logging_active = 0;
static int g_daemon_mode = 0;  /* Flag to detect daemon mode */

/* Thread function to duplicate output to both console and file */
static DWORD WINAPI tee_thread_func(LPVOID lpParam) {
    char buffer[LOG_BUFFER_SIZE];
    int bytes_read;

    (void)lpParam; /* Unused parameter */

    while (g_logging_active && (bytes_read = _read(g_pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        /* Write to original stdout (console) - but skip if daemon mode */
        if (!g_daemon_mode && g_original_stdout_fd >= 0) {
            int result = _write(g_original_stdout_fd, buffer, bytes_read);
            if (result < 0) {
                /* If write fails, we're probably in daemon mode */
                g_daemon_mode = 1;
            }
        }

        /* Write to log file */
        if (g_log_file != NULL) {
            fwrite(buffer, 1, bytes_read, g_log_file);
            fflush(g_log_file);
        }
    }

    return 0;
}
#endif

#if defined(_POSIX) || defined(__APPLE__) || defined(__linux__) || defined(__unix__)
static int g_original_stdout_fd = -1;
static pthread_t g_tee_thread;
static int g_pipe_fds[2] = {-1, -1};
static volatile int g_logging_active = 0;
static int g_daemon_mode = 0;  /* Flag to detect daemon mode */

/* Thread function to duplicate output to both console and file */
static void* tee_thread_func(void* arg) {
    char buffer[LOG_BUFFER_SIZE];
    ssize_t bytes_read;

    (void)arg; /* Unused parameter */

    while (g_logging_active && (bytes_read = read(g_pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        /* Write to original stdout (console) - but skip if daemon mode */
        if (!g_daemon_mode && g_original_stdout_fd >= 0) {
            ssize_t result = write(g_original_stdout_fd, buffer, bytes_read);
            if (result < 0) {
                /* If write fails, we're probably in daemon mode */
                g_daemon_mode = 1;
            }
        }

        /* Write to log file */
        if (g_log_file != NULL) {
            fwrite(buffer, 1, bytes_read, g_log_file);
            fflush(g_log_file);
        }
    }

    return NULL;
}
#endif

/* Thread-safe mutex for logging */
#ifdef WIN32
static CRITICAL_SECTION g_log_mutex;
static int g_log_mutex_initialized = 0;
#else
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static volatile int g_disk_error_count = 0;
static volatile int g_disk_available = 1;

static inline void init_log_mutex(void) {
#ifdef WIN32
    if (!g_log_mutex_initialized) {
        InitializeCriticalSection(&g_log_mutex);
        g_log_mutex_initialized = 1;
    }
#endif
}

static inline void lock_log(void) {
#ifdef WIN32
    EnterCriticalSection(&g_log_mutex);
#else
    pthread_mutex_lock(&g_log_mutex);
#endif
}

static inline void unlock_log(void) {
#ifdef WIN32
    LeaveCriticalSection(&g_log_mutex);
#else
    pthread_mutex_unlock(&g_log_mutex);
#endif
}

static inline long get_file_size(const char* filepath) {
    struct stat st;
    if (stat(filepath, &st) == 0) {
        return (long)st.st_size;
    }
    return 0;
}

static inline int compress_file_to_gzip(const char* source_path, const char* dest_path) {
#ifdef WIN32
    FILE* src = fopen(source_path, "rb");
    FILE* dst = fopen(dest_path, "wb");
    char buffer[8192];
    size_t bytes;

    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return 0;
    }

    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
            fclose(src);
            fclose(dst);
            return 0;
        }
    }

    fclose(src);
    fclose(dst);
    return 1;
#else
    FILE* src = fopen(source_path, "rb");
    gzFile dst = gzopen(dest_path, "wb9");
    char buffer[8192];
    size_t bytes;

    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) gzclose(dst);
        return 0;
    }

    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (gzwrite(dst, buffer, (unsigned int)bytes) != (int)bytes) {
            fclose(src);
            gzclose(dst);
            return 0;
        }
    }

    fclose(src);
    gzclose(dst);
    return 1;
#endif
}

static inline int rotate_log_file(void) {
    char log_path[LOG_PATH_SIZE];
    char archive_path[LOG_PATH_SIZE];
    time_t now;
    struct tm* tm_info;
    int pid;

    if (g_log_file == NULL) return 0;

    /* Build file paths */
    if (g_log_directory[0] != '\0') {
        snprintf(log_path, sizeof(log_path), "%s/%s", g_log_directory, LOG_FILENAME);
        snprintf(archive_path, sizeof(archive_path), "%s/%s", g_log_directory, LOG_ARCHIVE_FILENAME);
    } else {
        snprintf(log_path, sizeof(log_path), "%s", LOG_FILENAME);
        snprintf(archive_path, sizeof(archive_path), "%s", LOG_ARCHIVE_FILENAME);
    }

    fprintf(g_log_file, "\n========================================\n");
    fprintf(g_log_file, "LOG ROTATION - File size limit reached\n");
    fprintf(g_log_file, "========================================\n");
    fflush(g_log_file);
    fclose(g_log_file);
    g_log_file = NULL;

    remove(archive_path);
    compress_file_to_gzip(log_path, archive_path);
    remove(log_path);

    g_log_file = fopen(log_path, "a");
    if (g_log_file == NULL) {
        return 0;
    }
    setvbuf(g_log_file, NULL, _IONBF, 0);
    g_current_log_size = 0;

    now = time(NULL);
    tm_info = localtime(&now);
#ifdef WIN32
    pid = _getpid();
#else
    pid = getpid();
#endif

    fprintf(g_log_file, "========================================\n");
    fprintf(g_log_file, "MeshAgent Log Started (after rotation)\n");
    fprintf(g_log_file, "Time: %04d-%02d-%02d %02d:%02d:%02d\n",
            tm_info->tm_year + 1900,
            tm_info->tm_mon + 1,
            tm_info->tm_mday,
            tm_info->tm_hour,
            tm_info->tm_min,
            tm_info->tm_sec);
    fprintf(g_log_file, "PID: %d\n", pid);
    fprintf(g_log_file, "Log file: %s\n", log_path);
    fprintf(g_log_file, "Previous log archived to: %s\n", archive_path);
    fprintf(g_log_file, "========================================\n\n");
    fflush(g_log_file);

    return 1;
}

static inline void check_and_rotate(int bytes_written) {
    g_current_log_size += bytes_written;

    if (g_current_log_size >= LOG_MAX_SIZE) {
        rotate_log_file();
    }
}

/*
 * Enable file logging - duplicates stdout and stderr to file AND console
 * All existing printf() calls will write to BOTH destinations
 *
 * Returns: 1 on success, 0 on failure
 */
static inline int enable_file_logging(const char* log_directory, const char* log_prefix)
{
    char logfile_path[LOG_PATH_SIZE];
    int pid;

    (void)log_prefix;

    if (g_log_file != NULL) {
        fprintf(stderr, "WARNING: File logging is already enabled\n");
        return 1;
    }

#ifdef WIN32
    pid = _getpid();
#else
    pid = getpid();
#endif

    if (log_directory != NULL && strlen(log_directory) > 0) {
        strncpy(g_log_directory, log_directory, sizeof(g_log_directory) - 1);
        g_log_directory[sizeof(g_log_directory) - 1] = '\0';
        snprintf(logfile_path, sizeof(logfile_path), "%s/%s", log_directory, LOG_FILENAME);
    } else {
        g_log_directory[0] = '\0';
        snprintf(logfile_path, sizeof(logfile_path), "%s", LOG_FILENAME);
    }

    g_log_file = fopen(logfile_path, "a");
    if (g_log_file == NULL) {
        fprintf(stderr, "WARNING: Failed to open log file: %s\n", logfile_path);
        return 0;
    }
    setvbuf(g_log_file, NULL, _IONBF, 0);
    g_current_log_size = get_file_size(logfile_path);

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);

    fprintf(g_log_file, "========================================\n");
    fprintf(g_log_file, "MeshAgent Log Started\n");
    fprintf(g_log_file, "Time: %04d-%02d-%02d %02d:%02d:%02d\n",
            tm_info->tm_year + 1900,
            tm_info->tm_mon + 1,
            tm_info->tm_mday,
            tm_info->tm_hour,
            tm_info->tm_min,
            tm_info->tm_sec);
    fprintf(g_log_file, "PID: %d\n", pid);
    fprintf(g_log_file, "Log file: %s\n", logfile_path);
    fprintf(g_log_file, "Max size before rotation: %d MB\n", LOG_MAX_SIZE / (1024 * 1024));
    fprintf(g_log_file, "========================================\n\n");
    fflush(g_log_file);

#ifdef WIN32
    /* Create pipe for tee functionality */
    if (_pipe(g_pipe_fds, LOG_BUFFER_SIZE, _O_BINARY) != 0) {
        fprintf(stderr, "WARNING: Failed to create pipe for logging\n");
        fclose(g_log_file);
        g_log_file = NULL;
        return 0;
    }

    /* Save original stdout and check if we're running as service */
    g_original_stdout_fd = _dup(1); /* 1 is stdout */
    if (g_original_stdout_fd < 0) {
        fprintf(g_log_file, "WARNING: Failed to duplicate stdout - running as service\n");
        fflush(g_log_file);
        g_daemon_mode = 1;
    } else {
        /* Check if stdout is redirected (common in service mode) */
        if (!_isatty(g_original_stdout_fd)) {
            /* stdout is not a console - likely service mode */
            g_daemon_mode = 1;
            fprintf(g_log_file, "INFO: Detected service mode - console output disabled\n");
            fflush(g_log_file);
        } else {
            fprintf(g_log_file, "INFO: Console mode detected - output will go to both console and file\n");
            fflush(g_log_file);
        }
    }

    /* Start tee thread */
    g_logging_active = 1;
    g_tee_thread = CreateThread(NULL, 0, tee_thread_func, NULL, 0, NULL);
    if (g_tee_thread == NULL) {
        fprintf(g_log_file, "WARNING: Failed to create logging thread\n");
        fflush(g_log_file);
        if (g_original_stdout_fd >= 0) _close(g_original_stdout_fd);
        _close(g_pipe_fds[0]);
        _close(g_pipe_fds[1]);
        fclose(g_log_file);
        g_log_file = NULL;
        g_logging_active = 0;
        return 0;
    }

    /* Redirect stdout to pipe */
    if (_dup2(g_pipe_fds[1], 1) < 0) { /* 1 is stdout */
        fprintf(g_log_file, "WARNING: Failed to redirect stdout\n");
        fflush(g_log_file);
        g_logging_active = 0;
        _close(g_pipe_fds[1]); /* Close write end to signal thread to exit */
        WaitForSingleObject(g_tee_thread, 1000);
        CloseHandle(g_tee_thread);
        if (g_original_stdout_fd >= 0) _close(g_original_stdout_fd);
        _close(g_pipe_fds[0]);
        fclose(g_log_file);
        g_log_file = NULL;
        return 0;
    }

    if (_dup2(g_pipe_fds[1], 2) < 0) { /* 2 is stderr */
        fprintf(g_log_file, "WARNING: Failed to redirect stderr\n");
        fflush(g_log_file);
        if (g_original_stdout_fd >= 0) _dup2(g_original_stdout_fd, 1);
        g_logging_active = 0;
        _close(g_pipe_fds[1]); /* Close write end to signal thread to exit */
        WaitForSingleObject(g_tee_thread, 1000);
        CloseHandle(g_tee_thread);
        if (g_original_stdout_fd >= 0) _close(g_original_stdout_fd);
        _close(g_pipe_fds[0]);
        fclose(g_log_file);
        g_log_file = NULL;
        return 0;
    }

    _close(g_pipe_fds[1]);

    /* Disable buffering */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

#if defined(_POSIX) || defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    /* Create pipe for tee functionality */
    if (pipe(g_pipe_fds) != 0) {
        fprintf(stderr, "WARNING: Failed to create pipe for logging\n");
        fclose(g_log_file);
        g_log_file = NULL;
        return 0;
    }

    /* Save original stdout and check if we're running as daemon */
    g_original_stdout_fd = dup(STDOUT_FILENO);
    if (g_original_stdout_fd < 0) {
        fprintf(g_log_file, "WARNING: Failed to duplicate stdout - running in daemon mode\n");
        fflush(g_log_file);
        g_daemon_mode = 1;
    } else {
        /* Check if stdout is redirected (common in daemon mode) */
        struct stat stat_buf;
        if (fstat(g_original_stdout_fd, &stat_buf) == 0) {
            if (!isatty(g_original_stdout_fd)) {
                /* stdout is not a terminal - likely daemon mode */
                g_daemon_mode = 1;
                fprintf(g_log_file, "INFO: Detected daemon mode - console output disabled\n");
                fflush(g_log_file);
            } else {
                fprintf(g_log_file, "INFO: Console mode detected - output will go to both console and file\n");
                fflush(g_log_file);
            }
        }
    }

    /* Start tee thread */
    g_logging_active = 1;
    if (pthread_create(&g_tee_thread, NULL, tee_thread_func, NULL) != 0) {
        fprintf(g_log_file, "WARNING: Failed to create logging thread\n");
        fflush(g_log_file);
        if (g_original_stdout_fd >= 0) close(g_original_stdout_fd);
        close(g_pipe_fds[0]);
        close(g_pipe_fds[1]);
        fclose(g_log_file);
        g_log_file = NULL;
        g_logging_active = 0;
        return 0;
    }

    /* Redirect stdout to pipe */
    dup2(g_pipe_fds[1], STDOUT_FILENO);
    dup2(g_pipe_fds[1], STDERR_FILENO);
    close(g_pipe_fds[1]);

    /* Disable buffering */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

    fprintf(stderr, "File logging enabled: %s\n", logfile_path);

    /* Force immediate flush to ensure message appears */
    fflush(stdout);
    fflush(stderr);

    return 1;
}

/*
 * Simpler version - auto-generates filename in current directory
 */
static inline int enable_file_logging_simple(void)
{
    return enable_file_logging(NULL, NULL);
}

static volatile int g_at_line_start = 1;

static inline int openframe_printf_timestamp(char *buf, int bufsize)
{
#ifdef WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    return snprintf(buf, bufsize, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ INFO ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    struct tm tm_info;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm_info);
    return snprintf(buf, bufsize, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ INFO ",
        tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
        ts.tv_nsec / 1000000);
#endif
}

static inline int openframe_printf(const char *format, ...)
{
    va_list args;
    char raw[4096];
    char buffer[8192];
    int raw_len;
    int len = 0;
    int success = 0;

    va_start(args, format);
    raw_len = vsnprintf(raw, sizeof(raw), format, args);
    va_end(args);

    if (raw_len <= 0) return raw_len;
    if (raw_len >= (int)sizeof(raw)) raw_len = (int)sizeof(raw) - 1;

    /* Prepend timestamp at line boundaries */
    for (int i = 0; i < raw_len && len < (int)sizeof(buffer) - 40; i++) {
        if (g_at_line_start && raw[i] != '\n') {
            len += openframe_printf_timestamp(buffer + len, (int)sizeof(buffer) - len);
            g_at_line_start = 0;
        }
        buffer[len++] = raw[i];
        if (raw[i] == '\n') {
            g_at_line_start = 1;
        }
    }

    init_log_mutex();
    lock_log();

    /* Try to write to log file with error handling */
    if (g_log_file != NULL && g_disk_available) {
        size_t written = fwrite(buffer, 1, len, g_log_file);

        if (written == (size_t)len) {
            /* Successful write - try to flush */
            if (fflush(g_log_file) == 0) {
                success = 1;
                g_disk_error_count = 0; /* Reset error counter */

                /* Check for rotation */
                check_and_rotate(len);
            } else {
                /* Flush failed - disk might be full */
                g_disk_error_count++;
                if (g_disk_error_count >= 5) {
                    g_disk_available = 0; /* Disable disk writes temporarily */
                }
            }
        } else {
            /* Write failed - handle gracefully */
            g_disk_error_count++;
            if (g_disk_error_count >= 3) {
                g_disk_available = 0;

                if (g_log_file != NULL) {
                    fprintf(g_log_file, "\n*** LOG ERROR: Disk write failures detected, switching to stdout-only mode ***\n");
                    fflush(g_log_file);
                }
            }
        }
    }

    unlock_log();

    /* ALWAYS write to stdout - this is our backup */
#ifdef WIN32
    DWORD written_stdout;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, len, &written_stdout, NULL);
#else
    ignore_result(write(STDOUT_FILENO, buffer, len));
#endif

    /* Periodically try to re-enable disk writes */
    if (!g_disk_available && (g_disk_error_count % 50 == 0)) {
        lock_log();
        g_disk_available = 1; /* Try again */
        unlock_log();
    }

    return len;
}

/*
 * Override printf with reliable version when file logging is active
 * This ensures printf works correctly in both console and daemon modes
 */
#define OPENFRAME_PRINTF_OVERRIDE 1
#if OPENFRAME_PRINTF_OVERRIDE
#define printf(...) openframe_printf(__VA_ARGS__)
#endif

/*
 * Disable file logging and clean up resources
 */
static inline void disable_file_logging(void)
{
    if (g_log_file == NULL) {
        return; /* Logging not active */
    }

#ifdef WIN32
    if (g_logging_active) {
        g_logging_active = 0;

        /* Restore original stdout/stderr */
        if (g_original_stdout_fd >= 0) {
            _dup2(g_original_stdout_fd, 1);
            _dup2(g_original_stdout_fd, 2);
            _close(g_original_stdout_fd);
            g_original_stdout_fd = -1;
        }

        /* Wait for thread to finish and clean up */
        if (g_tee_thread != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(g_tee_thread, 1000); /* Wait up to 1 second */
            CloseHandle(g_tee_thread);
            g_tee_thread = INVALID_HANDLE_VALUE;
        }

        /* Close pipe */
        if (g_pipe_fds[0] >= 0) {
            _close(g_pipe_fds[0]);
            g_pipe_fds[0] = -1;
        }
        g_pipe_fds[1] = -1;
    }
#endif

#if defined(_POSIX) || defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    if (g_logging_active) {
        g_logging_active = 0;

        /* Restore original stdout/stderr */
        if (g_original_stdout_fd >= 0) {
            dup2(g_original_stdout_fd, STDOUT_FILENO);
            dup2(g_original_stdout_fd, STDERR_FILENO);
            close(g_original_stdout_fd);
            g_original_stdout_fd = -1;
        }

        /* Wait for thread to finish and clean up */
        pthread_join(g_tee_thread, NULL);

        /* Close pipe */
        if (g_pipe_fds[0] >= 0) {
            close(g_pipe_fds[0]);
            g_pipe_fds[0] = -1;
        }
        g_pipe_fds[1] = -1;
    }
#endif

    /* Close log file */
    if (g_log_file != NULL) {
        fprintf(g_log_file, "\n========================================\n");
        fprintf(g_log_file, "MeshAgent Log Ended\n");
        fprintf(g_log_file, "========================================\n");
        fclose(g_log_file);
        g_log_file = NULL;
    }

    /* Reset state */
    g_daemon_mode = 0;
    g_disk_available = 1;
    g_disk_error_count = 0;
    g_current_log_size = 0;
    g_log_directory[0] = '\0';

#ifdef WIN32
    if (g_log_mutex_initialized) {
        DeleteCriticalSection(&g_log_mutex);
        g_log_mutex_initialized = 0;
    }
#endif

    fprintf(stderr, "File logging disabled.\n");
}

#endif // OPENFRAME_FILE_LOGGER_H
