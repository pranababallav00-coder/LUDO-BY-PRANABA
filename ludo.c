#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <process.h>
  typedef SOCKET sock_t;
  #define SOCK_BAD INVALID_SOCKET
  #define sock_close closesocket
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/time.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  typedef int sock_t;
  #define SOCK_BAD (-1)
  #define sock_close close
#endif

#define NP 4            
#define NT 4             
#define TRACK 52
#define LAST_MAIN 50     
#define HOME 56
#define YARD (-1)
#define EVN 12         

enum { PH_ROLL = 0, PH_MOVE = 1, PH_OVER = 2, PH_SETUP = 3 };

enum { EV_ROLL, EV_OUT, EV_MOVE, EV_CAPTURE, EV_HOME, EV_FINISH, EV_AGAIN,
       EV_NOMOVE, EV_SIX3, EV_OVER, EV_NEW };

typedef struct {
    int pos[NP][NT];
    int active[NP], rank[NP];
    int cur, phase, dice, sixes, nfinished;
    int legal;                     
    int ev[EVN][4];
    int ev_count;
} Game;

static Game G;
static unsigned g_ver = 1, g_rolls = 0;       
static const char *index_path = "index.html";
static char exe_dir[512];
static volatile int g_quit = 0;

static const int START[NP] = {0, 13, 26, 39};

static uint64_t rs[4];
static uint64_t rotl64(uint64_t x, int k) {
     return (x << k) | (x >> (64 - k)); 
    }

static uint64_t rnext(void) {
    uint64_t r = rotl64(rs[1] * 5, 7) * 9, t = rs[1] << 17;
    rs[2] ^= rs[0]; rs[3] ^= rs[1]; rs[1] ^= rs[2]; rs[0] ^= rs[3];
    rs[2] ^= t; rs[3] = rotl64(rs[3], 45);
    return r;
}

static void rseed(void) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(rs, sizeof rs, 1, f) != 1) {
        uint64_t s = (uint64_t)time(NULL) * 6364136223846793005ULL ^ (uint64_t)clock()
                   ^ (uint64_t)(uintptr_t)&s;
        #ifdef _WIN32
            { LARGE_INTEGER q; QueryPerformanceCounter(&q); s ^= (uint64_t)q.QuadPart;
                s ^= (uint64_t)GetCurrentProcessId() << 32;
            }
        #else
            s ^= (uint64_t)getpid() << 32;
        #endif
            for (int i = 0; i < 4; i++) {                
                uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                rs[i] = z ^ (z >> 31);
            }
    }
    if (f) fclose(f);
    if (!(rs[0] | rs[1] | rs[2] | rs[3])) rs[0] = 1;
}

static int roll_die(void) {
    const uint64_t lim = UINT64_MAX - (UINT64_MAX % 6);
    uint64_t x;
    do { x = rnext(); } while (x >= lim);
    return (int)(x % 6) + 1;
}

static void ev_add(int type, int p, int a, int b) {
    int *e = G.ev[G.ev_count % EVN];
    e[0] = type; e[1] = p; e[2] = a; e[3] = b;
    G.ev_count++;
}

static int is_safe(int g) { 
    int m = g % 13; return m == 0 || m == 8; 
}

static int gidx(int p, int prog) { 
    return (START[p] + prog) % TRACK; 
}    
static int popc(int x) { int c = 0; 
    while (x) {
        c += x & 1; x >>= 1; 
    } 
    return c; 
}

static int legal_mask(int p, int d) {
    int m = 0;
    for (int t = 0; t < NT; t++) {
        int ps = G.pos[p][t];
        if (ps == YARD) { if (d == 6) m |= 1 << t; }
        else if (ps < HOME && ps + d <= HOME) m |= 1 << t;
    }
    return m;
}

static void next_turn(void) {
    for (int i = 0; i < NP; i++) {
        G.cur = (G.cur + 1) % NP;
        if (G.active[G.cur] && !G.rank[G.cur]) break;
    }
    G.sixes = 0; G.phase = PH_ROLL; G.legal = 0;
}

static int new_game(int mask) {
    if (popc(mask & 15) < 2) 
    return -1;

    memset(&G, 0, sizeof G);
    for (int p = 0; p < NP; p++) {
        G.active[p] = (mask >> p) & 1;
        for (int t = 0; t < NT; t++) G.pos[p][t] = YARD;
    }
    G.cur = 0;
    while (!G.active[G.cur]) G.cur++;

    G.phase = PH_ROLL;
    ev_add(EV_NEW, G.cur, 0, 0);
    g_ver++;
    return 0;
}

static const char *act_roll(void) {
    if (G.phase != PH_ROLL) return "Not time to roll";
    int p = G.cur, d = roll_die();
    G.dice = d; g_rolls++;
    G.sixes = (d == 6) ? G.sixes + 1 : 0;
    ev_add(EV_ROLL, p, d, 0);
    if (G.sixes >= 3) {
        ev_add(EV_SIX3, p, 0, 0);
        next_turn();
    } else {
        G.legal = legal_mask(p, d);
        if (G.legal) G.phase = PH_MOVE;
        else if (d == 6) { ev_add(EV_NOMOVE, p, 0, 0); ev_add(EV_AGAIN, p, 0, 0); }
        else { ev_add(EV_NOMOVE, p, 0, 0); next_turn(); }
    }
    g_ver++;
    return NULL;
}

static const char *act_move(int t) {
    if (G.phase != PH_MOVE) return "Not time to move";
    if (t < 0 || t >= NT || !((G.legal >> t) & 1)) return "That token cannot move";
    int p = G.cur, d = G.dice, from = G.pos[p][t];
    int to = (from == YARD) ? 0 : from + d;
    int bonus = (d == 6), captured = 0;

    G.pos[p][t] = to;
    G.legal = 0;
    if (from == YARD) ev_add(EV_OUT, p, t, 0);
    else ev_add(EV_MOVE, p, t, d);

    if (to <= LAST_MAIN && !is_safe(gidx(p, to))) {
        int g = gidx(p, to);
        for (int q = 0; q < NP; q++) {
            if (q == p) continue;
            for (int k = 0; k < NT; k++) {
                int pq = G.pos[q][k];
                if (pq >= 0 && pq <= LAST_MAIN && gidx(q, pq) == g) {
                    G.pos[q][k] = YARD; captured++;
                    ev_add(EV_CAPTURE, p, q, 0);
                }
            }
        }
    }
    if (captured) bonus = 1;
    if (to == HOME) { ev_add(EV_HOME, p, t, 0); bonus = 1; }

    int all = 1;
    for (int k = 0; k < NT; k++) if (G.pos[p][k] != HOME) all = 0;
    if (all) { G.rank[p] = ++G.nfinished; ev_add(EV_FINISH, p, G.rank[p], 0); }

    int rem = 0, last = -1;
    for (int q = 0; q < NP; q++) if (G.active[q] && !G.rank[q]) { rem++; last = q; }
    if (rem <= 1) {
        if (rem == 1) G.rank[last] = ++G.nfinished;
        G.phase = PH_OVER;
        for (int q = 0; q < NP; q++) if (G.rank[q] == 1) ev_add(EV_OVER, q, 0, 0);
    } else if (bonus && !G.rank[p]) {
        G.phase = PH_ROLL;
        ev_add(EV_AGAIN, p, 0, 0);
    } else {
        next_turn();
    }
    g_ver++;
    return NULL;
}

static int build_state(char *buf, int cap, const char *err) {
    int off = 0;
#define W(...) do { if (off < cap) off += snprintf(buf + off, (size_t)(cap - off), __VA_ARGS__); } while (0)
    W("{\"ver\":%u,\"rolls\":%u,\"phase\":%d,\"cur\":%d,\"dice\":%d,\"sixes\":%d,",
      g_ver, g_rolls, G.phase, G.cur, G.dice, G.sixes);
    W("\"active\":[%d,%d,%d,%d],", G.active[0], G.active[1], G.active[2], G.active[3]);
    W("\"rank\":[%d,%d,%d,%d],", G.rank[0], G.rank[1], G.rank[2], G.rank[3]);
    W("\"legal\":[%d,%d,%d,%d],", G.legal & 1, (G.legal >> 1) & 1, (G.legal >> 2) & 1, (G.legal >> 3) & 1);
    W("\"pos\":[");
    for (int p = 0; p < NP; p++)
        W("%s[%d,%d,%d,%d]", p ? "," : "", G.pos[p][0], G.pos[p][1], G.pos[p][2], G.pos[p][3]);
    W("],\"logn\":%d,\"log\":[", G.ev_count);
    int start = G.ev_count > EVN ? G.ev_count - EVN : 0;
    for (int i = start; i < G.ev_count; i++) {
        const int *e = G.ev[i % EVN];
        W("%s[%d,%d,%d,%d]", i > start ? "," : "", e[0], e[1], e[2], e[3]);
    }
    W("],\"err\":\"%s\"}", err ? err : "");
#undef W
    return off < cap ? off : cap - 1;
}

#ifdef MSG_NOSIGNAL
  #define SEND_FLAGS MSG_NOSIGNAL
#else
  #define SEND_FLAGS 0
#endif

static void send_all(sock_t fd, const char *b, size_t n) {
    while (n) {
        int w = (int)send(fd, b, (int)(n > 65536 ? 65536 : n), SEND_FLAGS);
        if (w <= 0) {
#ifndef _WIN32
            if (w < 0 && errno == EINTR) continue;
#endif
            return;
        }
        b += w; n -= (size_t)w;
    }
}

static void respond(sock_t fd, int code, const char *status, const char *ctype, const char *body, size_t len) {
    char h[256];
    int hl = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", code, status, ctype, (unsigned long)len);
    send_all(fd, h, (size_t)hl);
    if (len) send_all(fd, body, len);
}

static FILE *open_index(void) {
    FILE *f = fopen(index_path, "rb");
    if (!f && exe_dir[0]) {
        char p[640];
        snprintf(p, sizeof p, "%s/index.html", exe_dir);
        f = fopen(p, "rb");
    }
    return f;
}

static void serve_index(sock_t fd) {
    FILE *f = open_index();
    if (!f) {
        const char *m = "index.html not found. Keep it in the same folder as the ludo program "
                        "(or pass its path as an argument).";
        respond(fd, 404, "Not Found", "text/plain", m, strlen(m));
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n)
        respond(fd, 200, "OK", "text/html; charset=utf-8", b, (size_t)n);
    else
        respond(fd, 500, "Internal Server Error", "text/plain", "read error", 10);
    free(b);
    fclose(f);
}

static int qint(const char *q, const char *key, int def) {
    size_t kl = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') return atoi(q + kl + 1);
        q = strchr(q, '&');
        if (q) q++;
    }
    return def;
}

static void handle_request(sock_t fd, char *req) {
    char method[8], target[1024];
    if (sscanf(req, "%7s %1023s", method, target) != 2) {
        respond(fd, 400, "Bad Request", "text/plain", "bad request", 11);
        return;
    }
    if (strcmp(method, "GET") && strcmp(method, "POST")) {
        respond(fd, 405, "Method Not Allowed", "text/plain", "method not allowed", 18);
        return;
    }
    char *q = strchr(target, '?');
    if (q) *q++ = 0; else q = target + strlen(target);

    if (!strcmp(target, "/") || !strcmp(target, "/index.html")) { serve_index(fd); return; }
    if (strncmp(target, "/api/", 5)) { respond(fd, 404, "Not Found", "text/plain", "not found", 9); return; }

    char out[2048];
    const char *err = NULL;
    int v = qint(q, "ver", -1);
    int stale = (v >= 0 && (unsigned)v != g_ver);

    if (!strcmp(target, "/api/state")) {
       
    } else if (!strcmp(target, "/api/roll")) {
        err = stale ? "Screen was out of date, refreshed" : act_roll();
    } else if (!strcmp(target, "/api/move")) {
        err = stale ? "Screen was out of date, refreshed" : act_move(qint(q, "t", -1));
    } else if (!strcmp(target, "/api/new")) {
        if (new_game(qint(q, "mask", 15)) < 0) err = "Pick at least 2 players";
    } else if (!strcmp(target, "/api/quit")) {
        respond(fd, 200, "OK", "application/json", "{\"bye\":1}", 9);
        g_quit = 1;
        return;
    } else {
        err = "Unknown request";
    }
    int len = build_state(out, sizeof out, err);
    respond(fd, 200, "OK", "application/json", out, (size_t)len);
}

#define MAXC 32
typedef struct { sock_t fd; char buf[4096]; size_t len; time_t t0; } Conn;

static void serve_forever(sock_t ls) {
    static Conn C[MAXC];
    for (int i = 0; i < MAXC; i++) C[i].fd = SOCK_BAD;

    while (!g_quit) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        sock_t maxfd = ls;
        for (int i = 0; i < MAXC; i++)
            if (C[i].fd != SOCK_BAD) { FD_SET(C[i].fd, &rf); if (C[i].fd > maxfd) maxfd = C[i].fd; }
        struct timeval tv = {1, 0};
        int n = select((int)maxfd + 1, &rf, NULL, NULL, &tv);
        time_t now = time(NULL);

        if (n > 0 && FD_ISSET(ls, &rf)) {
            sock_t c = accept(ls, NULL, NULL);
            if (c != SOCK_BAD) {
                int slot = -1;
                for (int i = 0; i < MAXC; i++) if (C[i].fd == SOCK_BAD) { slot = i; break; }
                if (slot < 0) sock_close(c);
                else {
#ifdef _WIN32
                    DWORD ms = 3000;
                    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
                    struct timeval st = {3, 0};
                    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st);
#endif
                    C[slot].fd = c; C[slot].len = 0; C[slot].t0 = now;
                }
            }
        }
        for (int i = 0; i < MAXC; i++) {
            Conn *c = &C[i];
            if (c->fd == SOCK_BAD) continue;
            if (n > 0 && FD_ISSET(c->fd, &rf)) {
                int r = (int)recv(c->fd, c->buf + c->len, (int)(sizeof c->buf - 1 - c->len), 0);
                if (r <= 0) { sock_close(c->fd); c->fd = SOCK_BAD; continue; }
                c->len += (size_t)r;
                c->buf[c->len] = 0;
                if (strstr(c->buf, "\r\n\r\n") || c->len >= sizeof c->buf - 1) {
                    handle_request(c->fd, c->buf);
                    sock_close(c->fd); c->fd = SOCK_BAD;
                }
            } else if (now - c->t0 > 10) {              
                sock_close(c->fd); c->fd = SOCK_BAD;
            }
        }
    }
    for (int i = 0; i < MAXC; i++) if (C[i].fd != SOCK_BAD) sock_close(C[i].fd);
}

static void open_browser(int port) {
    char cmd[200];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "start \"\" \"http://127.0.0.1:%d/\"", port);
#elif defined(__APPLE__)
    snprintf(cmd, sizeof cmd, "open \"http://127.0.0.1:%d/\" >/dev/null 2>&1 &", port);
#else
    snprintf(cmd, sizeof cmd, "xdg-open \"http://127.0.0.1:%d/\" >/dev/null 2>&1 &", port);
#endif
    if (system(cmd) != 0) {}
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "INVARIANT FAILED: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static void check_invariants(void) {
    int owner[TRACK];
    for (int g = 0; g < TRACK; g++) owner[g] = -1;
    for (int p = 0; p < NP; p++)
        for (int t = 0; t < NT; t++) {
            int ps = G.pos[p][t];
            CHECK(ps >= YARD && ps <= HOME);
            if (!G.active[p]) CHECK(ps == YARD);
            if (ps >= 0 && ps <= LAST_MAIN) {
                int g = gidx(p, ps);
                if (!is_safe(g)) {                 
                    CHECK(owner[g] == -1 || owner[g] == p);
                    owner[g] = p;
                }
            }
        }
}

static int selftest(int games) {
    long wins[NP] = {0}, hist[7] = {0}, steps_total = 0, played = 0, sixes3 = 0, caps = 0;
    for (int i = 0; i < games; i++) {
        static const int masks[] = {15, 15, 15, 5, 7, 11, 13, 3};
        int mask = masks[i % 8];
        CHECK(new_game(mask) == 0);
        long steps = 0;
        int evseen = G.ev_count;
        while (G.phase != PH_OVER) {
            CHECK(++steps < 2000000);
            if (G.phase == PH_ROLL) {
                CHECK(act_roll() == NULL);
                hist[G.dice]++;
            } else {
                CHECK(G.phase == PH_MOVE && G.legal != 0);
                int c = popc(G.legal), k = (int)(rnext() % (uint64_t)c), t;
                for (t = 0; t < NT; t++) if (((G.legal >> t) & 1) && k-- == 0) break;
                CHECK(act_move(t) == NULL);
            }
            for (int e = evseen; e < G.ev_count; e++) {
                int type = G.ev[e % EVN][0];
                if (type == EV_SIX3) sixes3++;
                if (type == EV_CAPTURE) caps++;
            }
            evseen = G.ev_count;
            check_invariants();
        }
        for (int p = 0; p < NP; p++) {
            CHECK(G.active[p] == (G.rank[p] != 0));
            if (G.rank[p] == 1) wins[p]++;
        }
        CHECK(G.nfinished == popc(mask));
        steps_total += steps; played++;
    }
    long tot = 0;
    for (int d = 1; d <= 6; d++) tot += hist[d];
    printf("selftest OK: %ld games (2/3/4 players), avg %.0f actions/game, no invariant violations\n",
           played, (double)steps_total / (double)played);
    printf("captures: %ld, three-sixes forfeits: %ld\n", caps, sixes3);
    printf("wins by seat  Red=%ld Green=%ld Yellow=%ld Blue=%ld\n", wins[0], wins[1], wins[2], wins[3]);
    printf("dice  ");
    for (int d = 1; d <= 6; d++) printf("%d:%.2f%%  ", d, 100.0 * (double)hist[d] / (double)tot);
    printf("(ideal 16.67%%)\n");
    return 0;
}

int main(int argc, char **argv) {
    int port = 8080, browser = 1;
    rseed();
    memset(&G, 0, sizeof G);
    G.phase = PH_SETUP;                             

    if (argc > 0 && argv[0]) {                   
        const char *s1 = strrchr(argv[0], '/'), *s2 = strrchr(argv[0], '\\');
        const char *s = s1 > s2 ? s1 : s2;
        if (s) {
            size_t n = (size_t)(s - argv[0]);
            if (n < sizeof exe_dir) { memcpy(exe_dir, argv[0], n); exe_dir[n] = 0; }
        }
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) return selftest(i + 1 < argc ? atoi(argv[i + 1]) : 2000);
        else if (!strcmp(argv[i], "--no-browser")) browser = 0;
        else if (atoi(argv[i]) > 0) port = atoi(argv[i]);
        else index_path = argv[i];
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "Winsock failed to start\n"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == SOCK_BAD) { perror("socket"); return 1; }
#ifndef _WIN32
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
    int bound = 0;
    for (int tries = 0; tries < 20 && !bound; tries++, port++) {    
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);              
        a.sin_port = htons((unsigned short)port);
        if (bind(ls, (struct sockaddr *)&a, sizeof a) == 0) { bound = 1; port--; }
    }
    if (!bound || listen(ls, 16) != 0) { fprintf(stderr, "Could not open a local port.\n"); return 1; }

    printf("Ludo is running at http://127.0.0.1:%d/\n"
           "Keep this window open while you play. Stop with Menu > Quit or Ctrl+C.\n", port);
    fflush(stdout);
    if (browser) open_browser(port);

    serve_forever(ls);
    sock_close(ls);
#ifdef _WIN32
    WSACleanup();
#endif
    printf("Ludo stopped.\n");
    return 0;
}
