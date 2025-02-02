//
// Created by skytl on 1/31/25.
//

/*** Includes ***/
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

/*** Defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define VERSION "0.0.1"

enum editorKey {
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
struct editorConfig {
    int cx, cy;
    int screen_width;
    int screen_height;
    struct termios orig_termios;
};
struct editorConfig E;

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

/*** Terminal ***/
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
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
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
                        case '2': return DEL;
                        case '4': return END;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME;
                        case '8': return END;
                    }
                }
            }
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME;
                case 'F': return END;
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME;
                case 'F': return END;
            }
        }
    return '\x1b';
    }
    return c;
}

int getCursorPosition(int *width, int *height) {
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
    if (sscanf(&buffer[2], "%d;%d", width, height) != 2) return -1;

    return 0;
}

int getWindowSize(int *width, int *height) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
        ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(width, height);
    }
    *width = ws.ws_col;
    *height = ws.ws_row;
    return 0;
}

/*** Append Buffer ***/
struct append_buffer {
    char *buf;
    int len;
};
#define APPEND_BUFFER_INIT {NULL, 0}

void appendBufferAppend(struct append_buffer *ab, const char *buf, int len) {
    char *new = realloc(ab->buf, ab->len + len);

    if (new ==  NULL) return;
    memcpy(&new[ab->len], buf, len);
    ab->buf = new;
    ab->len += len;
}

void appendBufferFree(struct append_buffer *ab) {
    free(ab->buf);
}

/*** Output ***/
void editorDrawRows(struct append_buffer *ab) {
    for (int y = 0; y < E.screen_height; y++) {
        if (y == E.screen_height / 3) {
            char welcome[80];
            int welcomeLen = snprintf(welcome, sizeof(welcome),
                "Text Editor -- Version %s", VERSION);
            if (welcomeLen > E.screen_width) welcomeLen = E.screen_width;
            int padding = (E.screen_width - welcomeLen) / 2;
            if (padding) {
                appendBufferAppend(ab, "~", 1);
                padding--;
            }
            while (padding--) appendBufferAppend(ab, " ", 1);
            appendBufferAppend(ab, welcome, welcomeLen);
        } else {
            appendBufferAppend(ab, "~", 1);
        }

        appendBufferAppend(ab, "\x1b[K", 3);
        if (y < E.screen_width - 1) appendBufferAppend(ab, "\r\n", 2);
    }
}

void editorRefreshScreen() {
    struct append_buffer ab = APPEND_BUFFER_INIT;

    appendBufferAppend(&ab, "\x1b[?25l", 6);
    appendBufferAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    appendBufferAppend(&ab, buffer, strlen(buffer));

    appendBufferAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    appendBufferFree(&ab);
}

/*** Input ***/
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screen_width - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.screen_height - 1) {
                E.cy++;
            }
            break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case HOME:
            E.cx = 0;
            break;
        case END:
            E.cx = E.screen_width - 1;
            break;
        case PAGE_UP:
        case PAGE_DOWN: {
            int times = E.screen_height;
            while (times--) {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}


/*** Init ***/
void initEditor() {
    E.cx = 0;
    E.cy = 0;

    if (getWindowSize(&E.screen_width, &E.screen_height) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while (1){
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}