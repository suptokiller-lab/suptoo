
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>



#ifdef _WIN32
  #include <winsock2.h>

  typedef int    (WSAAPI *fn_WSAStartup)(WORD, LPWSADATA);
  typedef SOCKET (WSAAPI *fn_socket)(int, int, int);
  typedef int    (WSAAPI *fn_bind)(SOCKET, const struct sockaddr *, int);
  typedef int    (WSAAPI *fn_listen)(SOCKET, int);
  typedef SOCKET (WSAAPI *fn_accept)(SOCKET, struct sockaddr *, int *);
  typedef int    (WSAAPI *fn_recv)(SOCKET, char *, int, int);
  typedef int    (WSAAPI *fn_send)(SOCKET, const char *, int, int);
  typedef int    (WSAAPI *fn_closesocket)(SOCKET);
  typedef int    (WSAAPI *fn_setsockopt)(SOCKET, int, int, const char *, int);
  static fn_WSAStartup  p_WSAStartup;
  static fn_socket      p_socket;
  static fn_bind        p_bind;
  static fn_listen      p_listen;
  static fn_accept      p_accept;
  static fn_recv        p_recv;
  static fn_send        p_send;
  static fn_closesocket p_closesocket;
  static fn_setsockopt  p_setsockopt;
  #define WSAStartup  p_WSAStartup
  #define socket      p_socket
  #define bind        p_bind
  #define listen      p_listen
  #define accept      p_accept
  #define recv        p_recv
  #define send        p_send
  #define closesocket p_closesocket
  #define setsockopt  p_setsockopt
  #define CLOSESOCK closesocket

  static int loadWinsock(void) {
      HMODULE ws = LoadLibraryA("ws2_32.dll");
      if (!ws) return 0;
      p_WSAStartup  = (fn_WSAStartup)GetProcAddress(ws, "WSAStartup");
      p_socket      = (fn_socket)GetProcAddress(ws, "socket");
      p_bind        = (fn_bind)GetProcAddress(ws, "bind");
      p_listen      = (fn_listen)GetProcAddress(ws, "listen");
      p_accept      = (fn_accept)GetProcAddress(ws, "accept");
      p_recv        = (fn_recv)GetProcAddress(ws, "recv");
      p_send        = (fn_send)GetProcAddress(ws, "send");
      p_closesocket = (fn_closesocket)GetProcAddress(ws, "closesocket");
      p_setsockopt  = (fn_setsockopt)GetProcAddress(ws, "setsockopt");
      return p_WSAStartup && p_socket && p_bind && p_listen && p_accept &&
             p_recv && p_send && p_closesocket && p_setsockopt;
  }
#else
  #include <sys/types.h>
  #include <sys/time.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCK close
#endif

#ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
#endif

#define PORT        8765
#define MAX_BOOKS   500
#define MAX_USERS   50
#define MAX_TOKENS  50
#define BUF_SIZE    16384
#define BOOKS_FILE  "books.txt"
#define USERS_FILE  "users.txt"


typedef struct {
    int id;
    char title[100];
    char author[60];
    char category[20];
    int qty;
    int issued;
} Book;

typedef struct {
    char username[50];
    char password[50];
} User;

typedef struct {
    char token[33];
    char user[50];
    int used;
} Session;

static Book books[MAX_BOOKS];
static int bookCount = 0;
static User users[MAX_USERS];
static int userCount = 0;
static Session sessions[MAX_TOKENS];
static int nextSession = 0;

static const char *CATS[] = {"CSE", "BBA", "English", "Law", "Pharmacy"};

static void clean(char *s) {
    for (; *s; s++)
        if (*s == '|' || *s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
}

static void trim(char *s) {
    size_t len = strlen(s), start = 0;
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start) memmove(s, s + start, strlen(s + start) + 1);
}

static int ciEqual(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int ciPrefix(const char *s, const char *pre) {
    while (*pre) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
        s++; pre++;
    }
    return 1;
}

static int validCategory(const char *c) {
    int i;
    for (i = 0; i < 5; i++)
        if (strcmp(CATS[i], c) == 0) return 1;
    return 0;
}

static void urlDecode(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (*src && i < n - 1) {
        if (*src == '+') { dst[i++] = ' '; src++; }
        else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char h[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(h, NULL, 16);
            src += 3;
        } else dst[i++] = *src++;
    }
    dst[i] = '\0';
}

static int getParam(const char *data, const char *key, char *out, size_t n) {
    size_t klen = strlen(key);
    const char *p = data;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t len = strcspn(v, "&");
            char tmp[512];
            if (len >= sizeof tmp) len = sizeof tmp - 1;
            memcpy(tmp, v, len);
            tmp[len] = '\0';
            urlDecode(out, tmp, n);
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = '\0';
    return 0;
}

static void jsonEscape(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; *src && i + 7 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[i++] = '\\'; dst[i++] = (char)c; }
        else if (c < 0x20) { i += (size_t)sprintf(dst + i, "\\u%04x", c); }
        else dst[i++] = (char)c;
    }
    dst[i] = '\0';
}

static int nextBookId(void) {
    int i, max = 0;
    for (i = 0; i < bookCount; i++)
        if (books[i].id > max) max = books[i].id;
    return max + 1;
}

static int findBook(int id) {
    int i;
    for (i = 0; i < bookCount; i++)
        if (books[i].id == id) return i;
    return -1;
}

static void saveBooks(void) {
    FILE *fp = fopen(BOOKS_FILE, "w");
    int i;
    if (!fp) { printf("Cannot save books.txt!\n"); return; }
    for (i = 0; i < bookCount; i++)
        fprintf(fp, "%d|%s|%s|%s|%d|%d\n", books[i].id, books[i].title,
                books[i].author, books[i].category, books[i].qty, books[i].issued);
    fclose(fp);
}

static void addBookData(const char *title, const char *author, const char *cat, int qty) {
    if (bookCount >= MAX_BOOKS) return;
    books[bookCount].id = nextBookId();
    strcpy(books[bookCount].title, title);
    strcpy(books[bookCount].author, author);
    strcpy(books[bookCount].category, cat);
    books[bookCount].qty = qty;
    books[bookCount].issued = 0;
    bookCount++;
}

static void seedBooks(void) {
    bookCount = 0;
    /* CSE */
    addBookData("Introduction to Algorithms", "Thomas H. Cormen", "CSE", 5);
    addBookData("The C Programming Language", "Kernighan & Ritchie", "CSE", 6);
    addBookData("Database System Concepts", "Silberschatz", "CSE", 4);
    addBookData("Computer Networks", "Andrew S. Tanenbaum", "CSE", 3);
    addBookData("Operating System Concepts", "Abraham Silberschatz", "CSE", 4);
    /* BBA */
    addBookData("Principles of Marketing", "Philip Kotler", "BBA", 5);
    addBookData("Financial Management", "Eugene Brigham", "BBA", 4);
    addBookData("Principles of Management", "Stephen Robbins", "BBA", 5);
    addBookData("Business Statistics", "Gupta S.P.", "BBA", 3);
    addBookData("Human Resource Management", "Gary Dessler", "BBA", 4);
    /* English */
    addBookData("Hamlet", "William Shakespeare", "English", 4);
    addBookData("Pride and Prejudice", "Jane Austen", "English", 5);
    addBookData("English Grammar in Use", "Raymond Murphy", "English", 6);
    addBookData("Great Expectations", "Charles Dickens", "English", 3);
    addBookData("Oxford Advanced Learner's Dictionary", "A S Hornby", "English", 5);
    /* Law */
    addBookData("Constitution of Bangladesh", "Govt. of Bangladesh", "Law", 5);
    addBookData("Law of Contract", "Pollock & Mulla", "Law", 3);
    addBookData("Criminal Procedure Code", "Ratanlal", "Law", 4);
    addBookData("Jurisprudence", "Salmond", "Law", 3);
    addBookData("Family Law in Bangladesh", "Mulla", "Law", 3);
    /* Pharmacy */
    addBookData("Pharmaceutical Chemistry", "Beckett & Stenlake", "Pharmacy", 4);
    addBookData("Pharmacology", "Rang & Dale", "Pharmacy", 5);
    addBookData("Pharmacognosy", "Trease & Evans", "Pharmacy", 3);
    addBookData("Physical Pharmacy", "Alfred Martin", "Pharmacy", 3);
    addBookData("Clinical Pharmacy", "Roger Walker", "Pharmacy", 4);
    saveBooks();
}

static void loadBooks(void) {
    FILE *fp = fopen(BOOKS_FILE, "r");
    Book b;
    if (!fp) { seedBooks(); return; }
    while (bookCount < MAX_BOOKS &&
           fscanf(fp, "%d|%99[^|]|%59[^|]|%19[^|]|%d|%d\n", &b.id, b.title,
                  b.author, b.category, &b.qty, &b.issued) == 6)
        books[bookCount++] = b;
    fclose(fp);
    if (bookCount == 0) seedBooks();
}


static void saveUsers(void) {
    FILE *fp = fopen(USERS_FILE, "w");
    int i;
    if (!fp) return;
    for (i = 0; i < userCount; i++)
        fprintf(fp, "%s|%s\n", users[i].username, users[i].password);
    fclose(fp);
}

static void loadUsers(void) {
    FILE *fp = fopen(USERS_FILE, "r");
    char line[128];
    if (fp) {
        while (userCount < MAX_USERS && fgets(line, sizeof line, fp)) {
            User u;
            line[strcspn(line, "\r\n")] = '\0';
            if (sscanf(line, "%49[^|]|%49[^\n]", u.username, u.password) == 2)
                users[userCount++] = u;
        }
        fclose(fp);
    }
    if (userCount == 0) {
        strcpy(users[0].username, "admin");
        strcpy(users[0].password, "admin123");
        userCount = 1;
        saveUsers();
    }
}

static void newToken(const char *user, char *out) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) out[i] = hex[rand() % 16];
    out[32] = '\0';
    strcpy(sessions[nextSession].token, out);
    strncpy(sessions[nextSession].user, user, 49);
    sessions[nextSession].user[49] = '\0';
    sessions[nextSession].used = 1;
    nextSession = (nextSession + 1) % MAX_TOKENS;
}

static int checkToken(const char *t) {
    int i;
    if (!t[0]) return 0;
    for (i = 0; i < MAX_TOKENS; i++)
        if (sessions[i].used && strcmp(sessions[i].token, t) == 0) return 1;
    return 0;
}

static void dropToken(const char *t) {
    int i;
    for (i = 0; i < MAX_TOKENS; i++)
        if (sessions[i].used && strcmp(sessions[i].token, t) == 0) sessions[i].used = 0;
}

static void sendAll(SOCKET s, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(s, data + sent, (int)(len - sent), MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

static void sendResponse(SOCKET s, int status, const char *type, const char *body, size_t len) {
    char head[256];
    const char *text = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                       status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : "Error";
    int n = sprintf(head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                    status, text, type, (unsigned long)len);
    sendAll(s, head, (size_t)n);
    sendAll(s, body, len);
}

static void sendJson(SOCKET s, int status, const char *json) {
    sendResponse(s, status, "application/json; charset=utf-8", json, strlen(json));
}

static void sendError(SOCKET s, int status, const char *msg) {
    char out[300], esc[200];
    jsonEscape(esc, msg, sizeof esc);
    sprintf(out, "{\"ok\":false,\"error\":\"%s\"}", esc);
    sendJson(s, status, out);
}

static void sendOk(SOCKET s) { sendJson(s, 200, "{\"ok\":true}"); }

static int findHeader(const char *buf, const char *name, char *out, size_t n) {
    const char *p = strstr(buf, "\r\n");
    size_t nl = strlen(name);
    out[0] = '\0';
    while (p) {
        p += 2;
        if (p[0] == '\r' || p[0] == '\0') break;
        if (ciPrefix(p, name) && p[nl] == ':') {
            const char *v = p + nl + 1;
            size_t len;
            while (*v == ' ') v++;
            len = strcspn(v, "\r\n");
            if (len >= n) len = n - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 1;
        }
        p = strstr(p, "\r\n");
    }
    return 0;
}

static int readRequest(SOCKET s, char *buf, int cap) {
    int total = 0, headerEnd = -1, contentLen = 0;
    while (total < cap - 1) {
        int n = (int)recv(s, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (headerEnd < 0) {
            char *p = strstr(buf, "\r\n\r\n");
            if (p) {
                char cl[32];
                headerEnd = (int)(p - buf) + 4;
                if (findHeader(buf, "Content-Length", cl, sizeof cl)) contentLen = atoi(cl);
            }
        }
        if (headerEnd >= 0 && total >= headerEnd + contentLen) break;
    }
    return headerEnd < 0 ? -1 : total;
}


static const char APP_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"UTF-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
    "<title>Library Management System</title>\n"
    "<style>\n"
    "  * { box-sizing: border-box; }\n"
    "  body { font-family: Arial, sans-serif; margin: 0; background: #f1f5f9; color: #1e293b; }\n"
    "  .hidden { display: none !important; }\n"
    "  input, select, button { padding: 9px; border: 1px solid #cbd5e1; border-radius: 6px; font-size: 14px; }\n"
    "  button { background: #2563eb; color: #fff; border: none; cursor: pointer; }\n"
    "  button:hover { background: #1d4ed8; }\n"
    "\n"
    "  #loginView { min-height: 100vh; display: flex; align-items: center; justify-content: center; background: linear-gradient(135deg, #1e3a8a, #2563eb); }\n"
    "  .box { background: #fff; width: 100%; max-width: 380px; margin: 16px; padding: 28px; border-radius: 12px; box-shadow: 0 10px 30px rgba(0,0,0,.25); }\n"
    "  .box h1 { margin: 0 0 4px; font-size: 22px; color: #1e3a8a; text-align: center; }\n"
    "  .box p.sub { margin: 0 0 16px; text-align: center; color: #64748b; font-size: 14px; }\n"
    "  .box label { display: block; font-size: 13px; margin: 12px 0 4px; color: #334155; }\n"
    "  .box input { width: 100%; }\n"
    "  .pw { position: relative; }\n"
    "  .pw button { position: absolute; right: 4px; top: 4px; background: none; color: #2563eb; padding: 5px; font-size: 13px; }\n"
    "  .pw button:hover { background: none; }\n"
    "  .submit { width: 100%; margin-top: 18px; padding: 11px; font-size: 15px; }\n"
    "  .msg { margin-top: 12px; font-size: 13px; text-align: center; min-height: 18px; }\n"
    "  .err { color: #dc2626; } .ok { color: #16a34a; }\n"
    "  .switch { text-align: center; margin-top: 14px; font-size: 14px; color: #475569; }\n"
    "  .switch a { color: #2563eb; cursor: pointer; text-decoration: underline; }\n"
    "  .hint { margin-top: 16px; padding: 10px; background: #f1f5f9; border-radius: 6px; font-size: 12px; color: #475569; text-align: center; }\n"
    "\n"
    "  header { background: #1e3a8a; color: #fff; padding: 16px 24px; display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px; }\n"
    "  header h1 { margin: 0; font-size: 22px; }\n"
    "  main { max-width: 1100px; margin: 20px auto; padding: 0 16px; }\n"
    "  .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin-bottom: 16px; }\n"
    "  .stat { background: #fff; border-radius: 8px; padding: 14px; box-shadow: 0 1px 3px rgba(0,0,0,.1); }\n"
    "  .stat b { display: block; font-size: 22px; color: #1e3a8a; }\n"
    "  .card { background: #fff; border-radius: 8px; padding: 16px; margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,.1); }\n"
    "  .card h2 { margin: 0 0 12px; font-size: 17px; }\n"
    "  #addForm { display: grid; grid-template-columns: repeat(auto-fit, minmax(170px, 1fr)); gap: 10px; }\n"
    "  .toolbar { display: flex; gap: 10px; flex-wrap: wrap; margin-bottom: 12px; }\n"
    "  .toolbar input { flex: 1; min-width: 200px; }\n"
    "  .tabs button { background: #e2e8f0; color: #1e293b; }\n"
    "  .tabs button.active { background: #2563eb; color: #fff; }\n"
    "  .table-wrap { overflow-x: auto; }\n"
    "  table { width: 100%; border-collapse: collapse; font-size: 14px; }\n"
    "  th, td { padding: 9px 8px; text-align: left; border-bottom: 1px solid #e2e8f0; }\n"
    "  th { background: #f8fafc; }\n"
    "  td button { padding: 5px 9px; font-size: 12px; margin-right: 3px; }\n"
    "  .btn-return { background: #16a34a; } .btn-del { background: #dc2626; }\n"
    "  .badge { padding: 2px 8px; border-radius: 10px; font-size: 12px; background: #dbeafe; color: #1e40af; }\n"
    "  .out { color: #dc2626; font-weight: bold; }\n"
    "  .empty { text-align: center; padding: 20px; color: #64748b; }\n"
    "  #toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); background: #1e293b; color: #fff; padding: 10px 18px; border-radius: 8px; font-size: 14px; opacity: 0; transition: opacity .25s; pointer-events: none; }\n"
    "  #toast.show { opacity: 1; }\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "\n"
    "<!-- ===== LOGIN ===== -->\n"
    "<div id=\"loginView\">\n"
    "  <div class=\"box\">\n"
    "    <h1>\360\237\223\232 Library System</h1>\n"
    "    <p class=\"sub\" id=\"subtitle\">Please log in</p>\n"
    "    <form id=\"authForm\" autocomplete=\"off\">\n"
    "      <label for=\"username\">Username</label>\n"
    "      <input id=\"username\" required placeholder=\"Username\">\n"
    "      <label for=\"password\">Password</label>\n"
    "      <div class=\"pw\">\n"
    "        <input id=\"password\" type=\"password\" required placeholder=\"Password\">\n"
    "        <button type=\"button\" id=\"togglePw\">Show</button>\n"
    "      </div>\n"
    "      <div id=\"confirmWrap\" class=\"hidden\">\n"
    "        <label for=\"confirm\">Confirm Password</label>\n"
    "        <input id=\"confirm\" type=\"password\" placeholder=\"Re-enter password\">\n"
    "      </div>\n"
    "      <button class=\"submit\" type=\"submit\" id=\"submitBtn\">Login</button>\n"
    "      <div class=\"msg\" id=\"msg\"></div>\n"
    "    </form>\n"
    "    <div class=\"switch\" id=\"switchText\">No account\? <a id=\"switchLink\">Create a new account</a></div>\n"
    "    <div class=\"hint\">Default admin: <b>admin</b> / <b>admin123</b></div>\n"
    "  </div>\n"
    "</div>\n"
    "\n"
    "<!-- ===== LIBRARY ===== -->\n"
    "<div id=\"appView\" class=\"hidden\">\n"
    "  <header>\n"
    "    <h1>\360\237\223\232 Library Management System</h1>\n"
    "    <div><span id=\"userName\" style=\"margin-right:10px\"></span><button id=\"logoutBtn\" style=\"background:#dc2626\">Logout</button></div>\n"
    "  </header>\n"
    "  <main>\n"
    "    <div class=\"stats\" id=\"stats\"></div>\n"
    "\n"
    "    <div class=\"card\">\n"
    "      <h2>Add a New Book</h2>\n"
    "      <form id=\"addForm\">\n"
    "        <input id=\"title\" placeholder=\"Book title\" required maxlength=\"99\">\n"
    "        <input id=\"author\" placeholder=\"Author\" required maxlength=\"59\">\n"
    "        <select id=\"category\">\n"
    "          <option>CSE</option><option>BBA</option><option>English</option>\n"
    "          <option>Law</option><option>Pharmacy</option>\n"
    "        </select>\n"
    "        <input id=\"qty\" type=\"number\" min=\"1\" max=\"1000\" value=\"1\" required>\n"
    "        <button type=\"submit\">Add Book</button>\n"
    "      </form>\n"
    "    </div>\n"
    "\n"
    "    <div class=\"card\">\n"
    "      <div class=\"toolbar tabs\" id=\"tabs\"></div>\n"
    "      <div class=\"toolbar\">\n"
    "        <input id=\"search\" placeholder=\"Search by title or author...\">\n"
    "        <button id=\"resetBtn\" style=\"background:#64748b\">Reset Data</button>\n"
    "      </div>\n"
    "      <div class=\"table-wrap\">\n"
    "        <table>\n"
    "          <thead><tr><th>ID</th><th>Title</th><th>Author</th><th>Category</th><th>Total</th><th>Issued</th><th>Available</th><th>Action</th></tr></thead>\n"
    "          <tbody id=\"tbody\"></tbody>\n"
    "        </table>\n"
    "      </div>\n"
    "    </div>\n"
    "  </main>\n"
    "</div>\n"
    "<div id=\"toast\"></div>\n"
    "\n"
    "<script>\n"
    "/* ---------- token (session) ---------- */\n"
    "let token = \"\";\n"
    "try { token = sessionStorage.getItem(\"libToken\") || \"\"; } catch (e) {}\n"
    "let currentUser = \"\";\n"
    "try { currentUser = sessionStorage.getItem(\"libUser\") || \"\"; } catch (e) {}\n"
    "function saveSession(t, u) {\n"
    "  token = t; currentUser = u;\n"
    "  try { if (t) { sessionStorage.setItem(\"libToken\", t); sessionStorage.setItem(\"libUser\", u); }\n"
    "        else { sessionStorage.removeItem(\"libToken\"); sessionStorage.removeItem(\"libUser\"); } } catch (e) {}\n"
    "}\n"
    "\n"
    "/* ---------- API helper (talks to the C server) ---------- */\n"
    "async function api(path, data) {\n"
    "  const opts = { method: data === undefined \? \"GET\" : \"POST\", headers: { \"X-Token\": token } };\n"
    "  if (data !== undefined) {\n"
    "    opts.headers[\"Content-Type\"] = \"application/x-www-form-urlencoded\";\n"
    "    opts.body = new URLSearchParams(data).toString();\n"
    "  }\n"
    "  let res;\n"
    "  try { res = await fetch(path, opts); }\n"
    "  catch (e) { return { ok: false, error: \"Server is not running! Please run the server program.\" }; }\n"
    "  let json;\n"
    "  try { json = await res.json(); } catch (e) { json = { ok: false, error: \"Invalid server response!\" }; }\n"
    "  if (res.status === 401 && path !== \"/api/login\") { showLogin(); }\n"
    "  json._status = res.status;\n"
    "  return json;\n"
    "}\n"
    "\n"
    "function toast(text) {\n"
    "  const t = document.getElementById(\"toast\");\n"
    "  t.textContent = text; t.classList.add(\"show\");\n"
    "  clearTimeout(toast.timer);\n"
    "  toast.timer = setTimeout(() => t.classList.remove(\"show\"), 2200);\n"
    "}\n"
    "\n"
    "/* ---------- Login / Register ---------- */\n"
    "let registerMode = false;\n"
    "const msg = document.getElementById(\"msg\");\n"
    "function showMsg(t, ok) { msg.textContent = t; msg.className = \"msg \" + (ok \? \"ok\" : \"err\"); }\n"
    "\n"
    "function setMode(reg) {\n"
    "  registerMode = reg;\n"
    "  document.getElementById(\"confirmWrap\").classList.toggle(\"hidden\", !reg);\n"
    "  document.getElementById(\"subtitle\").textContent = reg \? \"Create a new account\" : \"Please log in\";\n"
    "  document.getElementById(\"submitBtn\").textContent = reg \? \"Register\" : \"Login\";\n"
    "  document.getElementById(\"switchText\").innerHTML = reg\n"
    "    \? 'Already have an account\? <a id=\"switchLink\">Log in</a>'\n"
    "    : 'No account\? <a id=\"switchLink\">Create a new account</a>';\n"
    "  document.getElementById(\"switchLink\").onclick = () => setMode(!registerMode);\n"
    "  msg.textContent = \"\";\n"
    "}\n"
    "document.getElementById(\"switchLink\").onclick = () => setMode(true);\n"
    "\n"
    "document.getElementById(\"togglePw\").onclick = (e) => {\n"
    "  const pw = document.getElementById(\"password\");\n"
    "  const show = pw.type === \"password\";\n"
    "  pw.type = show \? \"text\" : \"password\";\n"
    "  e.target.textContent = show \? \"Hide\" : \"Show\";\n"
    "};\n"
    "\n"
    "document.getElementById(\"authForm\").addEventListener(\"submit\", async (e) => {\n"
    "  e.preventDefault();\n"
    "  const username = document.getElementById(\"username\").value.trim();\n"
    "  const password = document.getElementById(\"password\").value;\n"
    "\n"
    "  if (registerMode) {\n"
    "    if (password !== document.getElementById(\"confirm\").value) return showMsg(\"Passwords do not match!\", false);\n"
    "    const r = await api(\"/api/register\", { username, password });\n"
    "    if (!r.ok) return showMsg(r.error, false);\n"
    "    showMsg(\"Account created! Please log in now.\", true);\n"
    "    document.getElementById(\"authForm\").reset();\n"
    "    setTimeout(() => setMode(false), 1000);\n"
    "    return;\n"
    "  }\n"
    "\n"
    "  const r = await api(\"/api/login\", { username, password });\n"
    "  if (!r.ok) return showMsg(r.error, false);\n"
    "  saveSession(r.token, r.username);\n"
    "  showApp();\n"
    "});\n"
    "\n"
    "function showLogin() {\n"
    "  saveSession(\"\", \"\");\n"
    "  document.getElementById(\"appView\").classList.add(\"hidden\");\n"
    "  document.getElementById(\"loginView\").classList.remove(\"hidden\");\n"
    "  document.getElementById(\"authForm\").reset();\n"
    "  setMode(false);\n"
    "}\n"
    "\n"
    "async function showApp() {\n"
    "  if (!token) return showLogin();\n"
    "  document.getElementById(\"loginView\").classList.add(\"hidden\");\n"
    "  document.getElementById(\"appView\").classList.remove(\"hidden\");\n"
    "  document.getElementById(\"userName\").textContent = \"\360\237\221\244 \" + currentUser;\n"
    "  await refresh();\n"
    "}\n"
    "\n"
    "document.getElementById(\"logoutBtn\").addEventListener(\"click\", async () => {\n"
    "  await api(\"/api/logout\", {});\n"
    "  showLogin();\n"
    "});\n"
    "\n"
    "/* ---------- Books ---------- */\n"
    "const CATEGORIES = [\"All\", \"CSE\", \"BBA\", \"English\", \"Law\", \"Pharmacy\"];\n"
    "let books = [], activeCategory = \"All\", searchText = \"\";\n"
    "\n"
    "async function refresh() {\n"
    "  const r = await api(\"/api/books\");\n"
    "  if (Array.isArray(r)) { books = r; render(); }\n"
    "  else if (r && r.error) toast(r.error);\n"
    "}\n"
    "\n"
    "function esc(s) {\n"
    "  return String(s).replace(/[&<>\"']/g, (c) => ({ \"&\": \"&amp;\", \"<\": \"&lt;\", \">\": \"&gt;\", '\"': \"&quot;\", \"'\": \"&#39;\" }[c]));\n"
    "}\n"
    "\n"
    "function renderTabs() {\n"
    "  const tabs = document.getElementById(\"tabs\");\n"
    "  tabs.innerHTML = CATEGORIES.map((c) => `<button class=\"${c === activeCategory \? \"active\" : \"\"}\" data-cat=\"${c}\">${c}</button>`).join(\"\");\n"
    "  tabs.querySelectorAll(\"button\").forEach((b) => { b.onclick = () => { activeCategory = b.dataset.cat; render(); }; });\n"
    "}\n"
    "\n"
    "function renderStats() {\n"
    "  const total = books.reduce((s, b) => s + b.qty, 0);\n"
    "  const issued = books.reduce((s, b) => s + b.issued, 0);\n"
    "  document.getElementById(\"stats\").innerHTML =\n"
    "    `<div class=\"stat\"><b>${books.length}</b>Total Titles</div>\n"
    "     <div class=\"stat\"><b>${total}</b>Total Copies</div>\n"
    "     <div class=\"stat\"><b>${issued}</b>Issued</div>\n"
    "     <div class=\"stat\"><b>${total - issued}</b>Available</div>`;\n"
    "}\n"
    "\n"
    "function renderTable() {\n"
    "  const q = searchText.toLowerCase();\n"
    "  const list = books.filter((b) =>\n"
    "    (activeCategory === \"All\" || b.category === activeCategory) &&\n"
    "    (b.title.toLowerCase().includes(q) || b.author.toLowerCase().includes(q)));\n"
    "  const tbody = document.getElementById(\"tbody\");\n"
    "  if (!list.length) { tbody.innerHTML = `<tr><td colspan=\"8\" class=\"empty\">No books found.</td></tr>`; return; }\n"
    "  tbody.innerHTML = list.map((b) => {\n"
    "    const avail = b.qty - b.issued;\n"
    "    return `<tr>\n"
    "      <td>${b.id}</td><td>${esc(b.title)}</td><td>${esc(b.author)}</td>\n"
    "      <td><span class=\"badge\">${esc(b.category)}</span></td>\n"
    "      <td>${b.qty}</td><td>${b.issued}</td><td class=\"${avail === 0 \? \"out\" : \"\"}\">${avail}</td>\n"
    "      <td>\n"
    "        <button onclick=\"action('issue',${b.id})\">Issue</button>\n"
    "        <button class=\"btn-return\" onclick=\"action('return',${b.id})\">Return</button>\n"
    "        <button class=\"btn-del\" onclick=\"action('delete',${b.id})\">Delete</button>\n"
    "      </td></tr>`;\n"
    "  }).join(\"\");\n"
    "}\n"
    "\n"
    "function render() { renderTabs(); renderStats(); renderTable(); }\n"
    "\n"
    "async function action(type, id) {\n"
    "  if (type === \"delete\" && !confirm(\"Delete this book\?\")) return;\n"
    "  const r = await api(\"/api/\" + type, { id });\n"
    "  if (!r.ok) toast(r.error);\n"
    "  await refresh();\n"
    "}\n"
    "\n"
    "document.getElementById(\"addForm\").addEventListener(\"submit\", async (e) => {\n"
    "  e.preventDefault();\n"
    "  const r = await api(\"/api/books\", {\n"
    "    title: document.getElementById(\"title\").value.trim(),\n"
    "    author: document.getElementById(\"author\").value.trim(),\n"
    "    category: document.getElementById(\"category\").value,\n"
    "    qty: document.getElementById(\"qty\").value,\n"
    "  });\n"
    "  if (!r.ok) return toast(r.error);\n"
    "  e.target.reset();\n"
    "  document.getElementById(\"qty\").value = 1;\n"
    "  toast(\"Book added!\");\n"
    "  await refresh();\n"
    "});\n"
    "\n"
    "document.getElementById(\"search\").addEventListener(\"input\", (e) => { searchText = e.target.value; renderTable(); });\n"
    "\n"
    "document.getElementById(\"resetBtn\").addEventListener(\"click\", async () => {\n"
    "  if (!confirm(\"Reset all data and restore the sample data\?\")) return;\n"
    "  await api(\"/api/reset\", {});\n"
    "  await refresh();\n"
    "});\n"
    "\n"
    "/* ---------- Start ---------- */\n"
    "if (token) showApp(); else showLogin();\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static void serveHtml(SOCKET c) {
    sendResponse(c, 200, "text/html; charset=utf-8", APP_HTML, sizeof(APP_HTML) - 1);
}

static void sendBooks(SOCKET c) {
    size_t cap = (size_t)bookCount * 600 + 16, len = 0;
    char *out = (char *)malloc(cap);
    int i;
    if (!out) { sendError(c, 500, "Memory error"); return; }
    len += (size_t)sprintf(out + len, "[");
    for (i = 0; i < bookCount; i++) {
        char t[260], a[180];
        jsonEscape(t, books[i].title, sizeof t);
        jsonEscape(a, books[i].author, sizeof a);
        len += (size_t)sprintf(out + len,
            "%s{\"id\":%d,\"title\":\"%s\",\"author\":\"%s\",\"category\":\"%s\",\"qty\":%d,\"issued\":%d}",
            i ? "," : "", books[i].id, t, a, books[i].category, books[i].qty, books[i].issued);
    }
    strcat(out, "]");
    sendJson(c, 200, out);
    free(out);
}

static void handle(SOCKET c) {
    static char buf[BUF_SIZE];
    char method[8] = "", path[256] = "", token[64] = "", v1[128], v2[128], v3[128], v4[32];
    char *body, *q;
    int total = readRequest(c, buf, sizeof buf);
    if (total < 0) return;

    sscanf(buf, "%7s %255s", method, path);
    q = strchr(path, '?');
    if (q) *q = '\0';
    findHeader(buf, "X-Token", token, sizeof token);
    body = strstr(buf, "\r\n\r\n") + 4;

    /* ---- pages ---- */
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        serveHtml(c);
        return;
    }

    /* ---- login / register (no token needed) ---- */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/login") == 0) {
        int i;
        getParam(body, "username", v1, sizeof v1);
        getParam(body, "password", v2, sizeof v2);
        for (i = 0; i < userCount; i++)
            if (strcmp(users[i].username, v1) == 0 && strcmp(users[i].password, v2) == 0) {
                char tk[33], out[200], esc[100];
                newToken(users[i].username, tk);
                jsonEscape(esc, users[i].username, sizeof esc);
                sprintf(out, "{\"ok\":true,\"token\":\"%s\",\"username\":\"%s\"}", tk, esc);
                sendJson(c, 200, out);
                return;
            }
        sendError(c, 401, "Wrong username or password!");
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/register") == 0) {
        int i;
        getParam(body, "username", v1, sizeof v1);
        getParam(body, "password", v2, sizeof v2);
        clean(v1); trim(v1);
        if (strlen(v1) < 3 || strlen(v1) > 40) { sendError(c, 400, "Username must be 3-40 characters!"); return; }
        if (strlen(v2) < 4 || strlen(v2) > 40) { sendError(c, 400, "Password must be 4-40 characters!"); return; }
        clean(v2);
        for (i = 0; i < userCount; i++)
            if (ciEqual(users[i].username, v1)) { sendError(c, 400, "This username already exists!"); return; }
        if (userCount >= MAX_USERS) { sendError(c, 400, "User limit reached!"); return; }
        strcpy(users[userCount].username, v1);
        strcpy(users[userCount].password, v2);
        userCount++;
        saveUsers();
        sendOk(c);
        return;
    }

    /* ---- everything below needs login ---- */
    if (strncmp(path, "/api/", 5) != 0) { sendError(c, 404, "Not found"); return; }
    if (!checkToken(token)) { sendError(c, 401, "Please log in"); return; }

    if (strcmp(path, "/api/logout") == 0) { dropToken(token); sendOk(c); return; }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/books") == 0) { sendBooks(c); return; }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/books") == 0) {
        Book b;
        getParam(body, "title", v1, sizeof v1);
        getParam(body, "author", v2, sizeof v2);
        getParam(body, "category", v3, sizeof v3);
        getParam(body, "qty", v4, sizeof v4);
        clean(v1); trim(v1); clean(v2); trim(v2);
        if (!v1[0] || !v2[0]) { sendError(c, 400, "Title and author are required!"); return; }
        if (strlen(v1) > 99 || strlen(v2) > 59) { sendError(c, 400, "Title/author is too long!"); return; }
        if (!validCategory(v3)) { sendError(c, 400, "Invalid category!"); return; }
        b.qty = atoi(v4);
        if (b.qty < 1 || b.qty > 1000) { sendError(c, 400, "Number of copies must be 1-1000!"); return; }
        if (bookCount >= MAX_BOOKS) { sendError(c, 400, "Library is full!"); return; }
        addBookData(v1, v2, v3, b.qty);
        saveBooks();
        sendOk(c);
        return;
    }

    if (strcmp(method, "POST") == 0 &&
        (strcmp(path, "/api/issue") == 0 || strcmp(path, "/api/return") == 0 ||
         strcmp(path, "/api/delete") == 0)) {
        int idx, k;
        getParam(body, "id", v1, sizeof v1);
        idx = findBook(atoi(v1));
        if (idx < 0) { sendError(c, 400, "Book not found!"); return; }
        if (strcmp(path, "/api/issue") == 0) {
            if (books[idx].qty - books[idx].issued <= 0) { sendError(c, 400, "This book is not available right now!"); return; }
            books[idx].issued++;
        } else if (strcmp(path, "/api/return") == 0) {
            if (books[idx].issued <= 0) { sendError(c, 400, "No copy of this book is issued!"); return; }
            books[idx].issued--;
        } else {
            for (k = idx; k < bookCount - 1; k++) books[k] = books[k + 1];
            bookCount--;
        }
        saveBooks();
        sendOk(c);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reset") == 0) {
        seedBooks();
        sendOk(c);
        return;
    }

    sendError(c, 404, "Not found");
}

/* ===================== main ===================== */
int main(void) {
    SOCKET srv;
    struct sockaddr_in addr;
#ifdef _WIN32
    WSADATA wsa;
    if (!loadWinsock()) { printf("Could not load ws2_32.dll!\n"); getchar(); return 1; }
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    int yes = 1;
#endif
    srand((unsigned)time(NULL));
    loadBooks();
    loadUsers();

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { printf("Failed to create socket!\n"); return 1; }
#ifndef _WIN32
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
#ifdef _WIN32
    addr.sin_addr.s_addr = 0x0100007F;               /* 127.0.0.1 (only your own computer) */
    addr.sin_port = (unsigned short)(((PORT & 0xFF) << 8) | ((PORT >> 8) & 0xFF));
#else
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* only reachable from your own computer */
    addr.sin_port = htons(PORT);
#endif
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        printf("Port %d is already in use. Close the other program or change PORT.\n", PORT);
#ifdef _WIN32
        getchar();
#endif
        return 1;
    }
    listen(srv, 10);

    printf("=====================================\n");
    printf(" Library server started!\n");
    printf(" Open in browser: http://localhost:%d\n", PORT);
    printf(" Login: admin / admin123\n");
    printf(" To stop: Ctrl + C\n");
    printf("=====================================\n");

#ifdef _WIN32
    {   /* the browser will open automatically */
        char cmd[100];
        sprintf(cmd, "start http://localhost:%d", PORT);
        system(cmd);
    }
#endif

    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
#ifdef _WIN32
        { DWORD tv = 2000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv); }
#else
        { struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
#endif
        handle(c);
        CLOSESOCK(c);
    }
    return 0;
}
