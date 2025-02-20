//
// Created by skytl on 1/31/25.
//

/*** Includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


/*** Defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define VERSION "0.0.1"
#define TAB_STOP 8
#define QUIT_CONFIRMATION 2

enum editorKey {
    BACKSPACE = 127,
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    DEL,
    HOME,
    END,
    PAGE_UP,
    PAGE_DOWN,
};

/*** Data ***/
typedef struct erow {
    int size;
    int rsize;
    char *data;
    char *render;
} erow;

struct editorConfig {
    int cx, cy;
    int rx;
    int rowoffset;
    int coloffset;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

/*** Prototypes ***/

void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** Terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= (OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME;
                        case '3': return DEL;
                        case '4': return END;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME;
                        case '8': return END;
                    }
                }
            } else{
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME;
                    case 'F': return END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME;
                case 'F': return END;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buffer[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buffer) - 1) {
        if (read(STDIN_FILENO, &buffer[i], 1) != 1) break;
        if (buffer[i] == 'R') break;
        i++;
    }
    buffer[i] = '\0';

    if (buffer[0] != '\x1b' || buffer[1] != '[') return -1;
    if (sscanf(&buffer[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
        ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** Row Operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->data[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int current_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->data[cx] == '\t')
            current_rx += (TAB_STOP - 1) - (current_rx % TAB_STOP);
        current_rx++;

        if (current_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->data[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->data[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->data[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    E.row[at].size = len;
    E.row[at].data = malloc(len + 1);
    memcpy(E.row[at].data, s, len);
    E.row[at].data[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->data);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, char c) {
    if (at < 0 || at > row->size) at = row->size;
    row->data = realloc(row->data, row->size + 2);
    memmove(&row->data[at + 1], &row->data[at], row->size - at + 1);
    row->size++;
    row->data[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->data = realloc(row->data, row->size + len + 1);
    memcpy(&row->data[row->size], s, len);
    row->size += len;
    row->data[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->data[at], &row->data[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/** Editor Operations ***/

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->data[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->data[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->data, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}


/*** File I/O ***/

char *editorRowsToString(int *buflen) {
    int totallen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totallen += E.row[j].size + 1;
    *buflen = totallen;

    char *buf = malloc(totallen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].data, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buffer = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buffer, len) == len) {
                close(fd);
                free(buffer);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buffer);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/** Search ***/
void editorSearchCallback(char *query, int key) {
    if (key == '\r' || key == '\x1b') {
        return;
    }

    int i;
    for (i = 0; i < E.numrows; i++) {
        erow *row = &E.row[i];
        char *match = strstr(row->render, query);
        if (match) {
            E.cy = i;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoffset = E.numrows;
            break;
        }
    }
}

void editorSearch() {
    int scx = E.cx;
    int scy = E.cy;
    int scoloffset = E.coloffset;
    int srowoffset = E.rowoffset;


    char *query = editorPrompt("Search: %s (ESC to cancel)", editorSearchCallback);
    if (query) {
        free(query);
    } else {
        E.cx = scx;
        E.cy = scy;
        E.coloffset = scoloffset;
        E.rowoffset = srowoffset;
    }


}
/*** Append Buffer ***/
struct append_buffer {
    char *buf;
    int len;
};

#define APPEND_BUFFER_INIT {NULL, 0}

void appendBufferAppend(struct append_buffer *ab, const char *s, int len) {
    char *new = realloc(ab->buf, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->buf = new;
    ab->len += len;
}

void appendBufferFree(struct append_buffer *ab) {
    free(ab->buf);
}

/*** Output ***/
void editorScroll() {
    E.rx = 0;

    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoffset) {
        E.rowoffset = E.cy;
    }
    if (E.cy >= E.rowoffset + E.screenrows) {
        E.rowoffset = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloffset) {
        E.coloffset = E.rx;
    }
    if (E.rx >= E.coloffset + E.screencols) {
        E.coloffset = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct append_buffer *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoffset;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                  "TE -- version %s", VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    appendBufferAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) appendBufferAppend(ab, " ", 1);
                appendBufferAppend(ab, welcome, welcomelen);
            } else {
                appendBufferAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloffset;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            appendBufferAppend(ab, &E.row[filerow].render[E.coloffset], len);
        }

        appendBufferAppend(ab, "\x1b[K", 3);
        appendBufferAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct append_buffer *ab) {
    appendBufferAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
      E.filename ? E.filename : "[No Name]", E.numrows,
      E.dirty ? "[modified]" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
      E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    appendBufferAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            appendBufferAppend(ab, rstatus, rlen);
            break;
        } else {
            appendBufferAppend(ab, " ", 1);
            len++;
        }
    }
    appendBufferAppend(ab, "\x1b[m", 3);
    appendBufferAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct append_buffer *ab) {
    appendBufferAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        appendBufferAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct append_buffer ab = APPEND_BUFFER_INIT;

    appendBufferAppend(&ab, "\x1b[?25l", 6);
    appendBufferAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", (E.cy - E.rowoffset) + 1,
                                                                 (E.rx - E.coloffset) + 1);
    appendBufferAppend(&ab, buffer, strlen(buffer));

    appendBufferAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    appendBufferFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** Input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t buffersize = 128;
    char *buffer = malloc(buffersize);

    size_t bufferlen = 0;
    buffer[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buffer);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (bufferlen != 0) buffer[--bufferlen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buffer, c);
            free(buffer);
            return NULL;
        } else if (c == '\r') {
            if (bufferlen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buffer, c);
                return buffer;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (bufferlen == buffersize - 1) {
                buffersize *= 2;
                buffer = realloc(buffer, buffersize);
            }
            buffer[bufferlen++] = c;
            buffer[bufferlen] = '\0';
        }
        if (callback) callback(buffer, c);
    }
}

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() {
    static int quit_conf = QUIT_CONFIRMATION;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_conf > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes."
                    "Press CTRL-Q %d more times to quit.", quit_conf);
                quit_conf--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;
        case HOME:
            E.cx = 0;
            break;
        case END:
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;
        case CTRL_KEY('f'):
            editorSearch();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL:
            if (c == DEL) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoffset;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoffset + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }
    quit_conf = QUIT_CONFIRMATION;
}


/*** Init ***/
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoffset = 0;
    E.coloffset = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP:: CTRL-S to save | CTRL-F to search | CTRL-Q to quit");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
