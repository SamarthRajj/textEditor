#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

/*** includes ***/

#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <time.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
// After taking logical and with 00011111 first 3 bits are set to 0 and last bits are retained as a => 01100001 that is ASCII values (for alphabets for combo with control key) are till 122 => 01111010 that is only end 5 bits change from a to z so taking and with hex 1f gives the end 5 bits which signifies the value of key;
// so if the key pressed has value equal to value of CTRL_KEY that is after taking end then it means the key is pressed with control key then we can map its functionality
// we will have to code for ascii value of all control combo keys (26)

enum editorKey
{
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
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
};

struct editorConfig E;

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

void editorAppendRow(char *s, size_t len)
{
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
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
}

/*** file i/o ***/

void editorOpen(char *filename)
{
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
        editorAppendRow(line, linelen);
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

void editorDrawStatusBar(struct abuf* ab){
    abAppend(ab, "\x1b[7m", 4);

    char status[80];
    char rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines", E.filename? E.filename:"[NO Name]", E.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy+1, E.numrows);
    if(len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while(len < E.screencols){
        if(E.screencols - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        }else{
        abAppend(ab, " ", 1);
        }
        len++;
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf* ab){
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols) msglen = E.screencols;
    if( msglen && time(NULL) - E.statusmsg_time < 5){
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen()
{
    editorScroll();
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    // write(STDOUT_FILENO, "\x1b[2J", 4);
    // abAppend(&ab, "\x1b[2J", 4); cleared entire screen at once
    // write(STDOUT_FILENO, "\x1b[H", 3);
    abAppend(&ab, "\x1b[H", 3);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowOffset + 1, E.rx - E.colOffset + 1);
    abAppend(&ab, buf, strlen(buf));
    // write(STDOUT_FILENO, "\x1b[H", 3);
    // abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input  ***/

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
    int c = editorReadKey();
    switch (c)
    {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if(E.cy < E.numrows){
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
        if(c == PAGE_UP){
            E.cy = E.rowOffset;
        }
        else if(c == PAGE_DOWN){
            E.cy = E.rowOffset + E.screenrows - 1;
            if(E.cy > E.numrows) E.cy = E.numrows;
        }
        int times = E.screenrows;
        while (times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    }
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
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("Window Size");
    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }
    editorSetStatusMessage("HELP: Ctrl-Q = quit");
    // char c;
    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}