#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

/*** includes ***/

#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>

/*** defines ***/

#define UNDO_LIMIT 50
#define BENCH_RENDER_ITERS 100
#define BENCH_RENDER_WARMUP 3

#define CTRL_KEY(k) ((k) & 0x1f)
// After taking logical and with 00011111 first 3 bits are set to 0 and last bits are retained as a => 01100001 that is ASCII values (for alphabets for combo with control key) are till 122 => 01111010 that is only end 5 bits change from a to z so taking and with hex 1f gives the end 5 bits which signifies the value of key;
// so if the key pressed has value equal to value of CTRL_KEY that is after taking end then it means the key is pressed with control key then we can map its functionality
// we will have to code for ascii value of all control combo keys (26)

/* ANSI escape sequences */
#define ESC_HIDE_CURSOR "\x1b[?25l"
#define ESC_SHOW_CURSOR "\x1b[?25h"
#define ESC_CLEAR_SCREEN "\x1b[2J"
#define ESC_POSITION_CURSOR "\x1b[H"
#define ESC_CLEAR_LINE "\x1b[K"
#define ESC_INVERT_COLORS "\x1b[7m"
#define ESC_NORMAL_COLORS "\x1b[m"
#define ESC_QUERY_CURSOR_POS "\x1b[6n"
#define ESC_MOVE_CURSOR_TO_MAX "\x1b[999C\x1b[999B"

enum editorKey
{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DELETE_KEY
};


/*** prototypes ***/

struct abuf;

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
void editorRefreshScreenBuild(struct abuf *ab);
char *editorPrompt(char *prompt, void (*callback)(char *, int));
void editorPushUndo();
void editorUndo();
void editorRedo();
void editorClearUndoStacks();
void runBenchmark(char *filename);

/*** data ***/

typedef struct erow
{
    int size;
    char *chars;
    int rsize;
    char *render;
} erow;

struct editorConfig
{
    struct termios orig_termios;
    int cx;
    int cy;
    int rx;
    int screenrows;
    int screencols;
    int numrows;
    int rowOffset;
    int colOffset;
    int changes;
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    
};

struct editorConfig E;

typedef struct
{
    int cx;
    int cy;
    int numrows;
    erow *row;
} EditorSnapshot;

struct BenchResult
{
    double open_ms;
    long file_bytes;
    int num_lines;
    long peak_rss_kb;
    double render_build_avg_ms;
    double render_build_min_ms;
    double render_build_max_ms;
    double render_e2e_avg_ms;
    double render_e2e_min_ms;
    double render_e2e_max_ms;
    double search_ms;
    int terminal_rows;
    int terminal_cols;
};

static EditorSnapshot undo_stack[UNDO_LIMIT];
static EditorSnapshot redo_stack[UNDO_LIMIT];
static int undo_len = 0;
static int redo_len = 0;
static int suppress_undo = 0;
static int benchmark_mode = 0;

/*** benchmark helpers ***/

double benchNowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

long benchPeakRssKb()
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return -1;
    return ru.ru_maxrss;
}

double benchSearchAll(const char *query)
{
    double start = benchNowMs();
    int i;
    for (i = 0; i < E.numrows; i++)
    {
        erow *row = &E.row[i];
        if (row->render)
            (void)strstr(row->render, query);
    }
    return benchNowMs() - start;
}

void benchPrintResult(struct BenchResult *r, const char *filename)
{
    fprintf(stderr, "=== Text Editor Benchmark ===\n");
    fprintf(stderr, "file:              %s\n", filename);
    fprintf(stderr, "file_bytes:        %ld (%.2f MB)\n", r->file_bytes,
            r->file_bytes / (1024.0 * 1024.0));
    fprintf(stderr, "num_lines:         %d\n", r->num_lines);
    fprintf(stderr, "open_ms:           %.3f\n", r->open_ms);
    fprintf(stderr, "peak_rss_kb:       %ld (%.2f MB)\n", r->peak_rss_kb,
            r->peak_rss_kb / 1024.0);
    fprintf(stderr, "terminal:          %dx%d\n", r->terminal_cols, r->terminal_rows);
    fprintf(stderr, "render_build_ms:   avg=%.3f min=%.3f max=%.3f\n",
            r->render_build_avg_ms, r->render_build_min_ms, r->render_build_max_ms);
    fprintf(stderr, "render_e2e_ms:     avg=%.3f min=%.3f max=%.3f\n",
            r->render_e2e_avg_ms, r->render_e2e_min_ms, r->render_e2e_max_ms);
    fprintf(stderr, "search_ms:         %.3f\n", r->search_ms);
    fprintf(stderr, "=============================\n");
}

/*** terminal ***/

void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey()
{
    int nread = 0;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
        {
            die("read");
        }
    }
    // if (iscntrl(c))
    // {
    //     printf("%d\r\n", c);
    // }
    // else
    // {
    //     printf("%d ('%c')\r\n", c, c);
    // }
    // sleep(2);
    if (c == '\x1b')
    {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '1':
                        return HOME_KEY;
                    case '7':
                        return HOME_KEY;
                    case '4':
                        return END_KEY;
                    case '8':
                        return END_KEY;
                    case '3':
                        return DELETE_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP; // up
                case 'B':
                    return ARROW_DOWN; // down
                case 'C':
                    return ARROW_RIGHT; // right
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY; // left
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
    return 'x';
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    // printf("\r\n&buf[1]: '%s'\r\n",&buf[1]);

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;

    // printf("\r\n");
    // char c;
    // while(read(STDIN_FILENO, &c, 1) == 1){
    //     if(iscntrl(c)){
    //         printf("%d\r\n",c);
    //     }
    //     else{
    //         printf("%d ('%c')\r\n",c,c);
    //     }
    // }

    // editorReadKey();
    // return -1;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
        {
            return -1;
        }

        // editorReadKey();
        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
            rx += (8 - 1) - (rx % 8);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
        {
            cur_rx += 7 - (cur_rx % 8);
        }
        cur_rx++;
        if (cur_rx > rx)
        {
            return cx;
        }
    }
    return cx;
}

void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j;

    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
            tabs++;
    }
    free(row->render);
    row->render = malloc(row->size + tabs * 7 + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';
            while (idx % 8 != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
        return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    // int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    // E.row.size = len;
    // E.row.chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.changes++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
    {
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.changes++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.changes++;
}

void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size)
    {
        return;
    }
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.changes++;
}

void editorFreeRow(erow *row)
{
    free(row->chars);
    free(row->render);
}

/*** undo / redo ***/

EditorSnapshot snapshotCopyState()
{
    EditorSnapshot snap;
    int i;
    snap.cx = E.cx;
    snap.cy = E.cy;
    snap.numrows = E.numrows;
    snap.row = malloc(sizeof(erow) * (snap.numrows ? snap.numrows : 1));
    for (i = 0; i < snap.numrows; i++)
    {
        snap.row[i].size = E.row[i].size;
        snap.row[i].rsize = E.row[i].rsize;
        snap.row[i].chars = malloc(E.row[i].size + 1);
        memcpy(snap.row[i].chars, E.row[i].chars, E.row[i].size + 1);
        if (E.row[i].render)
        {
            snap.row[i].render = malloc(E.row[i].rsize + 1);
            memcpy(snap.row[i].render, E.row[i].render, E.row[i].rsize + 1);
        }
        else
        {
            snap.row[i].render = NULL;
        }
    }
    return snap;
}

void snapshotFree(EditorSnapshot *snap)
{
    int i;
    for (i = 0; i < snap->numrows; i++)
    {
        free(snap->row[i].chars);
        free(snap->row[i].render);
    }
    free(snap->row);
    snap->row = NULL;
    snap->numrows = 0;
}

void editorFreeAllRows()
{
    int i;
    for (i = 0; i < E.numrows; i++)
        editorFreeRow(&E.row[i]);
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
}

void editorClearUndoStacks()
{
    int i;
    for (i = 0; i < undo_len; i++)
        snapshotFree(&undo_stack[i]);
    for (i = 0; i < redo_len; i++)
        snapshotFree(&redo_stack[i]);
    undo_len = 0;
    redo_len = 0;
}

void editorPushUndo()
{
    int i;
    if (suppress_undo)
        return;
    if (undo_len == UNDO_LIMIT)
    {
        snapshotFree(&undo_stack[0]);
        memmove(&undo_stack[0], &undo_stack[1],
                sizeof(EditorSnapshot) * (UNDO_LIMIT - 1));
        undo_len--;
    }
    undo_stack[undo_len++] = snapshotCopyState();
    for (i = 0; i < redo_len; i++)
        snapshotFree(&redo_stack[i]);
    redo_len = 0;
}

void editorRestoreSnapshot(EditorSnapshot *snap)
{
    int i;
    suppress_undo = 1;
    editorFreeAllRows();
    E.numrows = snap->numrows;
    E.row = malloc(sizeof(erow) * (E.numrows ? E.numrows : 1));
    for (i = 0; i < E.numrows; i++)
    {
        E.row[i].size = snap->row[i].size;
        E.row[i].rsize = snap->row[i].rsize;
        E.row[i].chars = malloc(snap->row[i].size + 1);
        memcpy(E.row[i].chars, snap->row[i].chars, snap->row[i].size + 1);
        if (snap->row[i].render)
        {
            E.row[i].render = malloc(snap->row[i].rsize + 1);
            memcpy(E.row[i].render, snap->row[i].render, snap->row[i].rsize + 1);
        }
        else
        {
            E.row[i].render = NULL;
        }
    }
    E.cx = snap->cx;
    E.cy = snap->cy;
    E.changes = 1;
    suppress_undo = 0;
}

void editorUndo()
{
    if (undo_len == 0)
        return;
    if (redo_len == UNDO_LIMIT)
    {
        snapshotFree(&redo_stack[0]);
        memmove(&redo_stack[0], &redo_stack[1],
                sizeof(EditorSnapshot) * (UNDO_LIMIT - 1));
        redo_len--;
    }
    redo_stack[redo_len++] = snapshotCopyState();
    EditorSnapshot snap = undo_stack[--undo_len];
    editorRestoreSnapshot(&snap);
    snapshotFree(&snap);
}

void editorRedo()
{
    if (redo_len == 0)
        return;
    if (undo_len == UNDO_LIMIT)
    {
        snapshotFree(&undo_stack[0]);
        memmove(&undo_stack[0], &undo_stack[1],
                sizeof(EditorSnapshot) * (UNDO_LIMIT - 1));
        undo_len--;
    }
    undo_stack[undo_len++] = snapshotCopyState();
    EditorSnapshot snap = redo_stack[--redo_len];
    editorRestoreSnapshot(&snap);
    snapshotFree(&snap);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
        return;
    erow *row = &E.row[at];
    editorFreeRow(row);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.changes++;
}

/*** editor operations  ***/

void editorInsertChar(int c)
{
    editorPushUndo();
    if (E.cy == E.numrows)
    {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline()
{
    editorPushUndo();
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar()
{
    if (E.cy == E.numrows)
    {
        return;
    }
    if (E.cx == 0 && E.cy == 0)
    {
        return;
    }

    editorPushUndo();

    erow *row = &E.row[E.cy];

    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], E.row[E.cy].chars, E.row[E.cy].size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen)
{
    int totalLen = 0;
    int i;
    for (i = 0; i < E.numrows; i++)
    {
        totalLen += E.row[i].size + 1;
    }
    *buflen = totalLen;
    char *buf = malloc(totalLen);
    char *p = buf;
    for (i = 0; i < E.numrows; i++)
    {
        memcpy(p, E.row[i].chars, E.row[i].size);
        p += E.row[i].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editorOpen(char *filename)
{
    editorClearUndoStacks();
    free(E.filename);
    E.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;

    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
        {
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
    }
    // linelen = getline(&line, &linecap, fp);
    // if(linelen != -1){
    //     while(linelen > 0 && (line[linelen-1]=='\n' || line[linelen-1]=='\r')){
    //         linelen--;
    //     }
    //     editorAppendRow(line, linelen);
    // }
    free(line);
    fclose(fp);
    E.changes = 0;
}

void editorSave()
{
    if (E.filename == NULL)
    {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1)
    {
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                editorSetStatusMessage("%d bytes written to disk", len);
                E.changes = 0;
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));

    // ftruncate(fd,len);
    // write(fd,buf,len);
    // close(fd);
    // free(buf);
}

/*** search ***/

void editorFindCallback(char *query, int key)
{
    if (key == '\r' || key == '\x1b')
    {
        return;
    }
    int i;
    for (i = 0; i < E.numrows; i++)
    {
        erow *row = &E.row[i];
        char *match = strstr(row->render, query);
        if (match)
        {
            E.cy = i;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowOffset = E.numrows;
            break;
        }
    }
}

void editorFind()
{
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.colOffset;
    int saved_rowoff = E.rowOffset;
    char *query = editorPrompt("Search: %s (ESC to cancel)", editorFindCallback);
    if (query)
    {
        free(query);
    }
    else
    {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.colOffset = saved_coloff;
        E.rowOffset = saved_rowoff;
    }
}

/*** append buffer ***/

struct abuf
{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0};

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL)
        return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output  ***/

void editorScroll()
{
    E.rx = 0;
    if (E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    if (E.cy < E.rowOffset)
    {
        E.rowOffset = E.cy;
    }
    if (E.cy >= E.rowOffset + E.screenrows)
    {
        E.rowOffset = E.cy - E.screenrows + 1;
    }
    if (E.cx < E.colOffset)
        E.colOffset = E.cx;
    if (E.cx >= E.colOffset + E.screencols)
        E.colOffset = E.cx - E.screencols + 1;
    if (E.rx < E.colOffset)
    {
        E.colOffset = E.rx;
    }
    if (E.rx >= E.colOffset + E.screencols)
    {
        E.colOffset = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    int i;
    for (i = 0; i < E.screenrows; i++)
    {
        int fileRow = i + E.rowOffset;
        if (fileRow >= E.numrows)
        {
            if (E.numrows == 0 && i == E.screenrows / 6)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Samarth's editor -- version 0.0.1");
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                {
                    abAppend(ab, " ", 1);
                }
                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            int len = E.row[fileRow].rsize - E.colOffset;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;
            abAppend(ab, &E.row[fileRow].render[E.colOffset], len);
        }
        abAppend(ab, "\x1b[K", 3);
        // if (i < E.screenrows - 1)
        // {
        abAppend(ab, "\r\n", 2);
        // }
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4);

    char status[80];
    char rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[NO Name]", E.numrows, E.changes ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    if (len > E.screencols)
        len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
        }
        len++;
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
    {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreenBuild(struct abuf *ab)
{
    editorScroll();

    abAppend(ab, "\x1b[?25l", 6);
    abAppend(ab, "\x1b[H", 3);
    editorDrawRows(ab);
    editorDrawStatusBar(ab);
    editorDrawMessageBar(ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowOffset + 1, E.rx - E.colOffset + 1);
    abAppend(ab, buf, strlen(buf));
    abAppend(ab, "\x1b[?25h", 6);
}

void editorRefreshScreen()
{
    struct abuf ab = ABUF_INIT;
    editorRefreshScreenBuild(&ab);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input  ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();
        if (c == DELETE_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                if (callback)
                    callback(buf, c);
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback)
            callback(buf, c);
    }
}

void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch (key)
    {
    case ARROW_UP:
        if (E.cy != 0)
        {
            E.cy--;
        }
        break;
    case ARROW_LEFT:
        if (E.cx != 0)
        {
            E.cx--;
        }
        else if (E.cy > 0)
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows)
        {
            E.cy++;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size)
        {
            E.cx++;
        }
        else if (row && E.cx == row->size)
        {
            E.cy++;
            E.cx = 0;
        }
        // E.cx++;
        break;

    default:
        break;
    }
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

void editorProcessKeypress()
{
    static int times = 1;
    int c = editorReadKey();
    switch (c)
    {
    case '\r':
        // later
        editorInsertNewline();
        break;
    case BACKSPACE:
        editorDelChar();
        break;
    case CTRL_KEY('h'):
        // later

        break;
    case DELETE_KEY:
        // later
        editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;
    case CTRL_KEY('l'):
        // later
        break;
    case '\x1b':
        // later
        break;
    case CTRL_KEY('q'):
        if (E.changes && times > 0)
        {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q one more time to quit");
            times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    case CTRL_KEY('s'):
        editorSave();
        break;
    case CTRL_KEY('f'):
        editorFind();
        break;
    case CTRL_KEY('z'):
        editorUndo();
        break;
    case CTRL_KEY('y'):
        editorRedo();
        break;
    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if (E.cy < E.numrows)
        {
            E.cx = E.row[E.cy].size;
        }
        // E.cx = E.screencols - 1;
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    case PAGE_UP:
    case PAGE_DOWN:
    {
        if (c == PAGE_UP)
        {
            E.cy = E.rowOffset;
        }
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowOffset + E.screenrows - 1;
            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }
        int times = E.screenrows;
        while (times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        break;
    }
    default:
        editorInsertChar(c);
        break;
    }
    times = 1;
}

/*** init ***/

void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowOffset = 0;
    E.colOffset = 0;
    E.rx = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.changes = 0;
    if (benchmark_mode || !isatty(STDOUT_FILENO))
    {
        E.screenrows = 24;
        E.screencols = 80;
    }
    else if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("Window Size");
    E.screenrows -= 2;
}

static void benchAccumulate(double *avg, double *min, double *max, double val, int *count)
{
    if (*count == 0)
    {
        *min = val;
        *max = val;
        *avg = val;
    }
    else
    {
        if (val < *min)
            *min = val;
        if (val > *max)
            *max = val;
        *avg += val;
    }
    (*count)++;
}

void runBenchmark(char *filename)
{
    struct stat st;
    if (stat(filename, &st) != 0)
        die("stat");

    struct BenchResult result = {0};
    result.file_bytes = st.st_size;

    double start = benchNowMs();
    editorOpen(filename);
    result.open_ms = benchNowMs() - start;
    result.num_lines = E.numrows;
    result.peak_rss_kb = benchPeakRssKb();
    result.terminal_rows = E.screenrows;
    result.terminal_cols = E.screencols;

    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd == -1)
        die("open /dev/null");
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout == -1)
        die("dup stdout");
    if (dup2(null_fd, STDOUT_FILENO) == -1)
        die("dup2 /dev/null");

    double build_avg = 0, build_min = 0, build_max = 0;
    double e2e_avg = 0, e2e_min = 0, e2e_max = 0;
    int build_count = 0;
    int e2e_count = 0;
    int i;

    for (i = 0; i < BENCH_RENDER_ITERS; i++)
    {
        struct abuf ab = ABUF_INIT;
        double t0 = benchNowMs();
        editorRefreshScreenBuild(&ab);
        double build_ms = benchNowMs() - t0;
        abFree(&ab);

        double t1 = benchNowMs();
        editorRefreshScreen();
        double e2e_ms = benchNowMs() - t1;

        if (i >= BENCH_RENDER_WARMUP)
        {
            benchAccumulate(&build_avg, &build_min, &build_max, build_ms, &build_count);
            benchAccumulate(&e2e_avg, &e2e_min, &e2e_max, e2e_ms, &e2e_count);
        }
    }

    if (dup2(saved_stdout, STDOUT_FILENO) == -1)
        die("restore stdout");
    close(saved_stdout);
    close(null_fd);

    if (build_count > 0)
        build_avg /= build_count;
    if (e2e_count > 0)
        e2e_avg /= e2e_count;

    result.render_build_avg_ms = build_avg;
    result.render_build_min_ms = build_min;
    result.render_build_max_ms = build_max;
    result.render_e2e_avg_ms = e2e_avg;
    result.render_e2e_min_ms = e2e_min;
    result.render_e2e_max_ms = e2e_max;
    result.search_ms = benchSearchAll("aaaa");

    benchPrintResult(&result, filename);
}

int main(int argc, char *argv[])
{
    if (argc >= 3 && strcmp(argv[1], "--benchmark") == 0)
    {
        benchmark_mode = 1;
        initEditor();
        runBenchmark(argv[2]);
        return 0;
    }

    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-F = search | Ctrl-Z = undo | Ctrl-Y = redo | Ctrl-Q = quit");
    // char c;
    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}