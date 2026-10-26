#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#define APP_NAME "recode_all_music_in_folder"
#define APP_ID "recode_all_music_in_folder"
#define APP_VERSION "0.2.0"
#define HASH_CHUNK (256 * 1024)
#define ONE_LINER_LIMIT 12000
#define GREEN "\033[32m"
#define DIMGREEN "\033[2;32m"
#define CYAN "\033[36m"
#define RESET "\033[0m"
#define PROMPT_BACK "\001RAMIF_PROMPT_BACK\001"

static const char *AUDIO_EXTS[] = {
    ".mp3", ".flac", ".wav", ".ogg", ".oga", ".opus", ".m4a",
    ".aac", ".wma", ".aiff", ".aif", ".ape", ".wv", ".mpc",
    ".tta", ".dsf", ".dff", ".m4b", ".amr", ".3ga", ".caf",
    ".au", ".ra", ".rm", ".mpga", ".mp2", ".mid", ".midi"
};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

typedef struct {
    char *target;
    char *output;
    char *format;
    char *run_uid;
    bool recursive;
    bool recursive_set;
    bool no_duplicates;
    bool no_clipboard;
    bool manual_input;
    bool ask_lyrics;
    bool ask_artist;
    bool ask_album;
    bool ask_title;
    bool ask_image;
    bool resume;
    bool reenter_metadata;
    bool flush;
    char *verify_script;
    bool format_set;
    bool no_duplicates_set;
    bool windows_batch;
} Options;

typedef struct {
    char *home;
    char *data_dir;
    char *db_path;
    char *ini_path;
    char *gpg_home;
    char *public_key_path;
    char *binary_path;
    char *ffmpeg_path;
    char *gpg_path;
    char *fingerprint;
    char *username;
    char *hostname;
    char *system_name;
    char *system_release;
} App;

static sqlite3 *g_db = NULL;
static bool g_color = false;
static volatile sig_atomic_t g_interrupted = 0;

static void on_signal(int sig) { (void)sig; g_interrupted = 1; }

static void log_info(const char *fmt, ...) {
    va_list ap;
    if (g_color) fputs(GREEN "[RAMIF] " RESET, stdout); else fputs("[RAMIF] ", stdout);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}
static void log_warn(const char *fmt, ...) {
    va_list ap;
    if (g_color) fputs("\033[33m[WARN] " RESET, stderr); else fputs("[WARN] ", stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
static void log_error(const char *fmt, ...) {
    va_list ap;
    if (g_color) fputs("\033[31m[ERROR] " RESET, stderr); else fputs("[ERROR] ", stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { log_error("Out of memory."); exit(2); }
    return p;
}
static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }
static char *str_printf(const char *fmt, ...) {
    va_list ap, cp; va_start(ap, fmt); va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp); va_end(cp);
    if (n < 0) { va_end(ap); return NULL; }
    char *s = xmalloc((size_t)n + 1); vsnprintf(s, (size_t)n + 1, fmt, ap); va_end(ap); return s;
}
static void sb_init(StrBuf *b) { b->data = xmalloc(1); b->data[0] = '\0'; b->len = 0; b->cap = 1; }
static void sb_reserve(StrBuf *b, size_t add) {
    if (add > SIZE_MAX - b->len - 1) { log_error("String too large."); exit(2); }
    size_t need = b->len + add + 1;
    if (need <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) { if (nc > SIZE_MAX / 2) { nc = need; break; } nc *= 2; }
    b->data = realloc(b->data, nc); if (!b->data) { log_error("Out of memory."); exit(2); } b->cap = nc;
}
static void sb_addn(StrBuf *b, const char *s, size_t n) { sb_reserve(b, n); memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = '\0'; }
static void sb_add(StrBuf *b, const char *s) { if (s) sb_addn(b, s, strlen(s)); }
static void sb_addf(StrBuf *b, const char *fmt, ...) {
    va_list ap, cp; va_start(ap, fmt); va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp); va_end(cp); if (n < 0) { va_end(ap); return; }
    sb_reserve(b, (size_t)n); vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap); va_end(ap); b->len += (size_t)n;
}
static void sb_free(StrBuf *b) { free(b->data); memset(b, 0, sizeof(*b)); }

static void fd_write_all(int fd, const void *data, size_t len) {
    const char *p=(const char*)data;
    while (len) {
        ssize_t n=write(fd,p,len);
        if (n<0) { if (errno==EINTR) continue; break; }
        if (n==0) break;
        p+=(size_t)n; len-=(size_t)n;
    }
}

static void memory_trim(void) {
    if (g_db) (void)sqlite3_db_release_memory(g_db);
#ifdef __GLIBC__
    (void)malloc_trim(0);
#endif
}

static char *path_join(const char *a, const char *b) {
    if (!a || !*a) return xstrdup(b ? b : "");
    if (!b || !*b) return xstrdup(a);
    if (b[0] == '/') return xstrdup(b);
    size_t na = strlen(a), nb = strlen(b);
    char *p = xmalloc(na + nb + 2);
    snprintf(p, na + nb + 2, "%s%s%s", a, (na && a[na-1] == '/') ? "" : "/", b);
    return p;
}
static char *path_basename_dup(const char *p) {
    if (!p) return xstrdup("");
    const char *slash = strrchr(p, '/'); return xstrdup(slash ? slash + 1 : p);
}
static char *path_dirname_dup(const char *p) {
    if (!p || !*p) return xstrdup(".");
    char *c = xstrdup(p); char *d = dirname(c); char *out = xstrdup(d); free(c); return out;
}
static bool path_has_linebreak(const char *s) { return s && (strchr(s, '\n') || strchr(s, '\r')); }
static bool is_prefix_path(const char *parent, const char *child) {
    size_t n = strlen(parent);
    if (strncmp(parent, child, n) != 0) return false;
    if (n == 1 && parent[0] == '/') return true;
    return child[n] == '\0' || child[n] == '/';
}
static char *expand_tilde(const char *input) {
    if (!input) return xstrdup("");
    if (input[0] == '~' && (input[1] == '/' || input[1] == '\0')) {
        const char *home = getenv("HOME"); if (!home) home = "";
        return path_join(home, input[1] == '/' ? input + 2 : "");
    }
    return xstrdup(input);
}
static char *absolute_existing(const char *input) {
    char *expanded = expand_tilde(input);
    char *r = realpath(expanded, NULL); free(expanded); return r;
}
static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char *tmp = xstrdup(path); size_t n = strlen(tmp);
    if (n > 1 && tmp[n-1] == '/') tmp[n-1] = '\0';
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; if (mkdir(tmp, mode) != 0 && errno != EEXIST) { free(tmp); return -1; } *p = '/'; }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) { free(tmp); return -1; }
    struct stat st; int rc = stat(tmp, &st); free(tmp);
    if (rc != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}
static char *find_executable(const char *name) {
    if (strchr(name, '/')) { if (access(name, X_OK) == 0) return realpath(name, NULL); return NULL; }
    const char *env = getenv("PATH"); if (!env) return NULL;
    char *copy = xstrdup(env), *save = NULL;
    for (char *part = strtok_r(copy, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
        if (!*part) part = ".";
        char *candidate = path_join(part, name);
        if (access(candidate, X_OK) == 0) { char *r = realpath(candidate, NULL); free(candidate); free(copy); return r; }
        free(candidate);
    }
    free(copy); return NULL;
}
static char *current_binary_path(const char *argv0) {
#ifdef __linux__
    char buf[PATH_MAX + 1]; ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX);
    if (n > 0) { buf[n] = '\0'; return xstrdup(buf); }
#endif
    if (strchr(argv0, '/')) { char *r = realpath(argv0, NULL); if (r) return r; }
    return find_executable(argv0);
}
static char *shell_quote(const char *s) {
    StrBuf b; sb_init(&b); sb_add(&b, "'");
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\'') sb_add(&b, "'\\''"); else sb_addn(&b, p, 1);
    }
    sb_add(&b, "'"); return b.data;
}
static char *extension_lower(const char *path) {
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.'); if (!dot) return xstrdup("");
    char *e = xstrdup(dot); for (char *p=e; *p; p++) *p = (char)tolower((unsigned char)*p); return e;
}
static bool is_audio_file(const char *path) {
    char *ext = extension_lower(path); bool yes = false;
    for (size_t i = 0; i < sizeof(AUDIO_EXTS)/sizeof(AUDIO_EXTS[0]); i++) if (strcmp(ext, AUDIO_EXTS[i]) == 0) { yes = true; break; }
    free(ext); return yes;
}
static bool is_regular_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

/* A tiny shell-independent line editor. TAB completes filenames in prompts, including paths with spaces. */
static void redraw_line(const char *prompt, const char *buf, size_t len) {
    fd_write_all(STDOUT_FILENO, "\r\033[2K", 5); fd_write_all(STDOUT_FILENO, prompt, strlen(prompt)); fd_write_all(STDOUT_FILENO, buf, len);
}
static char *path_completion_dir(const char *buf, char **leaf_out) {
    const char *slash = strrchr(buf, '/');
    char *dirpart = NULL; const char *leaf;
    if (slash) { size_t n = (size_t)(slash - buf); dirpart = xmalloc(n + 2); memcpy(dirpart, buf, n); dirpart[n] = '\0';
        if (n == 0) { free(dirpart); dirpart = xstrdup("/"); }
        else if (strcmp(dirpart, "~") == 0) { free(dirpart); dirpart = xstrdup("~/"); }
        leaf = slash + 1;
    } else { dirpart = xstrdup("."); leaf = buf; }
    char *expanded = expand_tilde(dirpart); free(dirpart); *leaf_out = xstrdup(leaf); return expanded;
}
static void complete_path(char *buf, size_t *len, size_t cap, const char *prompt) {
    char *leaf = NULL, *dir = path_completion_dir(buf, &leaf); DIR *d = opendir(dir);
    if (!d) { fd_write_all(STDOUT_FILENO, "\a", 1); free(dir); free(leaf); return; }
    char **matches = NULL; size_t n = 0, alloc = 0; struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, leaf, strlen(leaf)) != 0) continue;
        if (de->d_name[0] == '.' && leaf[0] != '.') continue;
        if (n == alloc) { alloc = alloc ? alloc * 2 : 16; matches = realloc(matches, alloc * sizeof(char*)); if (!matches) { closedir(d); free(dir); free(leaf); return; } }
        matches[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    if (n == 0) { fd_write_all(STDOUT_FILENO, "\a", 1); }
    else {
        size_t lcp = strlen(matches[0]);
        for (size_t i=1; i<n; i++) { size_t j=0; while (j<lcp && matches[0][j] && matches[i][j] && matches[0][j]==matches[i][j]) j++; lcp=j; }
        size_t leaflen = strlen(leaf);
        if (lcp > leaflen && *len + (lcp-leaflen) + 2 < cap) {
            memcpy(buf + *len, matches[0] + leaflen, lcp - leaflen); *len += lcp-leaflen; buf[*len]='\0';
            if (n == 1) {
                char *full = path_join(dir, matches[0]); struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode) && *len + 1 < cap) { buf[(*len)++]='/'; buf[*len]='\0'; }
                free(full);
            }
            redraw_line(prompt, buf, *len);
        } else if (n > 1) {
            fd_write_all(STDOUT_FILENO, "\r\n", 2);
            for (size_t i=0; i<n; i++) {
                char *full = path_join(dir, matches[i]); struct stat st;
                fd_write_all(STDOUT_FILENO, matches[i], strlen(matches[i]));
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) fd_write_all(STDOUT_FILENO, "/", 1);
                if (i + 1 < n) fd_write_all(STDOUT_FILENO, "\t", 1);
                if ((i+1)%5 == 0) fd_write_all(STDOUT_FILENO, "\r\n", 2);
                free(full);
            }
            fd_write_all(STDOUT_FILENO, "\r\n", 2); redraw_line(prompt, buf, *len);
        } else fd_write_all(STDOUT_FILENO, "\a", 1);
    }
    for (size_t i=0; i<n; i++) free(matches[i]);
    free(matches); free(dir); free(leaf);
}
static bool prompt_is_back(const char *value) { return value && strcmp(value, PROMPT_BACK) == 0; }

static char *read_line_prompt(const char *prompt, const char *initial, bool path_completion) {
    size_t cap = 256, len = 0; char *buf = xmalloc(cap); buf[0]='\0';
    if (initial && *initial) { len = strlen(initial); while (len + 2 > cap) cap *= 2; buf = realloc(buf, cap); if (!buf) return NULL; memcpy(buf, initial, len+1); }
    if (!isatty(STDIN_FILENO)) {
        printf("%s", prompt); fflush(stdout); char *line=NULL; size_t n=0; ssize_t got=getline(&line,&n,stdin); free(buf);
        if (got < 0) { free(line); return NULL; }
        while (got > 0 && (line[got-1]=='\n' || line[got-1]=='\r')) line[--got]='\0';
        if (got == 1 && (unsigned char)line[0] == 27) { free(line); return xstrdup(PROMPT_BACK); }
        if (got == 0 && initial && *initial) { free(line); return xstrdup(initial); }
        return line;
    }
    struct termios oldt, raw; bool have_termios = tcgetattr(STDIN_FILENO, &oldt) == 0;
    if (have_termios) { raw=oldt; raw.c_lflag &= (tcflag_t)~(ICANON|ECHO); raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0; (void)tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw); }
    fd_write_all(STDOUT_FILENO, prompt, strlen(prompt)); if (len) fd_write_all(STDOUT_FILENO,buf,len);
    bool go_back = false;
    for (;;) {
        char c; ssize_t r=read(STDIN_FILENO,&c,1); if (r <= 0) { free(buf); buf=NULL; break; }
        if (c=='\r' || c=='\n') { fd_write_all(STDOUT_FILENO,"\r\n",2); break; }
        if (c==3) { fd_write_all(STDOUT_FILENO,"\r\n",2); free(buf); buf=NULL; break; }
        if (c==4) { if (len==0) { free(buf); buf=NULL; } else fd_write_all(STDOUT_FILENO,"\a",1); break; }
        if (c==127 || c==8) { if (len) { len--; buf[len]='\0'; fd_write_all(STDOUT_FILENO,"\b \b",3); } continue; }
        if (c==21) { while (len) { len--; fd_write_all(STDOUT_FILENO,"\b \b",3); } buf[0]='\0'; continue; }
        if (c=='\t' && path_completion) { complete_path(buf,&len,cap,prompt); continue; }
        if (c==27) {
            /* Distinguish a bare Escape press from terminal cursor-key escape sequences. */
            struct termios timed = raw;
            timed.c_cc[VMIN] = 0;
            timed.c_cc[VTIME] = 2; /* 200 ms; a lone Escape has no following byte. */
            (void)tcsetattr(STDIN_FILENO,TCSANOW,&timed);
            char seqc = 0;
            ssize_t sr = read(STDIN_FILENO,&seqc,1);
            (void)tcsetattr(STDIN_FILENO,TCSANOW,&raw);
            if (sr <= 0) { fd_write_all(STDOUT_FILENO,"\r\n",2); go_back = true; break; }
            if (seqc=='[' || seqc=='O') {
                /* Swallow a complete ANSI cursor/function-key sequence. */
                for (int i=0; i<16; i++) {
                    timed = raw; timed.c_cc[VMIN]=0; timed.c_cc[VTIME]=1;
                    (void)tcsetattr(STDIN_FILENO,TCSANOW,&timed);
                    char tail = 0; ssize_t tr = read(STDIN_FILENO,&tail,1);
                    (void)tcsetattr(STDIN_FILENO,TCSANOW,&raw);
                    if (tr <= 0 || ((unsigned char)tail >= 0x40 && (unsigned char)tail <= 0x7e)) break;
                }
            }
            continue;
        }
        if ((unsigned char)c < 32) continue;
        if (len + 2 > cap) { cap *= 2; char *nb=realloc(buf,cap); if (!nb) { free(buf); buf=NULL; break; } buf=nb; }
        buf[len++]=c; buf[len]='\0'; fd_write_all(STDOUT_FILENO,&c,1);
    }
    if (have_termios) (void)tcsetattr(STDIN_FILENO,TCSAFLUSH,&oldt);
    if (go_back) { free(buf); return xstrdup(PROMPT_BACK); }
    return buf;
}

static char *read_nonempty_prompt(const char *prompt, const char *initial, const char *field_name) {
    for (;;) {
        char *value = read_line_prompt(prompt, initial, true);
        if (!value) return NULL;
        if (prompt_is_back(value)) {
            free(value);
            log_warn("There is no earlier entry before the %s prompt.", field_name);
            continue;
        }
        if (!*value) {
            free(value);
            log_warn("A %s is required; please enter a value.", field_name);
            continue;
        }
        return value;
    }
}

static int db_exec(const char *sql) {
    char *err=NULL; int rc=sqlite3_exec(g_db,sql,NULL,NULL,&err);
    if (rc != SQLITE_OK) { log_error("Database: %s", err ? err : sqlite3_errmsg(g_db)); sqlite3_free(err); return -1; }
    return 0;
}
static int db_prepare(const char *sql, sqlite3_stmt **st) {
    int rc=sqlite3_prepare_v2(g_db,sql,-1,st,NULL);
    if (rc != SQLITE_OK) { log_error("Database prepare failed: %s",sqlite3_errmsg(g_db)); return -1; } return 0;
}
static bool db_has_column(const char *table, const char *column) {
    char *sql = str_printf("PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (db_prepare(sql, &st) == 0) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && strcmp((const char *)name, column) == 0) { found = true; break; }
        }
        sqlite3_finalize(st);
    }
    free(sql);
    return found;
}
static int db_ensure_column(const char *table, const char *column, const char *definition) {
    if (db_has_column(table, column)) return 0;
    char *sql = str_printf("ALTER TABLE %s ADD COLUMN %s %s", table, column, definition);
    int rc = db_exec(sql);
    free(sql);
    return rc;
}
static int db_step_done(sqlite3_stmt *st) {
    int rc=sqlite3_step(st); if (rc != SQLITE_DONE) { log_error("Database write failed: %s",sqlite3_errmsg(g_db)); sqlite3_reset(st); sqlite3_clear_bindings(st); return -1; }
    sqlite3_reset(st); sqlite3_clear_bindings(st); return 0;
}
static int db_open(const char *path) {
    if (sqlite3_open_v2(path,&g_db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL)!=SQLITE_OK) { log_error("Cannot open database: %s", g_db ? sqlite3_errmsg(g_db) : "unknown error"); return -1; }
    (void)chmod(path,0600); sqlite3_busy_timeout(g_db,5000);
    sqlite3_soft_heap_limit64(4*1024*1024);
    if (db_exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-2048; PRAGMA mmap_size=0; PRAGMA temp_store=FILE; PRAGMA foreign_keys=ON;")) return -1;
    const char *schema =
      "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS runs("
      "run_uid TEXT PRIMARY KEY,created_at TEXT NOT NULL,target TEXT NOT NULL,output TEXT NOT NULL,"
      "recursive INTEGER NOT NULL,dedupe_all INTEGER NOT NULL,ask_artist INTEGER NOT NULL,ask_album INTEGER NOT NULL,ask_title INTEGER NOT NULL DEFAULT 0,windows_batch INTEGER NOT NULL DEFAULT 0,"
      "ask_lyrics INTEGER NOT NULL,ask_image INTEGER NOT NULL,format TEXT NOT NULL,status TEXT NOT NULL,"
      "command_text TEXT,generated_script TEXT,script_signature TEXT,script_sha256 TEXT,default_artist TEXT,default_album TEXT,last_error TEXT);"
      "CREATE TABLE IF NOT EXISTS files("
      "file_id INTEGER PRIMARY KEY AUTOINCREMENT,run_uid TEXT NOT NULL,path TEXT NOT NULL,relpath TEXT NOT NULL,basename TEXT NOT NULL,"
      "size INTEGER NOT NULL,mtime INTEGER NOT NULL,sha256 TEXT,hash_status INTEGER NOT NULL DEFAULT 0,duplicate_of INTEGER,"
      "status TEXT NOT NULL DEFAULT 'DISCOVERED',artist TEXT,album TEXT,title TEXT,lyrics_source TEXT,lyrics_path TEXT,lyrics_prompted INTEGER NOT NULL DEFAULT 0,image_path TEXT,output_path TEXT,error TEXT,"
      "UNIQUE(run_uid,path),FOREIGN KEY(run_uid) REFERENCES runs(run_uid));"
      "CREATE INDEX IF NOT EXISTS files_run_status ON files(run_uid,status);"
      "CREATE INDEX IF NOT EXISTS files_run_name ON files(run_uid,basename);"
      "CREATE INDEX IF NOT EXISTS files_run_hash ON files(run_uid,sha256);"
      "CREATE TABLE IF NOT EXISTS scripts(script_id INTEGER PRIMARY KEY AUTOINCREMENT,run_uid TEXT NOT NULL,path TEXT NOT NULL,"
      "signature_path TEXT NOT NULL,sha256 TEXT NOT NULL,fingerprint TEXT NOT NULL,created_at TEXT NOT NULL,"
      "FOREIGN KEY(run_uid) REFERENCES runs(run_uid));"
      "CREATE TABLE IF NOT EXISTS metadata_history(seq INTEGER PRIMARY KEY AUTOINCREMENT,run_uid TEXT NOT NULL,file_id INTEGER NOT NULL,field TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,FOREIGN KEY(run_uid) REFERENCES runs(run_uid));"
      "CREATE INDEX IF NOT EXISTS metadata_history_run_seq ON metadata_history(run_uid,seq);"
      "PRAGMA user_version=1;";
    if (db_exec(schema)) return -1;
    /* Additive migrations keep databases from earlier RAMIF builds usable. */
    if (db_ensure_column("runs", "ask_title", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        db_ensure_column("runs", "windows_batch", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        db_ensure_column("files", "title", "TEXT") != 0 ||
        db_ensure_column("files", "lyrics_prompted", "INTEGER NOT NULL DEFAULT 0") != 0) return -1;
    return 0;
}
static void db_close(void) { if (g_db) { sqlite3_close(g_db); g_db=NULL; } }
static char *setting_get(const char *key) {
    sqlite3_stmt *st=NULL; if (db_prepare("SELECT value FROM settings WHERE key=?1",&st)) return xstrdup("");
    sqlite3_bind_text(st,1,key,-1,SQLITE_TRANSIENT); int rc=sqlite3_step(st); char *v=(rc==SQLITE_ROW)?xstrdup((const char*)sqlite3_column_text(st,0)):xstrdup(""); sqlite3_finalize(st); return v;
}
static int setting_set(const char *key, const char *value) {
    sqlite3_stmt *st=NULL; if (db_prepare("INSERT INTO settings(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value",&st)) return -1;
    sqlite3_bind_text(st,1,key,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,value?value:"",-1,SQLITE_TRANSIENT); int rc=db_step_done(st); sqlite3_finalize(st); return rc;
}
static char *utc_now(void) {
    time_t t=time(NULL); struct tm tmv; gmtime_r(&t,&tmv); char b[32]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&tmv); return xstrdup(b);
}
static char *make_run_uid(const App *a) {
    unsigned char random[16]; if (RAND_bytes(random,sizeof(random)) != 1) { for (size_t i=0;i<sizeof(random);i++) random[i]=(unsigned char)(rand()&255); }
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    StrBuf seed; sb_init(&seed); sb_addf(&seed,"%ld.%09ld|%s|%s|%s|%s|%s|",(long)ts.tv_sec,ts.tv_nsec,a->system_name,a->system_release,a->username,a->hostname,a->binary_path);
    sb_addn(&seed,(const char*)random,sizeof(random));
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int dlen=0; EVP_Digest(seed.data,seed.len,digest,&dlen,EVP_sha256(),NULL);
    char hex[33]; for (int i=0;i<8;i++) snprintf(hex+i*4,5,"%02x%02x",digest[i*2],digest[i*2+1]); hex[32]='\0';
    struct tm tmv; gmtime_r(&ts.tv_sec,&tmv); char stamp[32]; strftime(stamp,sizeof(stamp),"%Y%m%dT%H%M%SZ",&tmv); sb_free(&seed);
    return str_printf("RAMIF-%s-%s",stamp,hex);
}
static int hash_file_stream(const char *path, char out[65]) {
    int fd=open(path,O_RDONLY|O_CLOEXEC); if (fd<0) return -1;
    struct stat before, after; if (fstat(fd,&before)!=0 || !S_ISREG(before.st_mode)) { close(fd); errno=EINVAL; return -1; }
    EVP_MD_CTX *ctx=EVP_MD_CTX_new(); if (!ctx) { close(fd); errno=ENOMEM; return -1; }
    if (EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)!=1) { EVP_MD_CTX_free(ctx); close(fd); errno=EIO; return -1; }
    unsigned char *buf=xmalloc(HASH_CHUNK); ssize_t n; int ok=1;
    while (!g_interrupted && (n=read(fd,buf,HASH_CHUNK))>0) if (EVP_DigestUpdate(ctx,buf,(size_t)n)!=1) { ok=0; break; }
    if (g_interrupted) ok=0;
    if (n<0) ok=0;
    unsigned int dlen=0; unsigned char d[EVP_MAX_MD_SIZE]; if (ok && EVP_DigestFinal_ex(ctx,d,&dlen)!=1) ok=0;
    if (fstat(fd,&after)!=0 || before.st_size!=after.st_size || before.st_mtime!=after.st_mtime) ok=0;
    if (ok && dlen==32) { for (unsigned int i=0;i<dlen;i++) snprintf(out+i*2,3,"%02x",d[i]); out[64]='\0'; } else ok=0;
    free(buf); EVP_MD_CTX_free(ctx); close(fd); memory_trim(); if (!ok) { errno=EIO; return -1; } return 0;
}
static char *get_file_output_path(const char *output_root, const char *relpath, const char *fmt) {
    const char *ext = strcmp(fmt,"mp3")==0 ? ".mp3" : strcmp(fmt,"flac")==0 ? ".flac" : strcmp(fmt,"opus")==0 ? ".opus" : strcmp(fmt,"aac")==0 ? ".m4a" : ".wav";
    char *copy=xstrdup(relpath); char *slash=strrchr(copy,'/'); char *dot=strrchr(copy,'.');
    if (dot && (!slash || dot>slash)) *dot='\0';
    char *with_ext=str_printf("%s%s",copy,ext); char *out=path_join(output_root,with_ext); free(copy); free(with_ext); return out;
}
static int capture_program(char *const argv[], char **output) {
    int pipefd[2]; if (pipe(pipefd)!=0) return -1;
    pid_t pid=fork(); if (pid<0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid==0) {
        close(pipefd[0]); dup2(pipefd[1],STDOUT_FILENO); close(pipefd[1]);
        int devnull=open("/dev/null",O_WRONLY); if (devnull>=0) { dup2(devnull,STDERR_FILENO); close(devnull); }
        execvp(argv[0],argv); _exit(127);
    }
    close(pipefd[1]); StrBuf b; sb_init(&b); char chunk[4096]; ssize_t n;
    while ((n=read(pipefd[0],chunk,sizeof(chunk)))>0) sb_addn(&b,chunk,(size_t)n);
    close(pipefd[0]); int status=0; waitpid(pid,&status,0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)!=0) { sb_free(&b); return -1; }
    *output=b.data; return 0;
}
static int run_program(char *const argv[], bool quiet) {
    pid_t pid=fork(); if (pid<0) return -1;
    if (pid==0) {
        if (quiet) { int dn=open("/dev/null",O_WRONLY); if (dn>=0) { dup2(dn,STDOUT_FILENO); dup2(dn,STDERR_FILENO); close(dn); } }
        execvp(argv[0],argv); _exit(127);
    }
    int status; while (waitpid(pid,&status,0)<0 && errno==EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
static int run_gpg(App *a, char **extra, bool quiet) {
    size_t n=0; while (extra[n]) n++;
    char **argv=xmalloc((n+4)*sizeof(char*)); argv[0]=a->gpg_path; argv[1]="--homedir"; argv[2]=a->gpg_home;
    for (size_t i=0;i<n;i++) argv[i+3]=extra[i];
    argv[n+3]=NULL;
    int rc=run_program(argv,quiet); free(argv); return rc;
}
static char *get_gpg_fingerprint(App *a) {
    char *argv[] = {a->gpg_path,"--homedir",a->gpg_home,"--batch","--with-colons","--list-secret-keys",NULL};
    char *txt=NULL; if (capture_program(argv,&txt)!=0 || !txt) return NULL;
    char *save=NULL, *line=strtok_r(txt,"\n",&save); char *fp=NULL;
    while (line) {
        if (strncmp(line,"fpr:",4)==0) {
            char *copy=xstrdup(line), *rest=copy, *field=NULL; int idx=0;
            while (rest) { field=strsep(&rest,":"); if (idx==9) break; idx++; }
            if (field && strlen(field)>=40) fp=xstrdup(field);
            free(copy);
            if (fp) break;
        }
        line=strtok_r(NULL,"\n",&save);
    }
    free(txt); return fp;
}
static int ensure_signing_key(App *a) {
    if (mkdir_p(a->gpg_home,0700)!=0) { log_error("Cannot create private GPG home %s: %s",a->gpg_home,strerror(errno)); return -1; }
    (void)chmod(a->gpg_home,0700);
    a->fingerprint=get_gpg_fingerprint(a);
    if (!a->fingerprint) {
        log_info("First launch: creating a dedicated local OpenPGP script-signing key...");
        char *uid=str_printf("RAMIF Script Signing (%s@%s)",a->username,a->hostname);
        char *args[] = {"--batch","--yes","--pinentry-mode","loopback","--passphrase","","--quick-generate-key",uid,"ed25519","sign","0",NULL};
        int rc=run_gpg(a,args,false); free(uid);
        if (rc!=0) { log_error("GPG key generation failed (exit %d). Check that GnuPG is installed.",rc); return -1; }
        a->fingerprint=get_gpg_fingerprint(a);
        if (!a->fingerprint) { log_error("GPG key was not found after generation."); return -1; }
    }
    char *exp_args[] = {"--batch","--yes","--armor","--output",a->public_key_path,"--export",a->fingerprint,NULL};
    if (run_gpg(a,exp_args,true)!=0) { log_error("Could not export the public signing key."); return -1; }
    (void)chmod(a->public_key_path,0600); return 0;
}
static int write_ini(const App *a) {
    char *tmp=str_printf("%s.tmp",a->ini_path); FILE *f=fopen(tmp,"w"); if (!f) { log_error("Cannot write config %s: %s",tmp,strerror(errno)); free(tmp); return -1; }
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv); char timebuf[64]; strftime(timebuf,sizeof(timebuf),"%Y-%m-%d %H:%M:%S %z",&tmv);
    fprintf(f,"# %s configuration. Values are updated at launch.\n",APP_NAME);
    fprintf(f,"[USER]\nname = %s\nuid = %ld\n\n",a->username,(long)getuid());
    fprintf(f,"[HOST]\nname = %s\nsystem = %s\nrelease = %s\n\n",a->hostname,a->system_name,a->system_release);
    fprintf(f,"[FFMPEG]\npath = %s\n\n",a->ffmpeg_path);
    fprintf(f,"[PATH]\nbinary = %s\ndata_dir = %s\ndatabase = %s\n\n",a->binary_path,a->data_dir,a->db_path);
    fprintf(f,"[KEY]\ngpg_home = %s\nprivate_key_directory = %s/private-keys-v1.d\npublic_keyring = %s/pubring.kbx\npublic_key_export = %s\nfingerprint = %s\nprotection = unprotected; rely on user-account and directory permissions\n\n",a->gpg_home,a->gpg_home,a->gpg_home,a->public_key_path,a->fingerprint?a->fingerprint:"");
    fprintf(f,"[MISC]\nversion = %s\ncreated_or_updated = %s\nclipboard = pbcopy (macOS), wl-copy, xclip, or xsel (Linux) when available\npath_completion = built-in Tab completion; Bash and Zsh completion files included\n",APP_VERSION,timebuf);
    if (fclose(f)!=0) { log_error("Failed writing config."); unlink(tmp); free(tmp); return -1; }
    chmod(tmp,0600); if (rename(tmp,a->ini_path)!=0) { log_error("Cannot replace config: %s",strerror(errno)); unlink(tmp); free(tmp); return -1; }
    free(tmp); return 0;
}
static void migrate_legacy_data_dir(App *a, const char *home, const char *xdg) {
    char *legacy = NULL;
    char *base = NULL;
    struct stat old_st, new_st;
    if (stat(a->data_dir, &new_st) == 0) return;
    if (xdg && *xdg) legacy = path_join(xdg, "recode_all_music_in_folder");
    else {
        base = path_join(home, ".local/share");
        legacy = path_join(base, "recode_all_music_in_folder");
        free(base);
    }
    if (legacy && stat(legacy, &old_st) == 0 && S_ISDIR(old_st.st_mode)) {
        /* On a fresh macOS account the Application Support parent may not exist yet. */
        char *new_parent = path_dirname_dup(a->data_dir);
        int parent_rc = new_parent ? mkdir_p(new_parent, 0700) : -1;
        int parent_errno = errno;
        free(new_parent);
        if (parent_rc == 0 && rename(legacy, a->data_dir) == 0) {
            const char *pairs[][2] = {
                {"ramif.ini", "recode_all_music_in_folder.ini"}
            };
            size_t i;
            for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
                char *old_file = path_join(a->data_dir, pairs[i][0]);
                char *new_file = path_join(a->data_dir, pairs[i][1]);
                struct stat st;
                if (old_file && new_file && stat(old_file, &st) == 0 && stat(new_file, &st) != 0)
                    (void)rename(old_file, new_file);
                free(old_file); free(new_file);
            }
            log_info("Migrated existing state directory from %s to %s.", legacy, a->data_dir);
        } else {
            if (parent_rc != 0) errno = parent_errno;
            log_warn("Could not automatically migrate legacy state directory %s: %s", legacy, strerror(errno));
        }
    }
    free(legacy);
}

static int init_app(App *a, const char *argv0) {
    memset(a,0,sizeof(*a));
    const char *home=getenv("HOME"); if (!home || !*home) { struct passwd *pw=getpwuid(getuid()); home=pw?pw->pw_dir:"/tmp"; }
    a->home=xstrdup(home);
    const char *xdg=getenv("XDG_DATA_HOME");
    if (xdg && *xdg) a->data_dir=path_join(xdg,APP_ID);
#ifdef __APPLE__
    else { char *base=path_join(home,"Library/Application Support"); a->data_dir=path_join(base,"RAMIF"); free(base); }
#else
    else { char *base=path_join(home,".local/share"); a->data_dir=path_join(base,APP_ID); free(base); }
#endif
    a->db_path=path_join(a->data_dir,"ramif_database.db"); a->ini_path=path_join(a->data_dir,"recode_all_music_in_folder.ini");
    a->gpg_home=path_join(a->data_dir,"gnupg"); a->public_key_path=path_join(a->data_dir,"public-key.asc");
    a->binary_path=current_binary_path(argv0); a->ffmpeg_path=find_executable("ffmpeg"); a->gpg_path=find_executable("gpg");
    struct passwd *pw=getpwuid(getuid()); a->username=xstrdup(pw && pw->pw_name ? pw->pw_name : "unknown");
    char host[256]="unknown"; if (gethostname(host,sizeof(host)-1)!=0) strcpy(host,"unknown"); host[sizeof(host)-1]='\0'; a->hostname=xstrdup(host);
    struct utsname u; if (uname(&u)==0) { a->system_name=xstrdup(u.sysname); a->system_release=xstrdup(u.release); } else { a->system_name=xstrdup("Linux"); a->system_release=xstrdup("unknown"); }
    if (!a->binary_path) a->binary_path=xstrdup(argv0);
    if (!a->ffmpeg_path) { log_error("ffmpeg was not found in PATH."); return -1; }
    if (!a->gpg_path) { log_error("gpg was not found in PATH."); return -1; }
    migrate_legacy_data_dir(a, home, xdg);
    if (mkdir_p(a->data_dir,0700)!=0) { log_error("Cannot create state directory %s: %s",a->data_dir,strerror(errno)); return -1; }
    chmod(a->data_dir,0700); return 0;
}
static void free_app(App *a) {
    free(a->home); free(a->data_dir); free(a->db_path); free(a->ini_path); free(a->gpg_home); free(a->public_key_path); free(a->binary_path); free(a->ffmpeg_path); free(a->gpg_path); free(a->fingerprint); free(a->username); free(a->hostname); free(a->system_name); free(a->system_release); memset(a,0,sizeof(*a));
}

static void print_banner(void) {
    const char *art =
"  ____   _    __  __ ___ _____\n"
" |  _ \\ / \\  |  \\/  |_ _|  ___|\n"
" | |_) / _ \\ | |\\  / || || |_\n"
" |  _ / ___ \\| | | | || ||  _|\n"
" |_|/_/   \\_\\_| |_| |___|_|\n"
"  MUSIC RECODE / HASH / RESUME / SIGN\n";
    if (g_color) printf(GREEN "%s" RESET,art); else printf("%s",art);
    printf("%s  version %s  |  low-memory scan  |  SQLite + SHA-256  |  FFmpeg\n", DIMGREEN,APP_VERSION);
    if (g_color) fputs(RESET,stdout);
}
static void usage(FILE *f) {
    fprintf(f,
"Usage: %s --target PATH --output PATH [options]\n"
"       %s --manual-input [options]\n"
"       %s --resume [options]\n"
"       %s --flush\n\n"
"Core options:\n"
"  --target PATH          Directory containing source audio\n"
"  --output PATH          Destination directory for recoded audio\n"
"  --no-recursive         Scan only the top-level directory\n"
"  --recursive            Recursively scan (default)\n"
"  --format FORMAT        mp3 (default), flac, opus, aac, or wav\n"
"  --no-duplicates        SHA-256 every audio file; plan only one identical copy\n"
"  --no-clipboard         Do not copy the generated command/script launcher\n"
"  --windows-batch        Generate a Windows .bat launcher plus signed PowerShell worker\n"
"  --manual-input         Prompt for target/output (Tab completes paths)\n"
"  --resume               Resume the latest unfinished database run\n"
"  --reenter-metadata     Re-prompt metadata already saved in a resumed run\n"
"  --lyrics               Prompt for optional lyrics per track (Enter/none skips)\n"
"  --artist               Prompt per file for artist; last artist is prefilled\n"
"  --album                Prompt per file for album; last album is prefilled\n"
"  --title                Prompt per file for song title; filename is prefilled\n"
"  --image                Prompt per file for embedded cover image (MP3 only)\n"
"  --flush                Clear RAMIF database records (preserves config and GPG key)\n"
"  --verify-script PATH   Verify PATH using its adjacent PATH.asc signature\n"
"  --help                 Show this help\n\n"
"Notes: originals are never deleted and conversions are not run during planning.\n"
"       Small plans produce a one-liner; plans over %d characters become signed scripts.\n"
"       Built-in Tab path completion and included Bash/Zsh completion files are provided.\n"
"       In metadata prompts, press Escape to revisit a previous entry.\n",APP_NAME,APP_NAME,APP_NAME,APP_NAME,ONE_LINER_LIMIT);
}
static bool need_value(int argc, char **argv, int *i, const char *arg, const char **value) {
    const char *eq=strchr(arg,'='); if (eq) { *value=eq+1; return true; }
    if (*i+1>=argc) { log_error("Missing value after %s",arg); return false; } *value=argv[++*i]; return true;
}
static int parse_args(int argc, char **argv, Options *o) {
    memset(o,0,sizeof(*o)); o->recursive=true; o->format=xstrdup("mp3");
    for (int i=1;i<argc;i++) {
        const char *arg=argv[i], *v=NULL;
        if (strcmp(arg,"--help")==0 || strcmp(arg,"-h")==0) { usage(stdout); return 1; }
        else if (strcmp(arg,"--target")==0 || strncmp(arg,"--target=",9)==0) { if (!need_value(argc,argv,&i,arg,&v)) return -1; free(o->target); o->target=xstrdup(v); }
        else if (strcmp(arg,"--output")==0 || strncmp(arg,"--output=",9)==0) { if (!need_value(argc,argv,&i,arg,&v)) return -1; free(o->output); o->output=xstrdup(v); }
        else if (strcmp(arg,"--format")==0 || strncmp(arg,"--format=",9)==0) { if (!need_value(argc,argv,&i,arg,&v)) return -1; free(o->format); o->format=xstrdup(v); o->format_set=true; }
        else if (strcmp(arg,"--no-clipboard")==0) o->no_clipboard=true;
        else if (strcmp(arg,"--windows-batch")==0 || strcmp(arg,"--batch-script")==0) o->windows_batch=true;
        else if (strcmp(arg,"--no-duplicates")==0) { o->no_duplicates=true; o->no_duplicates_set=true; }
        else if (strcmp(arg,"--manual-input")==0) o->manual_input=true;
        else if (strcmp(arg,"--lyrics")==0) o->ask_lyrics=true;
        else if (strcmp(arg,"--artist")==0) o->ask_artist=true;
        else if (strcmp(arg,"--album")==0) o->ask_album=true;
        else if (strcmp(arg,"--title")==0) o->ask_title=true;
        else if (strcmp(arg,"--image")==0) o->ask_image=true;
        else if (strcmp(arg,"--recursive")==0) { o->recursive=true; o->recursive_set=true; }
        else if (strcmp(arg,"--no-recursive")==0) { o->recursive=false; o->recursive_set=true; }
        else if (strcmp(arg,"--resume")==0) o->resume=true;
        else if (strcmp(arg,"--reenter-metadata")==0) o->reenter_metadata=true;
        else if (strcmp(arg,"--flush")==0) o->flush=true;
        else if (strcmp(arg,"--verify-script")==0 || strncmp(arg,"--verify-script=",16)==0) { if (!need_value(argc,argv,&i,arg,&v)) return -1; o->verify_script=xstrdup(v); }
        else { log_error("Unknown option: %s",arg); usage(stderr); return -1; }
    }
    if (strcmp(o->format,"mp3") && strcmp(o->format,"flac") && strcmp(o->format,"opus") && strcmp(o->format,"aac") && strcmp(o->format,"wav")) { log_error("Unsupported format: %s",o->format); return -1; }
    return 0;
}

static int flush_database(App *a) {
    const char *suffixes[]={"","-wal","-shm"};
    for (size_t i=0;i<3;i++) { char *p=str_printf("%s%s",a->db_path,suffixes[i]); if (unlink(p)!=0 && errno!=ENOENT) { log_error("Cannot remove %s: %s",p,strerror(errno)); free(p); return -1; } free(p); }
    if (db_open(a->db_path)!=0) return -1;
    log_info("Database cleared and schema recreated: %s",a->db_path);
    log_info("Config and signing keys were preserved; use the same local signing identity as before."); return 0;
}

static int verify_script(App *a, const char *path) {
    char *sig=str_printf("%s.asc",path);
    if (!is_regular_file(path) || !is_regular_file(sig)) { log_error("Expected script and adjacent signature: %s and %s",path,sig); free(sig); return -1; }
    if (ensure_signing_key(a)!=0) { free(sig); return -1; }
    char *args[]={"--batch","--verify",sig,(char*)path,NULL};
    int rc=run_gpg(a,args,false); free(sig);
    if (rc==0) log_info("Signature valid. This confirms integrity under the local RAMIF signing key, not that a script is safe to run.");
    else log_error("Signature verification failed.");
    return rc==0?0:-1;
}

static int create_run(App *a, Options *o, char **uid_out) {
    char *uid=make_run_uid(a), *now=utc_now(); sqlite3_stmt *st=NULL;
    const char *sql="INSERT INTO runs(run_uid,created_at,target,output,recursive,dedupe_all,ask_artist,ask_album,ask_title,windows_batch,ask_lyrics,ask_image,format,status) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,'SCANNING')";
    if (db_prepare(sql,&st)) { free(uid); free(now); return -1; }
    sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,now,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,o->target,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,o->output,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(st,5,o->recursive); sqlite3_bind_int(st,6,o->no_duplicates); sqlite3_bind_int(st,7,o->ask_artist); sqlite3_bind_int(st,8,o->ask_album); sqlite3_bind_int(st,9,o->ask_title); sqlite3_bind_int(st,10,o->windows_batch); sqlite3_bind_int(st,11,o->ask_lyrics); sqlite3_bind_int(st,12,o->ask_image); sqlite3_bind_text(st,13,o->format,-1,SQLITE_TRANSIENT);
    int rc=db_step_done(st); sqlite3_finalize(st); free(now); if (rc) { free(uid); return -1; } *uid_out=uid; return 0;
}
typedef struct { char *uid,*target,*output,*format,*status,*default_artist,*default_album; bool recursive,dedupe_all,ask_artist,ask_album,ask_title,ask_lyrics,ask_image,windows_batch; } RunInfo;
static void free_runinfo(RunInfo *r) { free(r->uid); free(r->target); free(r->output); free(r->format); free(r->status); free(r->default_artist); free(r->default_album); memset(r,0,sizeof(*r)); }
static int load_run(const char *uid, RunInfo *r) {
    sqlite3_stmt *st=NULL; const char *sql="SELECT run_uid,target,output,format,status,recursive,dedupe_all,ask_artist,ask_album,ask_title,windows_batch,ask_lyrics,ask_image,COALESCE(default_artist,''),COALESCE(default_album,'') FROM runs WHERE run_uid=?1";
    if (db_prepare(sql,&st)) return -1;
    sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT); int rc=sqlite3_step(st);
    if (rc!=SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    r->uid=xstrdup((const char*)sqlite3_column_text(st,0)); r->target=xstrdup((const char*)sqlite3_column_text(st,1)); r->output=xstrdup((const char*)sqlite3_column_text(st,2)); r->format=xstrdup((const char*)sqlite3_column_text(st,3)); r->status=xstrdup((const char*)sqlite3_column_text(st,4));
    r->recursive=sqlite3_column_int(st,5)!=0; r->dedupe_all=sqlite3_column_int(st,6)!=0; r->ask_artist=sqlite3_column_int(st,7)!=0; r->ask_album=sqlite3_column_int(st,8)!=0; r->ask_title=sqlite3_column_int(st,9)!=0; r->windows_batch=sqlite3_column_int(st,10)!=0; r->ask_lyrics=sqlite3_column_int(st,11)!=0; r->ask_image=sqlite3_column_int(st,12)!=0;
    r->default_artist=xstrdup((const char*)sqlite3_column_text(st,13)); r->default_album=xstrdup((const char*)sqlite3_column_text(st,14)); sqlite3_finalize(st); return 0;
}
static int load_latest_run(RunInfo *r) {
    sqlite3_stmt *st=NULL; if (db_prepare("SELECT run_uid FROM runs WHERE status!='DONE' ORDER BY rowid DESC LIMIT 1",&st)) return -1;
    int rc=sqlite3_step(st); char *uid=rc==SQLITE_ROW?xstrdup((const char*)sqlite3_column_text(st,0)):NULL; sqlite3_finalize(st);
    if (!uid) { log_error("No unfinished run is available to resume."); return -1; }
    int out=load_run(uid,r); free(uid); return out;
}
static int update_run_status(const char *uid,const char *status) {
    sqlite3_stmt *st=NULL; if (db_prepare("UPDATE runs SET status=?2 WHERE run_uid=?1",&st)) return -1; sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,status,-1,SQLITE_TRANSIENT); int rc=db_step_done(st); sqlite3_finalize(st); return rc;
}
static bool should_skip_output_subtree(const char *out_root,const char *candidate) { return is_prefix_path(out_root,candidate); }
static int upsert_audio_file(const char *uid,const char *target,const char *path) {
    char *rel=NULL;
    size_t tn=strlen(target);
    if (strncmp(path,target,tn)==0 && path[tn]=='/') rel=xstrdup(path+tn+1); else rel=path_basename_dup(path);
    char *base=path_basename_dup(path); struct stat stbuf;
    if (lstat(path,&stbuf)!=0 || !S_ISREG(stbuf.st_mode)) { free(rel); free(base); return 0; }
    sqlite3_stmt *st=NULL;
    const char *sql="INSERT INTO files(run_uid,path,relpath,basename,size,mtime,status) VALUES(?1,?2,?3,?4,?5,?6,'DISCOVERED') "
        "ON CONFLICT(run_uid,path) DO UPDATE SET relpath=excluded.relpath,basename=excluded.basename,size=excluded.size,mtime=excluded.mtime,"
        "sha256=CASE WHEN files.size!=excluded.size OR files.mtime!=excluded.mtime THEN NULL ELSE files.sha256 END,"
        "hash_status=CASE WHEN files.size!=excluded.size OR files.mtime!=excluded.mtime THEN 0 ELSE files.hash_status END,"
        "duplicate_of=CASE WHEN files.size!=excluded.size OR files.mtime!=excluded.mtime THEN NULL ELSE files.duplicate_of END,"
        "status=CASE WHEN files.size!=excluded.size OR files.mtime!=excluded.mtime THEN 'DISCOVERED' ELSE files.status END,"
        "error=CASE WHEN files.size!=excluded.size OR files.mtime!=excluded.mtime THEN NULL ELSE files.error END";
    int rc=0;
    if (db_prepare(sql,&st)!=0) rc=-1;
    else {
        sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,path,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,rel,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,base,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(st,5,(sqlite3_int64)stbuf.st_size); sqlite3_bind_int64(st,6,(sqlite3_int64)stbuf.st_mtime); rc=db_step_done(st); sqlite3_finalize(st);
    }
    free(rel); free(base); return rc;
}
static int walk_dir(const char *uid,const char *target,const char *out,const char *dir,bool recursive,uint64_t *count) {
    if (g_interrupted) return -1;
    DIR *dp=opendir(dir); if (!dp) { log_warn("Cannot read directory %s: %s",dir,strerror(errno)); return 0; }
    struct dirent *de;
    while (!g_interrupted && (de=readdir(dp))!=NULL) {
        if (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        char *p=path_join(dir,de->d_name); struct stat st;
        if (lstat(p,&st)!=0) { free(p); continue; }
        if (S_ISDIR(st.st_mode)) {
            if (recursive && !should_skip_output_subtree(out,p)) {
                if (walk_dir(uid,target,out,p,recursive,count)!=0) { free(p); closedir(dp); return -1; }
            }
        } else if (S_ISREG(st.st_mode) && is_audio_file(p)) {
            if (path_has_linebreak(p)) { log_warn("Skipping filename containing a newline/carriage return: %s",de->d_name); }
            else {
                if (upsert_audio_file(uid,target,p)!=0) { free(p); closedir(dp); return -1; }
                (*count)++; if ((*count%100)==0) { printf("\r" GREEN "[SCAN]" RESET " indexed %llu audio files...",(unsigned long long)*count); fflush(stdout); }
            }
        }
        free(p);
    }
    closedir(dp); return 0;
}
static int scan_directory(RunInfo *r) {
    log_info("Scanning %s (%s)",r->target,r->recursive?"recursive":"top-level only");
    uint64_t count=0;
    if (walk_dir(r->uid,r->target,r->output,r->target,r->recursive,&count)!=0 || g_interrupted) return -1;
    if (count>=100) putchar('\n');
    sqlite3_stmt *st=NULL; if (db_prepare("SELECT COUNT(*) FROM files WHERE run_uid=?1",&st)) return -1;
    sqlite3_bind_text(st,1,r->uid,-1,SQLITE_TRANSIENT); int rc=sqlite3_step(st); sqlite3_int64 total=rc==SQLITE_ROW?sqlite3_column_int64(st,0):0; sqlite3_finalize(st);
    log_info("Scan inventory contains %lld audio file(s).",(long long)total);
    if (update_run_status(r->uid,"SCANNED")!=0) return -1;
    memory_trim(); return 0;
}
static int hash_one_candidate(RunInfo *r, sqlite3_int64 id, const char *path,const char *basename) {
    char hash[65]; if (hash_file_stream(path,hash)!=0) {
        log_warn("Could not hash %s (file changed or unreadable); it remains eligible.",path);
        sqlite3_stmt *er=NULL; if (!db_prepare("UPDATE files SET error='SHA-256 failed',hash_status=1,status='HASH_ERROR' WHERE file_id=?1",&er)) { sqlite3_bind_int64(er,1,id); db_step_done(er); sqlite3_finalize(er); }
        return 0;
    }
    sqlite3_stmt *find=NULL; const char *q=r->dedupe_all?
        "SELECT file_id FROM files WHERE run_uid=?1 AND sha256=?2 AND duplicate_of IS NULL AND file_id!=?3 ORDER BY path LIMIT 1":
        "SELECT file_id FROM files WHERE run_uid=?1 AND basename=?2 AND sha256=?3 AND duplicate_of IS NULL AND file_id!=?4 ORDER BY path LIMIT 1";
    if (db_prepare(q,&find)) return -1;
    sqlite3_bind_text(find,1,r->uid,-1,SQLITE_TRANSIENT);
    if (r->dedupe_all) { sqlite3_bind_text(find,2,hash,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(find,3,id); }
    else { sqlite3_bind_text(find,2,basename,-1,SQLITE_TRANSIENT); sqlite3_bind_text(find,3,hash,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(find,4,id); }
    int fr=sqlite3_step(find); sqlite3_int64 duplicate_id=fr==SQLITE_ROW?sqlite3_column_int64(find,0):0; sqlite3_finalize(find);
    sqlite3_stmt *up=NULL;
    if (db_prepare("UPDATE files SET sha256=?2,hash_status=1,duplicate_of=?3,status=?4,error=NULL WHERE file_id=?1",&up)) return -1;
    sqlite3_bind_int64(up,1,id); sqlite3_bind_text(up,2,hash,-1,SQLITE_TRANSIENT);
    if (duplicate_id) sqlite3_bind_int64(up,3,duplicate_id); else sqlite3_bind_null(up,3);
    sqlite3_bind_text(up,4,duplicate_id?"DUPLICATE":"HASHED",-1,SQLITE_STATIC); int rc=db_step_done(up); sqlite3_finalize(up);
    if (duplicate_id) log_info("Duplicate content: %s (kept file id %lld)",path,(long long)duplicate_id);
    return rc;
}
static int hash_candidates(RunInfo *r) {
    if (r->dedupe_all) log_info("Hashing every audio file with streaming SHA-256 (duplicate filtering enabled).");
    else log_info("Hashing only same-name groups; unique filenames are not content-hashed in this mode.");
    sqlite3_stmt *reset=NULL;
    if (db_prepare("UPDATE files SET duplicate_of=NULL WHERE run_uid=?1 AND hash_status=0",&reset)) return -1;
    sqlite3_bind_text(reset,1,r->uid,-1,SQLITE_TRANSIENT); db_step_done(reset); sqlite3_finalize(reset);
    const char *sql = r->dedupe_all ?
      "SELECT file_id,path,basename FROM files WHERE run_uid=?1 AND hash_status=0 ORDER BY path LIMIT 1":
      "SELECT file_id,path,basename FROM files WHERE run_uid=?1 AND hash_status=0 AND basename IN (SELECT basename FROM files WHERE run_uid=?1 GROUP BY basename HAVING COUNT(*)>1) ORDER BY path LIMIT 1";
    uint64_t n=0;
    /* Stable bounded-memory loop: fetch one unprocessed candidate, hash it, repeat. */
    for (;;) {
        if (g_interrupted) return -1;
        sqlite3_stmt *one=NULL; if (db_prepare(sql,&one)) return -1; sqlite3_bind_text(one,1,r->uid,-1,SQLITE_TRANSIENT);
        int sr=sqlite3_step(one);
        if (sr!=SQLITE_ROW) { sqlite3_finalize(one); break; }
        sqlite3_int64 id=sqlite3_column_int64(one,0); char *path=xstrdup((const char*)sqlite3_column_text(one,1)); char *base=xstrdup((const char*)sqlite3_column_text(one,2)); sqlite3_finalize(one);
        if (hash_one_candidate(r,id,path,base)!=0) { free(path); free(base); return -1; }
        free(path); free(base); n++; if (n%25==0) { printf("\r" GREEN "[SHA-256]" RESET " hashed %llu candidate file(s)...",(unsigned long long)n); fflush(stdout); memory_trim(); }
    }
    if (n>=25) putchar('\n');
    if (!r->dedupe_all) {
        sqlite3_stmt *st2=NULL; if (!db_prepare("UPDATE files SET hash_status=1,status=CASE WHEN status='DISCOVERED' THEN 'HASHED' ELSE status END WHERE run_uid=?1 AND hash_status=0",&st2)) { sqlite3_bind_text(st2,1,r->uid,-1,SQLITE_TRANSIENT); db_step_done(st2); sqlite3_finalize(st2); }
    }
    update_run_status(r->uid,"HASHED"); memory_trim(); log_info("SHA-256 deduplication pass complete."); return 0;
}

static char *prompt_path_value(const char *label, bool allow_blank) {
    for (;;) {
        char *v=read_line_prompt(label,"",true); if (!v) return NULL;
        if (prompt_is_back(v)) return v;
        char *start=v; while (*start && isspace((unsigned char)*start)) start++;
        size_t n=strlen(start); while (n && isspace((unsigned char)start[n-1])) start[--n]='\0';
        if (!*start || (allow_blank && (!strcasecmp(start,"skip") || !strcasecmp(start,"none") ||
                                       !strcasecmp(start,"no") || !strcasecmp(start,"n") ||
                                       !strcasecmp(start,"no lyrics") || strcmp(start,"-")==0))) {
            free(v); return allow_blank?xstrdup(""):NULL;
        }
        char *expanded=expand_tilde(start); free(v);
        if (!is_regular_file(expanded)) { log_warn("That path is not a readable regular file. Use TAB to complete it, or press Enter/type 'none' to skip."); free(expanded); continue; }
        char *abs=realpath(expanded,NULL); free(expanded); if (!abs) { log_warn("Cannot resolve path: %s",strerror(errno)); continue; }
        if (path_has_linebreak(abs)) { log_warn("Paths with embedded newlines are not supported in generated commands."); free(abs); continue; }
        return abs;
    }
}
static char *title_from_path(const char *path) {
    char *title=path_basename_dup(path);
    char *dot=strrchr(title,'.');
    if (dot && dot!=title) *dot='\0';
    return title;
}
static void trim_inplace(char *s) {
    if (!s) return;
    char *start=s; while (*start && isspace((unsigned char)*start)) start++;
    if (start!=s) memmove(s,start,strlen(start)+1);
    size_t n=strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n]='\0';
}
static bool looks_like_srt(const char *p) { char *e=extension_lower(p); bool b=!strcmp(e,".srt"); free(e); return b; }
static bool looks_like_lyrics(const char *p) { char *e=extension_lower(p); bool b=!strcmp(e,".srt")||!strcmp(e,".txt")||!strcmp(e,".lrc"); free(e); return b; }
static int extract_srt_text(const char *src,const char *dst) {
    FILE *in=fopen(src,"r"); if (!in) return -1;
    char *dir=path_dirname_dup(dst); if (mkdir_p(dir,0700)!=0) { free(dir); fclose(in); return -1; } free(dir);
    FILE *out=fopen(dst,"w"); if (!out) { fclose(in); return -1; } chmod(dst,0600);
    char *line=NULL; size_t cap=0; ssize_t n; bool last_blank=true; int rc=0;
    while ((n=getline(&line,&cap,in))>=0) {
        while (n>0 && (line[n-1]=='\r'||line[n-1]=='\n')) line[--n]='\0';
        char *s=line; while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) { if (!last_blank) { fputc('\n',out); last_blank=true; } continue; }
        if (strstr(s,"-->")) continue;
        bool digits=true; for (char *p=s;*p;p++) if (!isdigit((unsigned char)*p)) { digits=false; break; }
        if (digits) continue;
        if (fputs(s,out)==EOF || fputc('\n',out)==EOF) { rc=-1; break; } last_blank=false;
    }
    free(line); if (fclose(in)!=0) rc=-1; if (fclose(out)!=0) rc=-1; return rc;
}
static int update_file_text(sqlite3_int64 id,const char *column,const char *value) {
    char *sql=str_printf("UPDATE files SET %s=?2 WHERE file_id=?1",column); sqlite3_stmt *st=NULL; int rc=-1;
    if (db_prepare(sql,&st)==0) { sqlite3_bind_int64(st,1,id); if (value && *value) sqlite3_bind_text(st,2,value,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,2); rc=db_step_done(st); sqlite3_finalize(st); }
    free(sql); return rc;
}
static int update_file_int(sqlite3_int64 id,const char *column,int value) {
    char *sql=str_printf("UPDATE files SET %s=?2 WHERE file_id=?1",column); sqlite3_stmt *st=NULL; int rc=-1;
    if (db_prepare(sql,&st)==0) { sqlite3_bind_int64(st,1,id); sqlite3_bind_int(st,2,value); rc=db_step_done(st); sqlite3_finalize(st); }
    free(sql); return rc;
}
static int update_run_defaults(const char *uid,const char *artist,const char *album) {
    sqlite3_stmt *st=NULL; if (db_prepare("UPDATE runs SET default_artist=?2,default_album=?3 WHERE run_uid=?1",&st)) return -1;
    sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,artist?artist:"",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,album?album:"",-1,SQLITE_TRANSIENT); int rc=db_step_done(st); sqlite3_finalize(st); return rc;
}
typedef enum {
    META_TITLE = 0,
    META_ARTIST,
    META_ALBUM,
    META_LYRICS,
    META_IMAGE
} MetadataField;

static const char *metadata_field_name(MetadataField field) {
    switch (field) {
        case META_TITLE: return "title";
        case META_ARTIST: return "artist";
        case META_ALBUM: return "album";
        case META_LYRICS: return "lyrics";
        case META_IMAGE: return "image";
    }
    return "unknown";
}

static int metadata_field_from_name(const char *name, MetadataField *field) {
    if (!name || !field) return -1;
    if (!strcmp(name, "title")) *field = META_TITLE;
    else if (!strcmp(name, "artist")) *field = META_ARTIST;
    else if (!strcmp(name, "album")) *field = META_ALBUM;
    else if (!strcmp(name, "lyrics")) *field = META_LYRICS;
    else if (!strcmp(name, "image")) *field = META_IMAGE;
    else return -1;
    return 0;
}

static int metadata_history_latest(const char *uid, sqlite3_int64 *cursor) {
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT COALESCE(MAX(seq),0) FROM metadata_history WHERE run_uid=?1", &st) != 0) return -1;
    sqlite3_bind_text(st, 1, uid, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) *cursor = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

static int metadata_history_record(const char *uid, sqlite3_int64 file_id,
                                   MetadataField field, sqlite3_int64 *cursor) {
    sqlite3_stmt *st = NULL;
    if (db_prepare("INSERT INTO metadata_history(run_uid,file_id,field) VALUES(?1,?2,?3)", &st) != 0) return -1;
    sqlite3_bind_text(st, 1, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, file_id);
    sqlite3_bind_text(st, 3, metadata_field_name(field), -1, SQLITE_TRANSIENT);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc == 0) *cursor = sqlite3_last_insert_rowid(g_db);
    return rc;
}

/* Return 0 with a prior prompt, 1 when there is no prior prompt, or -1 on DB error. */
static int metadata_history_previous(const char *uid, sqlite3_int64 cursor,
                                     sqlite3_int64 *seq_out, sqlite3_int64 *file_id_out,
                                     MetadataField *field_out) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT seq,file_id,field FROM metadata_history WHERE run_uid=?1 AND seq<=?2 ORDER BY seq DESC LIMIT 1";
    if (db_prepare(sql, &st) != 0) return -1;
    sqlite3_bind_text(st, 1, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, cursor);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return rc == SQLITE_DONE ? 1 : -1; }
    *seq_out = sqlite3_column_int64(st, 0);
    *file_id_out = sqlite3_column_int64(st, 1);
    const unsigned char *name = sqlite3_column_text(st, 2);
    int parsed = metadata_field_from_name((const char *)name, field_out);
    sqlite3_finalize(st);
    return parsed == 0 ? 0 : -1;
}

/* Prompts once and stores the chosen metadata. Return 0=accepted, 1=Escape, -1=EOF/error. */
static int prompt_and_save_metadata_field(App *a, RunInfo *r, Options *o,
                                          sqlite3_int64 id, MetadataField field,
                                          char **last_artist, char **last_album) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT path,COALESCE(title,''),COALESCE(artist,''),COALESCE(album,''),COALESCE(lyrics_source,''),COALESCE(image_path,'') FROM files WHERE file_id=?1";
    if (db_prepare(sql, &st) != 0) return -1;
    sqlite3_bind_int64(st, 1, id);
    int sr = sqlite3_step(st);
    if (sr != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    char *path = xstrdup((const char *)sqlite3_column_text(st, 0));
    char *title = xstrdup((const char *)sqlite3_column_text(st, 1));
    char *artist = xstrdup((const char *)sqlite3_column_text(st, 2));
    char *album = xstrdup((const char *)sqlite3_column_text(st, 3));
    char *lyrics = xstrdup((const char *)sqlite3_column_text(st, 4));
    char *image = xstrdup((const char *)sqlite3_column_text(st, 5));
    sqlite3_finalize(st);

    char *label = NULL;
    char *value = NULL;
    int result = 0;
    switch (field) {
        case META_TITLE: {
            char *suggested = title_from_path(path);
            char *default_title = xstrdup(*title ? title : suggested);
            label = str_printf("Song title for %s [%s] (Enter accepts; Esc goes back): ", path, *default_title ? default_title : "Untitled");
            value = read_line_prompt(label, default_title, false);
            if (!value) result = -1;
            else if (prompt_is_back(value)) result = 1;
            else {
                trim_inplace(value);
                if (!*value) { free(value); value = xstrdup(default_title); }
                if (update_file_text(id, "title", value) != 0) result = -1;
            }
            free(suggested); free(default_title);
            break;
        }
        case META_ARTIST: {
            const char *prefill = *artist ? artist : *last_artist;
            label = str_printf("Artist for %s [%s] (Esc goes back): ", path, *prefill ? prefill : "(none)");
            value = read_line_prompt(label, prefill, false);
            if (!value) { result = -1; break; }
            if (prompt_is_back(value)) { result = 1; break; }
            trim_inplace(value);
            if (*value) { free(*last_artist); *last_artist = xstrdup(value); }
            if (update_file_text(id, "artist", *last_artist) != 0 || setting_set("default_artist", *last_artist) != 0) result = -1;
            break;
        }
        case META_ALBUM: {
            const char *prefill = *album ? album : *last_album;
            label = str_printf("Album for %s [%s] (Esc goes back): ", path, *prefill ? prefill : "(none)");
            value = read_line_prompt(label, prefill, false);
            if (!value) { result = -1; break; }
            if (prompt_is_back(value)) { result = 1; break; }
            trim_inplace(value);
            if (*value) { free(*last_album); *last_album = xstrdup(value); }
            if (update_file_text(id, "album", *last_album) != 0 || setting_set("default_album", *last_album) != 0) result = -1;
            break;
        }
        case META_LYRICS: {
            label = str_printf("Lyrics for %s (optional; Enter/'none'/'skip' if absent; Esc goes back): ", path);
            value = prompt_path_value(label, true);
            if (!value) { result = -1; break; }
            if (prompt_is_back(value)) { result = 1; break; }
            if (*value && !looks_like_lyrics(value)) {
                log_warn("Lyrics must be .txt, .lrc, or .srt; skipping lyrics for this track.");
                free(value); value = xstrdup("");
            }
            if (update_file_text(id, "lyrics_source", value) != 0) { result = -1; break; }
            if (*value && looks_like_srt(value)) {
                char *cache_dir = path_join(a->data_dir, "lyrics-cache");
                char *per_run = path_join(cache_dir, r->uid);
                char *prepared = str_printf("%s/%lld.txt", per_run, (long long)id);
                if (extract_srt_text(value, prepared) != 0) {
                    log_warn("Could not preprocess SRT %s; lyrics will be skipped.", value);
                    update_file_text(id, "lyrics_path", "");
                } else {
                    update_file_text(id, "lyrics_path", prepared);
                }
                free(prepared); free(per_run); free(cache_dir);
            } else if (update_file_text(id, "lyrics_path", value) != 0) {
                result = -1; break;
            }
            if (update_file_int(id, "lyrics_prompted", 1) != 0) result = -1;
            break;
        }
        case META_IMAGE: {
            label = str_printf("Cover image path for %s (Enter skips; MP3 output only; Esc goes back): ", path);
            value = prompt_path_value(label, true);
            if (!value) { result = -1; break; }
            if (prompt_is_back(value)) { result = 1; break; }
            if (*value && strcmp(r->format, "mp3") != 0) {
                log_warn("Cover embedding is currently supported only for MP3 output; image not saved.");
                free(value); value = xstrdup("");
            }
            if (update_file_text(id, "image_path", value) != 0) result = -1;
            break;
        }
    }
    free(label); free(value); free(path); free(title); free(artist); free(album); free(lyrics); free(image);
    memory_trim();
    (void)o;
    return result;
}

/* Revisit an earlier saved prompt. The cursor temporarily moves behind that entry, so
 * Escape in the revisited prompt can continue walking backwards; after an answer the
 * cursor is restored and the currently active prompt resumes. */
static int navigate_metadata_back(App *a, RunInfo *r, Options *o, sqlite3_int64 *cursor,
                                  char **last_artist, char **last_album, unsigned depth) {
    if (depth > 128) { log_warn("Back-navigation depth limit reached."); return 1; }
    sqlite3_int64 seq = 0, file_id = 0;
    MetadataField field;
    int found = metadata_history_previous(r->uid, *cursor, &seq, &file_id, &field);
    if (found < 0) return -1;
    if (found > 0) { log_warn("There is no earlier metadata entry to return to."); return 1; }
    sqlite3_int64 saved_cursor = *cursor;
    *cursor = seq - 1;
    for (;;) {
        int rc = prompt_and_save_metadata_field(a, r, o, file_id, field, last_artist, last_album);
        if (rc == 0) break;
        if (rc < 0) { *cursor = saved_cursor; return -1; }
        int back_rc = navigate_metadata_back(a, r, o, cursor, last_artist, last_album, depth + 1);
        if (back_rc < 0) { *cursor = saved_cursor; return -1; }
    }
    *cursor = seq;
    log_info("Returned to the previous %s entry; continuing the current prompt.", metadata_field_name(field));
    return 0;
}

static int prompt_and_record_metadata_field(App *a, RunInfo *r, Options *o,
                                            sqlite3_int64 id, MetadataField field,
                                            sqlite3_int64 *history_cursor,
                                            char **last_artist, char **last_album) {
    for (;;) {
        int rc = prompt_and_save_metadata_field(a, r, o, id, field, last_artist, last_album);
        if (rc == 0) return metadata_history_record(r->uid, id, field, history_cursor);
        if (rc < 0) return -1;
        int back_rc = navigate_metadata_back(a, r, o, history_cursor, last_artist, last_album, 0);
        if (back_rc < 0) return -1;
    }
}

static int prepare_metadata(RunInfo *r, App *a, Options *o) {
    char *last_artist = setting_get("default_artist"), *last_album = setting_get("default_album");
    if (r->default_artist && *r->default_artist) { free(last_artist); last_artist = xstrdup(r->default_artist); }
    if (r->default_album && *r->default_album) { free(last_album); last_album = xstrdup(r->default_album); }
    sqlite3_int64 history_cursor = 0;
    if (metadata_history_latest(r->uid, &history_cursor) != 0) { free(last_artist); free(last_album); return -1; }
    sqlite3_int64 last_id = 0;
    for (;;) {
        if (g_interrupted) { free(last_artist); free(last_album); return -1; }
        sqlite3_stmt *st = NULL;
        const char *sql = "SELECT file_id,path,COALESCE(status,''),COALESCE(artist,''),COALESCE(album,''),COALESCE(title,''),COALESCE(lyrics_source,''),COALESCE(image_path,''),COALESCE(lyrics_prompted,0) FROM files WHERE run_uid=?1 AND duplicate_of IS NULL AND file_id>?2 ORDER BY file_id LIMIT 1";
        if (db_prepare(sql, &st)) { free(last_artist); free(last_album); return -1; }
        sqlite3_bind_text(st, 1, r->uid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, last_id);
        int sr = sqlite3_step(st);
        if (sr != SQLITE_ROW) { sqlite3_finalize(st); break; }
        sqlite3_int64 id = sqlite3_column_int64(st, 0); last_id = id;
        char *path = xstrdup((const char *)sqlite3_column_text(st, 1));
        char *status = xstrdup((const char *)sqlite3_column_text(st, 2));
        char *artist = xstrdup((const char *)sqlite3_column_text(st, 3));
        char *album = xstrdup((const char *)sqlite3_column_text(st, 4));
        char *title = xstrdup((const char *)sqlite3_column_text(st, 5));
        char *lyrics = xstrdup((const char *)sqlite3_column_text(st, 6));
        char *image = xstrdup((const char *)sqlite3_column_text(st, 7));
        bool lyrics_prompted = sqlite3_column_int(st, 8) != 0;
        sqlite3_finalize(st);
        if (!strcmp(status, "DONE")) { free(path); free(status); free(artist); free(album); free(title); free(lyrics); free(image); continue; }

        MetadataField fields[5]; size_t field_count = 0;
        if (o->ask_title && (o->reenter_metadata || !*title)) fields[field_count++] = META_TITLE;
        if (o->ask_artist && (o->reenter_metadata || !*artist)) fields[field_count++] = META_ARTIST;
        if (o->ask_album && (o->reenter_metadata || !*album)) fields[field_count++] = META_ALBUM;
        if (o->ask_lyrics && (o->reenter_metadata || (!lyrics_prompted && !*lyrics))) fields[field_count++] = META_LYRICS;
        if (o->ask_image && (o->reenter_metadata || !*image)) fields[field_count++] = META_IMAGE;
        for (size_t fi = 0; fi < field_count; fi++) {
            if (prompt_and_record_metadata_field(a, r, o, id, fields[fi], &history_cursor, &last_artist, &last_album) != 0) {
                free(path); free(status); free(artist); free(album); free(title); free(lyrics); free(image);
                free(last_artist); free(last_album); return -1;
            }
        }
        free(path); free(status); free(artist); free(album); free(title); free(lyrics); free(image);
        memory_trim();
    }
    update_run_defaults(r->uid, last_artist, last_album);
    free(last_artist); free(last_album);
    return 0;
}

static char *ffmpeg_audio_options(const char *format) {
    if (!strcmp(format,"mp3")) return xstrdup("-c:a libmp3lame -q:a 2");
    if (!strcmp(format,"flac")) return xstrdup("-c:a flac -compression_level 5");
    if (!strcmp(format,"opus")) return xstrdup("-c:a libopus -b:a 160k");
    if (!strcmp(format,"aac")) return xstrdup("-c:a aac -b:a 256k -f ipod");
    return xstrdup("-c:a pcm_s16le");
}
static char *make_ffmpeg_cmd(App *a, RunInfo *r, sqlite3_int64 id, const char *input,const char *output,const char *artist,const char *album,const char *title,const char *lyrics,const char *image) {
    if (path_has_linebreak(input)||path_has_linebreak(output)||path_has_linebreak(a->ffmpeg_path)) return NULL;
    char *qff=shell_quote(a->ffmpeg_path), *qin=shell_quote(input), *qout=shell_quote(output);
    StrBuf b; sb_init(&b); sb_addf(&b,"%s -hide_banner -nostdin -n -i %s",qff,qin);
    if (image && *image) { char *qi=shell_quote(image); sb_addf(&b," -i %s",qi); free(qi); }
    sb_add(&b," -map 0:a:0");
    if (image && *image) sb_add(&b," -map 1:v:0 -c:v copy -disposition:v:0 attached_pic"); else sb_add(&b," -vn");
    sb_add(&b," -map_metadata 0");
    if (artist && *artist) { char *m=str_printf("artist=%s",artist), *q=shell_quote(m); sb_addf(&b," -metadata %s",q); free(m);free(q); }
    if (album && *album) { char *m=str_printf("album=%s",album), *q=shell_quote(m); sb_addf(&b," -metadata %s",q); free(m);free(q); }
    if (title && *title) { char *m=str_printf("title=%s",title), *q=shell_quote(m); sb_addf(&b," -metadata %s",q); free(m);free(q); }
    if (lyrics && *lyrics) { char *ql=shell_quote(lyrics); sb_addf(&b," -metadata \"lyrics=$(cat -- %s)\"",ql); free(ql); }
    char *opts=ffmpeg_audio_options(r->format); sb_addf(&b," %s %s",opts,qout); free(opts); free(qff);free(qin);free(qout); (void)id; return b.data;
}
static char *make_file_action(App *a, RunInfo *r, sqlite3_int64 id,const char *input,const char *output,const char *artist,const char *album,const char *title,const char *lyrics,const char *image) {
    char *ff=make_ffmpeg_cmd(a,r,id,input,output,artist,album,title,lyrics,image); if (!ff) return NULL;
    char *qout=shell_quote(output), *dir=path_dirname_dup(output), *qdir=shell_quote(dir), *qbin=shell_quote(a->binary_path), *qdb=shell_quote(a->db_path), *quid=shell_quote(r->uid);
    char *qmsg=shell_quote(output);
    char *action=str_printf("if [ -e %s ]; then printf 'SKIP existing output: %%s\\n' %s; else mkdir -p %s && %s && %s --internal-mark-complete %s %s %lld; fi",qout,qmsg,qdir,ff,qbin,qdb,quid,(long long)id);
    free(ff);free(qout);free(dir);free(qdir);free(qbin);free(qdb);free(quid);free(qmsg); return action;
}
static int sha256_path(const char *path,char out[65]) { return hash_file_stream(path,out); }
static int sign_script(App *a,const char *script,const char *sig) {
    char *args[]={"--batch","--yes","--pinentry-mode","loopback","--passphrase","","--local-user",a->fingerprint,"--armor","--detach-sign","--output",(char*)sig,(char*)script,NULL};
    return run_gpg(a,args,true);
}
static int save_generated_plan(const char *uid,const char *line,const char *script,const char *sig,const char *hash) {
    sqlite3_stmt *st=NULL; const char *sql="UPDATE runs SET command_text=?2,generated_script=?3,script_signature=?4,script_sha256=?5,status='PLANNED' WHERE run_uid=?1";
    if (db_prepare(sql,&st)) return -1;
    sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT);
    if (line && *line) sqlite3_bind_text(st,2,line,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,2);
    if (script && *script) sqlite3_bind_text(st,3,script,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,3);
    if (sig && *sig) sqlite3_bind_text(st,4,sig,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,4);
    if (hash && *hash) sqlite3_bind_text(st,5,hash,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,5);
    int rc=db_step_done(st); sqlite3_finalize(st); return rc;
}
static int save_script_record(const char *uid,const char *script,const char *sig,const char *hash,const char *fp) {
    sqlite3_stmt *st=NULL; if (db_prepare("INSERT INTO scripts(run_uid,path,signature_path,sha256,fingerprint,created_at) VALUES(?1,?2,?3,?4,?5,?6)",&st)) return -1;
    char *now=utc_now(); sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,script,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,sig,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,hash,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,fp,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,now,-1,SQLITE_TRANSIENT);
    int rc=db_step_done(st); sqlite3_finalize(st); free(now); return rc;
}
static int copy_clipboard(const char *text) {
#ifdef __APPLE__
    const char *candidates[][3]={{"pbcopy",NULL,NULL},{"wl-copy",NULL,NULL},{"xclip","-selection","clipboard"},{"xsel","--clipboard","--input"}};
#else
    const char *candidates[][3]={{"wl-copy",NULL,NULL},{"xclip","-selection","clipboard"},{"xsel","--clipboard","--input"},{"pbcopy",NULL,NULL}};
#endif
    for (size_t i=0;i<4;i++) {
        char *exe=find_executable(candidates[i][0]); if (!exe) continue;
        int fds[2]; if (pipe(fds)!=0) { free(exe); continue; }
        pid_t pid=fork(); if (pid==0) {
            dup2(fds[0],STDIN_FILENO); close(fds[0]);close(fds[1]);
            if (strcmp(candidates[i][0],"xclip")==0) { char *av[]={exe,"-selection","clipboard","-in",NULL}; execv(exe,av); }
            else if (strcmp(candidates[i][0],"xsel")==0) { char *av[]={exe,"--clipboard","--input",NULL}; execv(exe,av); }
            else { char *av[]={exe,NULL}; execv(exe,av); }
            _exit(127);
        }
        close(fds[0]); size_t left=strlen(text); const char *p=text; while (left) { ssize_t n=write(fds[1],p,left); if (n<0) { if(errno==EINTR)continue; break; } p+=n;left-=(size_t)n; }
        close(fds[1]); int status=0; waitpid(pid,&status,0); free(exe);
        if (left==0 && WIFEXITED(status) && WEXITSTATUS(status)==0) return 0;
    }
    return -1;
}
static int copy_file_binary(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    char *dir = path_dirname_dup(dst);
    if (mkdir_p(dir, 0700) != 0) { free(dir); fclose(in); return -1; }
    free(dir);
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    unsigned char buffer[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) { rc = -1; break; }
    }
    if (ferror(in)) rc = -1;
    if (fclose(in) != 0) rc = -1;
    if (fclose(out) != 0) rc = -1;
    if (rc == 0) (void)chmod(dst, 0600);
    return rc;
}

/* PowerShell literals are represented as Base64 UTF-8 expressions so arbitrary
 * punctuation, quotes, Unicode, and newlines cannot inject code into the helper. */
static char *powershell_b64_expr(const char *value) {
    const char *s = value ? value : "";
    size_t len = strlen(s);
    if (len > (size_t)INT_MAX) return NULL;
    size_t cap = 4 * ((len + 2) / 3) + 1;
    unsigned char *encoded = xmalloc(cap);
    int n = EVP_EncodeBlock(encoded, (const unsigned char *)s, (int)len);
    if (n < 0) { free(encoded); return NULL; }
    encoded[n] = '\0';
    char *expr = str_printf("[System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('%s'))", (char *)encoded);
    free(encoded);
    return expr;
}

static char *windows_slashes(const char *path) {
    char *copy = xstrdup(path ? path : "");
    for (char *p = copy; *p; p++) if (*p == '/') *p = '\\';
    return copy;
}

/* A Linux filename can be valid on Linux but impossible to represent on Windows.
 * Reject those paths instead of emitting a batch package that silently cannot work. */
static bool windows_relative_path_valid(const char *path) {
    if (!path || !*path || path[0] == '/' || path[0] == '\\') return false;
    const char *segment = path;
    for (const char *p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*' || c == '\\') return false;
        if (c == '/' || c == '\0') {
            size_t n = (size_t)(p - segment);
            if (n == 0) return false;
            if (segment[n-1] == ' ' || segment[n-1] == '.') return false;
            char *name = xmalloc(n + 1);
            memcpy(name, segment, n); name[n] = '\0';
            char *dot = strchr(name, '.'); if (dot) *dot = '\0';
            bool reserved = !strcasecmp(name, "CON") || !strcasecmp(name, "PRN") ||
                            !strcasecmp(name, "AUX") || !strcasecmp(name, "NUL");
            if (strlen(name) == 4) {
                char pfx[4]; memcpy(pfx, name, 3); pfx[3] = '\0';
                if ((!strcasecmp(pfx, "COM") || !strcasecmp(pfx, "LPT")) && name[3] >= '1' && name[3] <= '9') reserved = true;
            }
            free(name);
            if (reserved) return false;
            if (c == '\0') break;
            segment = p + 1;
        }
        if (c == '\0') break;
    }
    return true;
}

static int write_ps_value(FILE *f, const char *name, const char *value) {
    char *expr = powershell_b64_expr(value);
    if (!expr) return -1;
    int rc = fprintf(f, "  %s = %s\n", name, expr) < 0 ? -1 : 0;
    free(expr);
    return rc;
}

static int write_ps_nullable_value(FILE *f, const char *name, const char *value) {
    if (!value || !*value) return fprintf(f, "  %s = $null\n", name) < 0 ? -1 : 0;
    return write_ps_value(f, name, value);
}

static int plan_and_emit_windows_batch(App *a, RunInfo *r, Options *o) {
    (void)o;
    char *script_dir = path_join(a->data_dir, "scripts");
    if (mkdir_p(script_dir, 0700) != 0) { free(script_dir); return -1; }
    char *stem = str_printf("recode-RAMIF-%s", r->uid);
    char *script_path = str_printf("%s/%s.bat", script_dir, stem);
    char *ps_path = str_printf("%s/%s.ps1", script_dir, stem);
    char *sig_path = str_printf("%s.asc", script_path);
    char *ps_sig_path = str_printf("%s.asc", ps_path);
    char *assets_name = str_printf("%s-assets", stem);
    char *assets_dir = path_join(script_dir, assets_name);
    char *tmp_bat = str_printf("%s/.%s.tmp.bat", script_dir, stem);
    char *tmp_ps = str_printf("%s/.%s.tmp.ps1", script_dir, stem);
    FILE *ps = NULL, *bat = NULL;
    bool assets_created = false;
    uint64_t actions_n = 0;
    sqlite3_int64 last_id = 0;
    int failed = 0;

    if (mkdir_p(assets_dir, 0700) != 0) {
        log_error("Cannot create Windows batch asset directory: %s", strerror(errno)); failed = 1; goto cleanup;
    }
    assets_created = true;
    ps = fopen(tmp_ps, "w");
    if (!ps) { log_error("Cannot create PowerShell worker: %s", strerror(errno)); failed = 1; goto cleanup; }
    (void)chmod(tmp_ps, 0600);
    fprintf(ps,
        "$ErrorActionPreference = 'Stop'\n"
        "# RAMIF Windows conversion worker; generated by %s %s\n"
        "# Run ID: %s\n"
        "$jobs = @()\n\n", APP_NAME, APP_VERSION, r->uid);

    for (;;) {
        sqlite3_stmt *st = NULL;
        const char *sql = "SELECT file_id,relpath,COALESCE(artist,''),COALESCE(album,''),COALESCE(title,''),COALESCE(lyrics_path,''),COALESCE(image_path,''),COALESCE(status,'') FROM files WHERE run_uid=?1 AND duplicate_of IS NULL AND file_id>?2 ORDER BY file_id LIMIT 1";
        if (db_prepare(sql, &st) != 0) { failed = 1; break; }
        sqlite3_bind_text(st, 1, r->uid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, last_id);
        int sr = sqlite3_step(st);
        if (sr != SQLITE_ROW) { sqlite3_finalize(st); break; }
        sqlite3_int64 id = sqlite3_column_int64(st, 0); last_id = id;
        char *rel = xstrdup((const char *)sqlite3_column_text(st, 1));
        char *artist = xstrdup((const char *)sqlite3_column_text(st, 2));
        char *album = xstrdup((const char *)sqlite3_column_text(st, 3));
        char *title = xstrdup((const char *)sqlite3_column_text(st, 4));
        char *lyrics = xstrdup((const char *)sqlite3_column_text(st, 5));
        char *image = xstrdup((const char *)sqlite3_column_text(st, 6));
        char *status = xstrdup((const char *)sqlite3_column_text(st, 7));
        sqlite3_finalize(st);
        if (!strcmp(status, "DONE")) { free(rel); free(artist); free(album); free(title); free(lyrics); free(image); free(status); continue; }

        char *out_rel_raw = get_file_output_path("", rel, r->format);
        if (!windows_relative_path_valid(rel) || !windows_relative_path_valid(out_rel_raw)) {
            log_warn("Cannot represent this Linux path in a Windows batch script; skipping: %s", rel);
            update_file_text(id, "error", "path is not representable on Windows");
            free(rel); free(out_rel_raw); free(artist); free(album); free(title); free(lyrics); free(image); free(status); continue;
        }
        char *in_rel = windows_slashes(rel);
        char *out_rel = windows_slashes(out_rel_raw);
        char *output_full = get_file_output_path(r->output, rel, r->format);
        update_file_text(id, "output_path", output_full);

        char *lyrics_asset = NULL, *lyrics_hash = NULL, *image_asset = NULL, *image_hash = NULL;
        if (lyrics && *lyrics) {
            char *lyric_dir = path_join(assets_dir, "lyrics");
            if (mkdir_p(lyric_dir, 0700) != 0) { free(lyric_dir); failed = 1; }
            else {
                char *lyric_name = str_printf("%lld.txt", (long long)id);
                char *lyric_dst = path_join(lyric_dir, lyric_name);
                if (copy_file_binary(lyrics, lyric_dst) != 0) {
                    log_error("Could not bundle lyrics for Windows batch file %lld: %s", (long long)id, lyrics); failed = 1;
                } else {
                    char hash[65];
                    if (sha256_path(lyric_dst, hash) != 0) failed = 1;
                    else {
                        lyrics_hash = xstrdup(hash);
                        lyrics_asset = str_printf("%s\\lyrics\\%s", assets_name, lyric_name);
                    }
                }
                free(lyric_dst); free(lyric_name);
            }
            free(lyric_dir);
        }
        if (!failed && image && *image) {
            char *image_dir = path_join(assets_dir, "images");
            if (mkdir_p(image_dir, 0700) != 0) { free(image_dir); failed = 1; }
            else {
                char *ext = extension_lower(image); if (!*ext || strlen(ext) > 12) { free(ext); ext = xstrdup(".img"); }
                char *image_name = str_printf("%lld%s", (long long)id, ext);
                char *image_dst = path_join(image_dir, image_name);
                if (copy_file_binary(image, image_dst) != 0) {
                    log_error("Could not bundle cover image for Windows batch file %lld: %s", (long long)id, image); failed = 1;
                } else {
                    char hash[65];
                    if (sha256_path(image_dst, hash) != 0) failed = 1;
                    else {
                        image_hash = xstrdup(hash);
                        image_asset = str_printf("%s\\images\\%s", assets_name, image_name);
                    }
                }
                free(image_dst); free(image_name); free(ext);
            }
            free(image_dir);
        }
        if (failed) {
            free(rel); free(out_rel_raw); free(in_rel); free(out_rel); free(output_full); free(artist); free(album); free(title); free(lyrics); free(image); free(status); free(lyrics_asset); free(lyrics_hash); free(image_asset); free(image_hash); break;
        }

        if (fprintf(ps, "$jobs += [pscustomobject]@{\n  Id = %lld\n", (long long)id) < 0 ||
            write_ps_value(ps, "Input", in_rel) != 0 ||
            write_ps_value(ps, "Output", out_rel) != 0 ||
            write_ps_value(ps, "Format", r->format) != 0 ||
            write_ps_value(ps, "Title", title) != 0 ||
            write_ps_value(ps, "Artist", artist) != 0 ||
            write_ps_value(ps, "Album", album) != 0 ||
            write_ps_nullable_value(ps, "Lyrics", lyrics_asset) != 0 ||
            write_ps_nullable_value(ps, "LyricsHash", lyrics_hash) != 0 ||
            write_ps_nullable_value(ps, "Image", image_asset) != 0 ||
            write_ps_nullable_value(ps, "ImageHash", image_hash) != 0 ||
            fputs("}\n\n", ps) == EOF) {
            log_error("Failed writing PowerShell conversion job."); failed = 1;
        }
        if (!failed) {
            sqlite3_stmt *u = NULL;
            if (!db_prepare("UPDATE files SET status='PLANNED' WHERE file_id=?1 AND status!='DONE'", &u)) {
                sqlite3_bind_int64(u, 1, id); db_step_done(u); sqlite3_finalize(u);
            }
            actions_n++;
        }
        free(rel); free(out_rel_raw); free(in_rel); free(out_rel); free(output_full); free(artist); free(album); free(title); free(lyrics); free(image); free(status); free(lyrics_asset); free(lyrics_hash); free(image_asset); free(image_hash);
        memory_trim();
        if (failed) break;
    }
    if (failed) goto cleanup;
    if (actions_n == 0) {
        if (ps) { fclose(ps); ps = NULL; }
        unlink(tmp_ps);
        unlink(tmp_bat);
        if (assets_created) (void)rmdir(assets_dir);
        log_warn("No Windows-compatible unfinished tracks remain for this run.");
        update_run_status(r->uid, "DONE");
        goto cleanup;
    }
    if (fputs(
        "\n$sourceRoot = $env:RAMIF_SOURCE_ROOT\n"
        "$outputRoot = $env:RAMIF_OUTPUT_ROOT\n"
        "if ([string]::IsNullOrWhiteSpace($sourceRoot) -or [string]::IsNullOrWhiteSpace($outputRoot)) { throw 'The batch launcher did not provide source/output roots.' }\n"
        "$sourceRoot = [System.IO.Path]::GetFullPath($sourceRoot)\n"
        "$outputRoot = [System.IO.Path]::GetFullPath($outputRoot)\n"
        "$ffmpegCommand = Get-Command 'ffmpeg.exe' -ErrorAction SilentlyContinue\n"
        "if (-not $ffmpegCommand) { $ffmpegCommand = Get-Command 'ffmpeg' -ErrorAction SilentlyContinue }\n"
        "if (-not $ffmpegCommand) { throw 'FFmpeg was not found. Install FFmpeg for Windows and add ffmpeg.exe to PATH.' }\n"
        "foreach ($job in $jobs) {\n"
        "  $inputPath = Join-Path $sourceRoot $job.Input\n"
        "  $outputPath = Join-Path $outputRoot $job.Output\n"
        "  if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { Write-Warning ('Missing input, skipping: ' + $inputPath); continue }\n"
        "  if (Test-Path -LiteralPath $outputPath) { Write-Host ('SKIP existing output: ' + $outputPath); continue }\n"
        "  $outputDirectory = Split-Path -Parent $outputPath\n"
        "  New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null\n"
        "  $ffArgs = [System.Collections.Generic.List[string]]::new()\n"
        "  $ffArgs.AddRange([string[]]@('-hide_banner','-nostdin','-n','-i',$inputPath))\n"
        "  $imagePath = $null\n"
        "  if ($job.Image) {\n"
        "    $imagePath = Join-Path $PSScriptRoot $job.Image\n"
        "    if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) { throw ('Missing bundled cover image: ' + $imagePath) }\n"
        "    $actual = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()\n"
        "    if ($actual -ne $job.ImageHash) { throw ('Cover image integrity check failed: ' + $imagePath) }\n"
        "    $ffArgs.AddRange([string[]]@('-i',$imagePath))\n"
        "  }\n"
        "  $ffArgs.AddRange([string[]]@('-map','0:a:0'))\n"
        "  if ($imagePath) { $ffArgs.AddRange([string[]]@('-map','1:v:0','-c:v','copy','-disposition:v:0','attached_pic')) } else { $ffArgs.Add('-vn') }\n"
        "  $ffArgs.AddRange([string[]]@('-map_metadata','0'))\n"
        "  if ($job.Title) { $ffArgs.Add('-metadata'); $ffArgs.Add(('title=' + $job.Title)) }\n"
        "  if ($job.Artist) { $ffArgs.Add('-metadata'); $ffArgs.Add(('artist=' + $job.Artist)) }\n"
        "  if ($job.Album) { $ffArgs.Add('-metadata'); $ffArgs.Add(('album=' + $job.Album)) }\n"
        "  if ($job.Lyrics) {\n"
        "    $lyricsPath = Join-Path $PSScriptRoot $job.Lyrics\n"
        "    if (-not (Test-Path -LiteralPath $lyricsPath -PathType Leaf)) { throw ('Missing bundled lyrics: ' + $lyricsPath) }\n"
        "    $actual = (Get-FileHash -LiteralPath $lyricsPath -Algorithm SHA256).Hash.ToLowerInvariant()\n"
        "    if ($actual -ne $job.LyricsHash) { throw ('Lyrics integrity check failed: ' + $lyricsPath) }\n"
        "    $lyricsText = [System.IO.File]::ReadAllText($lyricsPath).TrimEnd([char[]]\"`r`n\")\n"
        "    if ($lyricsText.Length -gt 0) { $ffArgs.Add('-metadata'); $ffArgs.Add(('lyrics=' + $lyricsText)) }\n"
        "  }\n"
        "  switch ($job.Format) {\n"
        "    'mp3'  { $ffArgs.AddRange([string[]]@('-c:a','libmp3lame','-q:a','2')) }\n"
        "    'flac' { $ffArgs.AddRange([string[]]@('-c:a','flac','-compression_level','5')) }\n"
        "    'opus' { $ffArgs.AddRange([string[]]@('-c:a','libopus','-b:a','160k')) }\n"
        "    'aac'  { $ffArgs.AddRange([string[]]@('-c:a','aac','-b:a','256k','-f','ipod')) }\n"
        "    default { $ffArgs.AddRange([string[]]@('-c:a','pcm_s16le')) }\n"
        "  }\n"
        "  $ffArgs.Add($outputPath)\n"
        "  Write-Host ('CONVERT: ' + $inputPath + ' -> ' + $outputPath)\n"
        "  $nativeArgs = $ffArgs.ToArray()\n"
        "  & $ffmpegCommand.Source @nativeArgs\n"
        "  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $outputPath -Force -ErrorAction SilentlyContinue; throw ('FFmpeg failed for: ' + $inputPath) }\n"
        "}\n"
        "Write-Host 'RAMIF Windows batch conversion plan finished.'\n", ps) == EOF) {
        log_error("Failed writing PowerShell worker footer."); failed = 1; goto cleanup;
    }
    if (fclose(ps) != 0) { ps = NULL; failed = 1; goto cleanup; }
    ps = NULL;

    bat = fopen(tmp_bat, "w");
    if (!bat) { log_error("Cannot create Windows batch launcher: %s", strerror(errno)); failed = 1; goto cleanup; }
    (void)chmod(tmp_bat, 0600);
    if (fprintf(bat,
        "@echo off\r\n"
        "setlocal DisableDelayedExpansion\r\n"
        "title RAMIF Windows Batch Converter\r\n"
        "echo.\r\n"
        "echo RAMIF Windows conversion plan - generated by %s %s\r\n"
        "echo This launcher uses the signed PowerShell worker beside this file.\r\n"
        "echo Copy this .bat, its matching .ps1, and the matching -assets folder together.\r\n"
        "echo.\r\n"
        "set \"RAMIF_SOURCE_ROOT=\"\r\n"
        "set /p \"RAMIF_SOURCE_ROOT=Windows folder containing the source music: \"\r\n"
        "if not defined RAMIF_SOURCE_ROOT goto :bad_source\r\n"
        "set \"RAMIF_OUTPUT_ROOT=\"\r\n"
        "set /p \"RAMIF_OUTPUT_ROOT=Windows folder for converted music: \"\r\n"
        "if not defined RAMIF_OUTPUT_ROOT goto :bad_output\r\n"
        "powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File \"%%~dp0%s.ps1\"\r\n"
        "if errorlevel 1 goto :failed\r\n"
        "echo.\r\n"
        "echo RAMIF conversion plan completed. Existing outputs were not overwritten.\r\n"
        "pause\r\n"
        "exit /b 0\r\n"
        ":bad_source\r\n"
        "echo ERROR: A source folder is required.\r\n"
        "pause\r\n"
        "exit /b 2\r\n"
        ":bad_output\r\n"
        "echo ERROR: An output folder is required.\r\n"
        "pause\r\n"
        "exit /b 2\r\n"
        ":failed\r\n"
        "echo ERROR: The conversion worker reported a failure. Read the error above.\r\n"
        "pause\r\n"
        "exit /b 1\r\n", APP_NAME, APP_VERSION, stem) < 0) {
        log_error("Failed writing Windows batch launcher."); failed = 1; goto cleanup;
    }
    if (fclose(bat) != 0) { bat = NULL; failed = 1; goto cleanup; }
    bat = NULL;
    if (rename(tmp_ps, ps_path) != 0 || rename(tmp_bat, script_path) != 0) {
        log_error("Could not finalize Windows batch package: %s", strerror(errno)); failed = 1; goto cleanup;
    }
    (void)chmod(ps_path, 0600); (void)chmod(script_path, 0700);

    char batch_hash[65], ps_hash[65];
    if (sha256_path(script_path, batch_hash) != 0 || sha256_path(ps_path, ps_hash) != 0) {
        log_error("Could not calculate generated Windows script hashes."); failed = 1; goto cleanup;
    }
    if (sign_script(a, script_path, sig_path) != 0) {
        log_error("Windows batch launcher was created but its signature failed."); failed = 1; goto cleanup;
    }
    if (sign_script(a, ps_path, ps_sig_path) != 0) {
        log_error("PowerShell worker was created but its signature failed."); failed = 1; goto cleanup;
    }
    if (save_script_record(r->uid, script_path, sig_path, batch_hash, a->fingerprint) != 0 ||
        save_script_record(r->uid, ps_path, ps_sig_path, ps_hash, a->fingerprint) != 0) {
        log_error("Could not save generated Windows script metadata in SQLite."); failed = 1; goto cleanup;
    }
    save_generated_plan(r->uid, script_path, script_path, sig_path, batch_hash);
    char *plan_display = xstrdup(strrchr(script_path, '/') ? strrchr(script_path, '/') + 1 : script_path);
    printf("\n%sWINDOWS BATCH PACKAGE%s\n  Launcher: %s\n  PowerShell worker: %s\n  Worker signature: %s\n  Assets directory: %s\n  Batch signature: %s\n  Batch SHA-256: %s\n\n",
           g_color ? CYAN : "", g_color ? RESET : "", script_path, ps_path, ps_sig_path, assets_dir, sig_path, batch_hash);
    log_info("Generated a Windows .bat launcher and signed PowerShell worker for %llu track(s).", (unsigned long long)actions_n);
    log_warn("Copy the .bat, .bat.asc, matching .ps1, .ps1.asc, and the entire matching -assets folder to Windows together.");
    log_info("On Windows, install FFmpeg and ensure ffmpeg.exe is on PATH. The launcher asks for Windows source/output roots.");
    if (!o->no_clipboard) {
        if (copy_clipboard(plan_display) == 0) log_info("Copied launcher filename %s to the clipboard.", plan_display);
        else log_warn("Clipboard copy unavailable (install wl-clipboard, xclip, or xsel (Linux), or use pbcopy (macOS)); the launcher filename is printed above.");
    } else log_info("--no-clipboard selected; clipboard was not touched.");
    log_info("Verify both scripts before execution with --verify-script PATH; the .bat signature does not cover its companion files.");
    log_info("Run ID: %s", r->uid);
    log_info("Database: %s", a->db_path);
    log_info("The Windows worker skips existing outputs and never deletes source music.");
    free(plan_display);

cleanup:
    if (ps) fclose(ps);
    if (bat) fclose(bat);
    if (failed) {
        unlink(tmp_bat); unlink(tmp_ps);
        if (assets_created) log_warn("Incomplete Windows batch assets may remain at %s", assets_dir);
    }
    free(script_dir); free(stem); free(script_path); free(ps_path); free(sig_path); free(ps_sig_path);
    free(assets_name); free(assets_dir); free(tmp_bat); free(tmp_ps);
    return failed ? -1 : 0;
}


static int plan_and_emit(App *a, RunInfo *r, Options *o) {
    if (o->windows_batch || r->windows_batch) return plan_and_emit_windows_batch(a, r, o);
    uint64_t actions_n=0; bool too_large=false; sqlite3_int64 last_id=0;
    StrBuf one; sb_init(&one); sb_add(&one,"set -e; ");
    char *script_dir=path_join(a->data_dir,"scripts"); if (mkdir_p(script_dir,0700)!=0) { free(script_dir); sb_free(&one); return -1; }
    char *tmp_path=str_printf("%s/.%s.tmp.sh",script_dir,r->uid); FILE *sf=fopen(tmp_path,"w");
    if (!sf) { log_error("Cannot create temporary script: %s",strerror(errno)); free(script_dir);free(tmp_path);sb_free(&one);return -1; }
    chmod(tmp_path,0700); fprintf(sf,"#!/bin/sh\nset -eu\n# Generated by %s %s\n# Run ID: %s\n# Detached OpenPGP signature is stored beside this file as .asc\n\n",APP_NAME,APP_VERSION,r->uid);
    for (;;) {
        sqlite3_stmt *st=NULL;
        const char *sql="SELECT file_id,path,relpath,COALESCE(artist,''),COALESCE(album,''),COALESCE(title,''),COALESCE(lyrics_path,''),COALESCE(image_path,''),COALESCE(status,'') FROM files WHERE run_uid=?1 AND duplicate_of IS NULL AND file_id>?2 ORDER BY file_id LIMIT 1";
        if (db_prepare(sql,&st)) { fclose(sf);unlink(tmp_path);free(script_dir);free(tmp_path);sb_free(&one);return -1; }
        sqlite3_bind_text(st,1,r->uid,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(st,2,last_id);
        int sr=sqlite3_step(st);
        if (sr!=SQLITE_ROW) { sqlite3_finalize(st); break; }
        sqlite3_int64 id=sqlite3_column_int64(st,0); last_id=id;
        char *path=xstrdup((const char*)sqlite3_column_text(st,1)); char *rel=xstrdup((const char*)sqlite3_column_text(st,2));
        char *artist=xstrdup((const char*)sqlite3_column_text(st,3)), *album=xstrdup((const char*)sqlite3_column_text(st,4));
        char *title=xstrdup((const char*)sqlite3_column_text(st,5)), *lyrics=xstrdup((const char*)sqlite3_column_text(st,6));
        char *image=xstrdup((const char*)sqlite3_column_text(st,7)), *status=xstrdup((const char*)sqlite3_column_text(st,8));
        sqlite3_finalize(st);
        if (!strcmp(status,"DONE")) { free(path);free(rel);free(artist);free(album);free(title);free(lyrics);free(image);free(status);continue; }
        char *out=get_file_output_path(r->output,rel,r->format); update_file_text(id,"output_path",out);
        char *action=make_file_action(a,r,id,path,out,artist,album,title,lyrics,image);
        if (!action) { log_warn("Skipping unsupported path containing a line break: %s",path); update_file_text(id,"error","unsupported newline in path"); free(path);free(rel);free(artist);free(album);free(title);free(lyrics);free(image);free(status);free(out);continue; }
        if (fprintf(sf,"# file-id %lld: %s\n%s\n\n",(long long)id,path,action)<0) { free(action);free(path);free(rel);free(artist);free(album);free(title);free(lyrics);free(image);free(status);free(out);fclose(sf);unlink(tmp_path);free(script_dir);free(tmp_path);sb_free(&one);return -1; }
        if (!too_large) {
            size_t extra=strlen(action)+(actions_n?4:0);
            if (one.len + extra > ONE_LINER_LIMIT) { too_large=true; free(one.data); one.data=NULL; one.len=one.cap=0; }
            else { if (actions_n) sb_add(&one," && "); sb_add(&one,action); }
        }
        actions_n++;
        sqlite3_stmt *u=NULL; if (!db_prepare("UPDATE files SET status='PLANNED' WHERE file_id=?1 AND status!='DONE'",&u)) { sqlite3_bind_int64(u,1,id);db_step_done(u);sqlite3_finalize(u); }
        free(action);free(path);free(rel);free(artist);free(album);free(title);free(lyrics);free(image);free(status);free(out); memory_trim();
    }
    if (fclose(sf)!=0) { log_error("Failed writing script body."); unlink(tmp_path);free(script_dir);free(tmp_path);sb_free(&one);return -1; }
    if (actions_n==0) {
        unlink(tmp_path); update_run_status(r->uid,"DONE"); log_info("No eligible unfinished tracks remain; all files are done or deduplicated."); free(script_dir);free(tmp_path);sb_free(&one);return 0;
    }
    char *plan_display=NULL, *saved_line=NULL, *script_path=NULL, *sig_path=NULL, *filehash=NULL;
    if (!too_large) {
        unlink(tmp_path); saved_line=one.data; one.data=NULL; plan_display=xstrdup(saved_line);
        log_info("Built a %llu-character FFmpeg one-liner for %llu track(s).",(unsigned long long)strlen(saved_line),(unsigned long long)actions_n);
        printf("\n%sFFMPEG ONE-LINER%s\n%s\n\n",g_color?CYAN:"",g_color?RESET:"",saved_line);
        save_generated_plan(r->uid,saved_line,NULL,NULL,NULL);
    } else {
        script_path=str_printf("%s/recode-RAMIF-%s.sh",script_dir,r->uid); sig_path=str_printf("%s.asc",script_path);
        if (rename(tmp_path,script_path)!=0) { log_error("Could not finalize script: %s",strerror(errno)); unlink(tmp_path); free(script_dir);free(tmp_path);free(script_path);free(sig_path);sb_free(&one);return -1; }
        chmod(script_path,0700);
        char hashbuf[65]; if (sha256_path(script_path,hashbuf)!=0) { log_error("Could not calculate script SHA-256."); free(script_dir);free(tmp_path);free(script_path);free(sig_path);sb_free(&one);return -1; }
        if (sign_script(a,script_path,sig_path)!=0) { log_error("Script was created but its GPG signature failed; do not treat it as verified."); free(script_dir);free(tmp_path);free(script_path);free(sig_path);sb_free(&one);return -1; }
        filehash=xstrdup(hashbuf); save_script_record(r->uid,script_path,sig_path,hashbuf,a->fingerprint);
        save_generated_plan(r->uid,NULL,script_path,sig_path,hashbuf);
        plan_display=str_printf("sh %s",shell_quote(script_path));
        log_info("The plan exceeded %d characters; created a signed POSIX shell script for %llu track(s).",ONE_LINER_LIMIT,(unsigned long long)actions_n);
        printf("\n%sSCRIPT%s\n  %s\n%sSIGNATURE%s\n  %s\n%sSHA-256%s\n  %s\n%sRUN COMMAND%s\n  %s\n\n",g_color?CYAN:"",g_color?RESET:"",script_path,g_color?CYAN:"",g_color?RESET:"",sig_path,g_color?CYAN:"",g_color?RESET:"",hashbuf,g_color?CYAN:"",g_color?RESET:"",plan_display);
        log_info("The script is signed with local key %s.",a->fingerprint);
    }
    if (!o->no_clipboard) {
        if (copy_clipboard(plan_display)==0) log_info("Copied the generated command/launcher to the clipboard.");
        else log_warn("Clipboard copy unavailable (install wl-clipboard, xclip, or xsel (Linux), or use pbcopy (macOS)); the command is printed above.");
    } else log_info("--no-clipboard selected; clipboard was not touched.");
    free(plan_display); free(saved_line); free(script_path); free(sig_path); free(filehash); free(script_dir);free(tmp_path);sb_free(&one);
    log_info("Run ID: %s",r->uid); log_info("Database: %s",a->db_path);
    log_info("Original files were not deleted and FFmpeg has not been run by RAMIF.");
    return 0;
}

static int internal_mark_complete(const char *db_path,const char *uid,const char *idstr) {
    char *end=NULL; long long id=strtoll(idstr,&end,10); if (!end || *end || id<=0) return 2;
    if (access(db_path,F_OK)!=0 || db_open(db_path)!=0) return 2;
    sqlite3_stmt *st=NULL; if (db_prepare("UPDATE files SET status='DONE',error=NULL WHERE run_uid=?1 AND file_id=?2",&st)) { db_close();return 2; }
    sqlite3_bind_text(st,1,uid,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(st,2,(sqlite3_int64)id); int rc=db_step_done(st); int changed=sqlite3_changes(g_db); sqlite3_finalize(st);
    if (rc!=0 || changed!=1) { log_error("Could not record completion for run %s / file %lld; database may have been flushed or moved.",uid,id); db_close(); return 2; }
    sqlite3_stmt *check=NULL;
    if (!db_prepare("SELECT COUNT(*) FROM files WHERE run_uid=?1 AND status NOT IN ('DONE','DUPLICATE') AND duplicate_of IS NULL",&check)) {
        sqlite3_bind_text(check,1,uid,-1,SQLITE_TRANSIENT); int sr=sqlite3_step(check); if (sr==SQLITE_ROW && sqlite3_column_int64(check,0)==0) update_run_status(uid,"DONE"); sqlite3_finalize(check);
    }
    db_close(); return rc==0?0:2;
}
static void free_options(Options *o) { free(o->target);free(o->output);free(o->format);free(o->run_uid);free(o->verify_script); }

int main(int argc,char **argv) {
    umask(077); g_color=isatty(STDOUT_FILENO)&&getenv("NO_COLOR")==NULL;
    signal(SIGINT,on_signal); signal(SIGTERM,on_signal); signal(SIGPIPE,SIG_IGN);
    if (argc==5 && strcmp(argv[1],"--internal-mark-complete")==0) {
        return internal_mark_complete(argv[2],argv[3],argv[4]);
    }
    Options o; int pr=parse_args(argc,argv,&o); if (pr!=0) { free_options(&o); return pr>0?0:2; }
    App app; if (init_app(&app,argv[0])!=0) { free_options(&o);free_app(&app);return 2; }
    print_banner();
    if (o.flush) {
        int rc=flush_database(&app); db_close();free_options(&o);free_app(&app);return rc?2:0;
    }
    if (db_open(app.db_path)!=0) { db_close();free_options(&o);free_app(&app);return 2; }
    if (ensure_signing_key(&app)!=0 || write_ini(&app)!=0) { db_close();free_options(&o);free_app(&app);return 2; }
    if (o.verify_script) { char *p=expand_tilde(o.verify_script); char *abs=absolute_existing(p); free(p); if (!abs) { log_error("Cannot resolve script path."); db_close();free_options(&o);free_app(&app);return 2; } int rc=verify_script(&app,abs);free(abs);db_close();free_options(&o);free_app(&app);return rc?2:0; }
    RunInfo run={0}; bool new_run=true;
    if (o.resume) {
        if (load_latest_run(&run)!=0) { db_close();free_options(&o);free_app(&app);return 2; }
        new_run=false;
        if (o.target||o.output) log_warn("--resume uses the saved target/output for the unfinished run.");
        bool previous_dedupe=run.dedupe_all;
        bool dedupe_changed=o.no_duplicates_set && !previous_dedupe;
        bool scan_incomplete=(!strcmp(run.status,"SCANNING") || !strcmp(run.status,"NEW"));
        if (o.recursive_set && !scan_incomplete && o.recursive!=run.recursive) {
            log_warn("The directory scan is already saved; recursive mode cannot be changed during this resume. Starting a new run applies a different scan mode.");
            o.recursive_set=false;
        }
        if (o.recursive_set) run.recursive=o.recursive;
        if (o.no_duplicates_set) run.dedupe_all=true;
        if (o.format_set) { free(run.format);run.format=xstrdup(o.format); }
        run.ask_artist=run.ask_artist||o.ask_artist; run.ask_album=run.ask_album||o.ask_album;run.ask_title=run.ask_title||o.ask_title;run.ask_lyrics=run.ask_lyrics||o.ask_lyrics;run.ask_image=run.ask_image||o.ask_image;run.windows_batch=run.windows_batch||o.windows_batch; o.windows_batch=run.windows_batch;
        if (dedupe_changed) {
            sqlite3_stmt *count=NULL; sqlite3_int64 done_count=0;
            if (!db_prepare("SELECT COUNT(*) FROM files WHERE run_uid=?1 AND status='DONE'",&count)) {
                sqlite3_bind_text(count,1,run.uid,-1,SQLITE_TRANSIENT); if (sqlite3_step(count)==SQLITE_ROW) done_count=sqlite3_column_int64(count,0); sqlite3_finalize(count);
            }
            if (done_count>0) {
                log_warn("Cannot change duplicate policy after output conversion has started; keeping the saved policy.");
                run.dedupe_all=previous_dedupe; o.no_duplicates_set=false; dedupe_changed=false;
            } else {
                sqlite3_stmt *reset=NULL;
                if (!db_prepare("UPDATE files SET sha256=NULL,hash_status=0,duplicate_of=NULL,status='DISCOVERED',error=NULL WHERE run_uid=?1",&reset)) {
                    sqlite3_bind_text(reset,1,run.uid,-1,SQLITE_TRANSIENT);db_step_done(reset);sqlite3_finalize(reset);
                }
                log_info("Duplicate policy changed; SHA-256 decisions will be recalculated.");
            }
        }
        o.ask_artist=run.ask_artist; o.ask_album=run.ask_album; o.ask_title=run.ask_title; o.ask_lyrics=run.ask_lyrics; o.ask_image=run.ask_image;
        if (strcmp(run.format,"mp3")&&run.ask_image) log_warn("Existing image choices are not supported for non-MP3 output.");
        if (o.recursive_set||o.no_duplicates_set||o.format_set||o.ask_artist||o.ask_album||o.ask_title||o.ask_lyrics||o.ask_image||o.windows_batch) {
            sqlite3_stmt *u=NULL; if (!db_prepare("UPDATE runs SET recursive=?2,dedupe_all=?3,format=?4,ask_artist=?5,ask_album=?6,ask_title=?7,ask_lyrics=?8,ask_image=?9,windows_batch=?10 WHERE run_uid=?1",&u)) {
                sqlite3_bind_text(u,1,run.uid,-1,SQLITE_TRANSIENT);sqlite3_bind_int(u,2,run.recursive);sqlite3_bind_int(u,3,run.dedupe_all);sqlite3_bind_text(u,4,run.format,-1,SQLITE_TRANSIENT);sqlite3_bind_int(u,5,run.ask_artist);sqlite3_bind_int(u,6,run.ask_album);sqlite3_bind_int(u,7,run.ask_title);sqlite3_bind_int(u,8,run.ask_lyrics);sqlite3_bind_int(u,9,run.ask_image);sqlite3_bind_int(u,10,run.windows_batch);db_step_done(u);sqlite3_finalize(u);
            }
        }
        log_info("Resuming run %s (database state: %s).",run.uid,run.status);
    } else {
        if ((!o.target||!o.output) && !o.manual_input) { log_error("--target and --output are required, or use --manual-input."); usage(stderr); db_close();free_options(&o);free_app(&app);return 2; }
        if (o.manual_input || !o.target) {
            char *p = read_nonempty_prompt("Target audio directory [TAB completes]: ", o.target ? o.target : "", "target directory");
            if (!p) { log_error("Could not read target directory."); db_close(); free_options(&o); free_app(&app); return 2; }
            free(o.target); o.target = p;
        }
        if (o.manual_input || !o.output) {
            char *p = NULL;
            for (;;) {
                p = read_line_prompt("Output directory [TAB completes; Esc returns to target]: ", o.output ? o.output : "", true);
                if (!p) break;
                if (prompt_is_back(p)) {
                    free(p); p = NULL;
                    log_info("Returning to target-directory input.");
                    char *target = read_nonempty_prompt("Target audio directory [TAB completes]: ", o.target ? o.target : "", "target directory");
                    if (!target) break;
                    free(o.target); o.target = target;
                    continue;
                }
                if (!*p) { free(p); p = NULL; log_warn("An output directory is required."); continue; }
                break;
            }
            if (!p) { log_error("Could not read output directory."); db_close(); free_options(&o); free_app(&app); return 2; }
            free(o.output); o.output = p;
        }
        char *t=absolute_existing(o.target); if (!t) { log_error("Target is not an existing path: %s",o.target);db_close();free_options(&o);free_app(&app);return 2; }
        struct stat ts; if (stat(t,&ts)!=0||!S_ISDIR(ts.st_mode)) { log_error("Target must be a directory.");free(t);db_close();free_options(&o);free_app(&app);return 2; }
        char *expanded_out=expand_tilde(o.output); if (expanded_out[0]!='/') { char cwd[PATH_MAX]; if (!getcwd(cwd,sizeof(cwd))) { free(t);free(expanded_out);db_close();free_options(&o);free_app(&app);return 2; } char *tmp=path_join(cwd,expanded_out);free(expanded_out);expanded_out=tmp; }
        if (mkdir_p(expanded_out,0700)!=0) { log_error("Cannot create output directory %s: %s",expanded_out,strerror(errno));free(t);free(expanded_out);db_close();free_options(&o);free_app(&app);return 2; }
        char *out=realpath(expanded_out,NULL); free(expanded_out); if (!out) { free(t);db_close();free_options(&o);free_app(&app);return 2; }
        if (!strcmp(t,out)) { log_error("Output directory must differ from target.");free(t);free(out);db_close();free_options(&o);free_app(&app);return 2; }
        free(o.target);free(o.output);o.target=t;o.output=out;
        if (o.ask_image && strcmp(o.format,"mp3")!=0) log_warn("--image embedding is supported for MP3 output only; prompts will be ignored for other formats.");
        if (create_run(&app,&o,&run.uid)!=0) { db_close();free_options(&o);free_app(&app);return 2; }
        run.target=xstrdup(o.target);run.output=xstrdup(o.output);run.format=xstrdup(o.format);run.status=xstrdup("SCANNING");run.recursive=o.recursive;run.dedupe_all=o.no_duplicates;run.ask_artist=o.ask_artist;run.ask_album=o.ask_album;run.ask_title=o.ask_title;run.ask_lyrics=o.ask_lyrics;run.ask_image=o.ask_image;run.windows_batch=o.windows_batch;run.default_artist=setting_get("default_artist");run.default_album=setting_get("default_album");
    }
    /* On a first run, or a run interrupted during directory scanning, rebuild the inventory. */
    bool rescan = new_run || !run.status || !strcmp(run.status,"SCANNING") || !strcmp(run.status,"NEW");
    if (rescan) {
        if (!new_run) { sqlite3_stmt *z=NULL; if (!db_prepare("DELETE FROM files WHERE run_uid=?1",&z)) { sqlite3_bind_text(z,1,run.uid,-1,SQLITE_TRANSIENT);db_step_done(z);sqlite3_finalize(z); } }
        if (scan_directory(&run)!=0) { update_run_status(run.uid,"SCANNING"); free_runinfo(&run);db_close();free_options(&o);free_app(&app);return 130; }
    }
    if (!strcmp(run.status?run.status:"","PLANNED") && !o.no_duplicates_set && !o.format_set) {
        /* The stored plan already has hashes and deduplication decisions; retain them for resume. */
    } else {
        if (hash_candidates(&run)!=0) { free_runinfo(&run);db_close();free_options(&o);free_app(&app);return 2; }
    }
    update_run_status(run.uid,"PLANNING");
    if (prepare_metadata(&run,&app,&o)!=0) { update_run_status(run.uid,"PLANNING"); log_warn("Run saved; use --resume to continue the unfinished metadata step."); free_runinfo(&run);db_close();free_options(&o);free_app(&app);return 130; }
    if (plan_and_emit(&app,&run,&o)!=0) { free_runinfo(&run);db_close();free_options(&o);free_app(&app);return 2; }
    free_runinfo(&run); db_close(); free_options(&o); free_app(&app); return g_interrupted?130:0;
}
