/*
 * ============================================
 *  GC VISUALIZER — C Backend Server
 * ============================================
 *  A simple HTTP server implementing three
 *  garbage collection algorithms:
 *    1. Reference Counting
 *    2. Mark & Sweep
 *    3. Generational GC
 *
 *  The frontend sends the object graph via POST
 *  and this server computes which objects to collect.
 *
 *  Compile: gcc server.c -o server
 *  Run:     ./server  (listens on PORT env or 8080)
 * ============================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ── Constants ── */
#define MAX_OBJ     64
#define MAX_EDGE    256
#define BUF_SIZE    65536
#define RES_SIZE    65536

/* ── Data Structures ── */

typedef struct {
    int id;
    int isRoot;
    int refCount;
    int marked;
    int gen;      /* 0 = young, 1 = old */
    int age;
} GCObject;

typedef struct {
    int from, to;
} Edge;

/* Per-request state */
static GCObject objects[MAX_OBJ];
static int      numObjects = 0;
static Edge     edges[MAX_EDGE];
static int      numEdges = 0;

/* ── Utility: find object index by id ── */
static int find_idx(int id) {
    for (int i = 0; i < numObjects; i++)
        if (objects[i].id == id) return i;
    return -1;
}

/* ══════════════════════════════════════════
 *  SECTION 1: Simple JSON Parsing
 *  (manual strstr/sscanf — no dependencies)
 * ══════════════════════════════════════════ */

/* Skip whitespace */
static const char* skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Find integer value after "key": in a JSON fragment */
static int json_int(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    p = skip_ws(p);
    if (*p == ':') p++;
    p = skip_ws(p);
    return atoi(p);
}

/* Check if "key":true or "key":1 */
static int json_bool(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    p = skip_ws(p);
    if (*p == ':') p++;
    p = skip_ws(p);
    if (*p == 't') return 1;  /* true */
    return atoi(p);
}

/* Find string value after "key": */
static int json_str(const char *json, const char *key, char *out, int maxlen) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) { out[0] = 0; return 0; }
    p += strlen(pat);
    while (*p && *p != '"') p++;
    if (*p != '"') { out[0] = 0; return 0; }
    p++; /* skip opening quote */
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

/*
 * Parse the full request body JSON:
 * {
 *   "objects": [ {"id":1,"isRoot":1,"gen":"young","age":0}, ... ],
 *   "edges":   [ {"from":1,"to":2}, ... ]
 * }
 */
static void parse_body(const char *body) {
    numObjects = 0;
    numEdges = 0;
    if (!body) return;

    /* Parse objects array */
    const char *arr = strstr(body, "\"objects\"");
    if (arr) {
        arr = strchr(arr, '[');
        if (arr) {
            const char *p = arr + 1;
            while (p && *p && *p != ']') {
                const char *obj_start = strchr(p, '{');
                if (!obj_start) break;
                const char *obj_end = strchr(obj_start, '}');
                if (!obj_end) break;

                /* Extract a single object's JSON into a buffer */
                int len = (int)(obj_end - obj_start + 1);
                char buf[512];
                if (len > 511) len = 511;
                strncpy(buf, obj_start, len);
                buf[len] = 0;

                if (numObjects < MAX_OBJ) {
                    GCObject *o = &objects[numObjects];
                    o->id       = json_int(buf, "id");
                    o->isRoot   = json_bool(buf, "isRoot");
                    o->refCount = 0;
                    o->marked   = 0;
                    o->age      = json_int(buf, "age");

                    char gen[16];
                    json_str(buf, "gen", gen, sizeof(gen));
                    o->gen = (strcmp(gen, "old") == 0) ? 1 : 0;

                    numObjects++;
                }
                p = obj_end + 1;
            }
        }
    }

    /* Parse edges array */
    arr = strstr(body, "\"edges\"");
    if (arr) {
        arr = strchr(arr, '[');
        if (arr) {
            const char *p = arr + 1;
            while (p && *p && *p != ']') {
                const char *e_start = strchr(p, '{');
                if (!e_start) break;
                const char *e_end = strchr(e_start, '}');
                if (!e_end) break;

                int len = (int)(e_end - e_start + 1);
                char buf[256];
                if (len > 255) len = 255;
                strncpy(buf, e_start, len);
                buf[len] = 0;

                if (numEdges < MAX_EDGE) {
                    edges[numEdges].from = json_int(buf, "from");
                    edges[numEdges].to   = json_int(buf, "to");
                    numEdges++;
                }
                p = e_end + 1;
            }
        }
    }
}

/* ══════════════════════════════════════════
 *  SECTION 2: BFS Reachability (shared util)
 * ══════════════════════════════════════════ */

/*
 * BFS from all root objects.
 * Sets reachable[i] = 1 for every object index
 * reachable from a root via edges.
 * Returns count of reachable objects.
 */
static int bfs_reachable(int reachable[MAX_OBJ]) {
    memset(reachable, 0, sizeof(int) * MAX_OBJ);
    int queue[MAX_OBJ], front = 0, back = 0;
    int count = 0;

    /* Enqueue all roots */
    for (int i = 0; i < numObjects; i++) {
        if (objects[i].isRoot) {
            reachable[i] = 1;
            queue[back++] = i;
            count++;
        }
    }

    /* BFS traversal */
    while (front < back) {
        int idx = queue[front++];
        int id  = objects[idx].id;

        /* Follow all outgoing edges from this object */
        for (int e = 0; e < numEdges; e++) {
            if (edges[e].from == id) {
                int child = find_idx(edges[e].to);
                if (child >= 0 && !reachable[child]) {
                    reachable[child] = 1;
                    queue[back++] = child;
                    count++;
                }
            }
        }
    }
    return count;
}

/* ══════════════════════════════════════════
 *  SECTION 3: Reference Counting Algorithm
 * ══════════════════════════════════════════
 *
 *  Each object has a refCount = number of
 *  incoming edges. Roots always have rc >= 1.
 *  Objects with rc = 0 are garbage.
 *
 *  Weakness: cannot detect reference cycles.
 */
static void run_refcount(char *resp) {
    /* Step 1: Compute reference counts */
    for (int i = 0; i < numObjects; i++) {
        objects[i].refCount = objects[i].isRoot ? 1 : 0;
    }
    for (int e = 0; e < numEdges; e++) {
        int idx = find_idx(edges[e].to);
        if (idx >= 0)
            objects[idx].refCount++;
    }

    /* Step 2: Collect objects with refcount = 0 */
    int collected[MAX_OBJ], numCollected = 0;
    for (int i = 0; i < numObjects; i++) {
        if (objects[i].refCount == 0)
            collected[numCollected++] = objects[i].id;
    }

    /* Step 3: Detect cycles — objects unreachable but rc > 0 */
    int reachable[MAX_OBJ];
    bfs_reachable(reachable);

    int cycles[MAX_OBJ], numCycles = 0;
    for (int i = 0; i < numObjects; i++) {
        if (!reachable[i] && !objects[i].isRoot && objects[i].refCount > 0)
            cycles[numCycles++] = objects[i].id;
    }

    /* Build JSON response */
    char *p = resp;
    p += sprintf(p, "{\"algorithm\":\"refcount\"");

    /* refcounts */
    p += sprintf(p, ",\"refcounts\":[");
    for (int i = 0; i < numObjects; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "{\"id\":%d,\"rc\":%d}", objects[i].id, objects[i].refCount);
    }
    p += sprintf(p, "]");

    /* collected */
    p += sprintf(p, ",\"collected\":[");
    for (int i = 0; i < numCollected; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "%d", collected[i]);
    }
    p += sprintf(p, "]");

    /* cycles */
    p += sprintf(p, ",\"cycles\":[");
    for (int i = 0; i < numCycles; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "%d", cycles[i]);
    }
    p += sprintf(p, "]");

    p += sprintf(p, "}");
}

/* ══════════════════════════════════════════
 *  SECTION 4: Mark & Sweep Algorithm
 * ══════════════════════════════════════════
 *
 *  Phase 1 (MARK): BFS/DFS from GC roots,
 *     mark every reachable object.
 *  Phase 2 (SWEEP): Scan all objects,
 *     free anything not marked.
 *
 *  Handles cycles correctly.
 */
static void run_marksweep(char *resp) {
    /* Phase 1: Mark — BFS from roots */
    int reachable[MAX_OBJ];
    int numReachable = bfs_reachable(reachable);

    /* Record marked objects in visit order */
    int marked[MAX_OBJ], numMarked = 0;
    for (int i = 0; i < numObjects; i++) {
        objects[i].marked = reachable[i];
        if (reachable[i])
            marked[numMarked++] = objects[i].id;
    }

    /* Phase 2: Sweep — collect unmarked */
    int collected[MAX_OBJ], numCollected = 0;
    for (int i = 0; i < numObjects; i++) {
        if (!objects[i].marked)
            collected[numCollected++] = objects[i].id;
    }

    /* Build JSON response */
    char *p = resp;
    p += sprintf(p, "{\"algorithm\":\"marksweep\"");

    p += sprintf(p, ",\"marked\":[");
    for (int i = 0; i < numMarked; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "%d", marked[i]);
    }
    p += sprintf(p, "]");

    p += sprintf(p, ",\"collected\":[");
    for (int i = 0; i < numCollected; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "%d", collected[i]);
    }
    p += sprintf(p, "]");

    p += sprintf(p, ",\"totalObjects\":%d", numObjects);
    p += sprintf(p, ",\"reachableCount\":%d", numReachable);
    p += sprintf(p, "}");
}

/* ══════════════════════════════════════════
 *  SECTION 5: Generational GC Algorithm
 * ══════════════════════════════════════════
 *
 *  Based on "generational hypothesis":
 *  most objects die young.
 *
 *  - Young gen collected frequently (minor GC)
 *  - Objects surviving 3+ cycles promoted to old
 *  - Old gen collected rarely (major GC, threshold=3)
 */
static void run_generational(char *resp) {
    int reachable[MAX_OBJ];
    bfs_reachable(reachable);

    /* Step 1: Age young objects, promote if age >= 3 */
    int promoted[MAX_OBJ], numPromoted = 0;
    for (int i = 0; i < numObjects; i++) {
        if (objects[i].gen == 0) { /* young */
            objects[i].age++;
            if (objects[i].age >= 3) {
                objects[i].gen = 1; /* promote to old */
                promoted[numPromoted++] = objects[i].id;
            }
        }
    }

    /* Step 2: Minor GC — collect unreachable young objects */
    int collectedYoung[MAX_OBJ], numCollectedYoung = 0;
    for (int i = 0; i < numObjects; i++) {
        if (objects[i].gen == 0 && !reachable[i] && !objects[i].isRoot)
            collectedYoung[numCollectedYoung++] = objects[i].id;
    }

    /* Step 3: Major GC — only if old gen has 3+ objects */
    int numOld = 0;
    for (int i = 0; i < numObjects; i++)
        if (objects[i].gen == 1) numOld++;

    int collectedOld[MAX_OBJ], numCollectedOld = 0;
    if (numOld >= 3) {
        for (int i = 0; i < numObjects; i++) {
            if (objects[i].gen == 1 && !reachable[i] && !objects[i].isRoot)
                collectedOld[numCollectedOld++] = objects[i].id;
        }
    }

    /* Build JSON response */
    char *p = resp;
    p += sprintf(p, "{\"algorithm\":\"generational\"");

    p += sprintf(p, ",\"promoted\":[");
    for (int i = 0; i < numPromoted; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "%d", promoted[i]);
    }
    p += sprintf(p, "]");

    p += sprintf(p, ",\"collectedYoung\":[");
    for (int i = 0; i < numCollectedYoung; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "%d", collectedYoung[i]);
    }
    p += sprintf(p, "]");

    p += sprintf(p, ",\"collectedOld\":[");
    for (int i = 0; i < numCollectedOld; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "%d", collectedOld[i]);
    }
    p += sprintf(p, "]");

    p += sprintf(p, ",\"majorGCTriggered\":%s", numOld >= 3 ? "true" : "false");

    p += sprintf(p, ",\"ages\":[");
    for (int i = 0; i < numObjects; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "{\"id\":%d,\"age\":%d,\"gen\":\"%s\"}",
            objects[i].id, objects[i].age,
            objects[i].gen == 1 ? "old" : "young");
    }
    p += sprintf(p, "]");

    p += sprintf(p, "}");
}

/* ══════════════════════════════════════════
 *  SECTION 6: HTTP Server
 * ══════════════════════════════════════════ */

/* Send HTTP response with CORS headers */
static void send_http(int client, int status, const char *body) {
    char header[1024];
    const char *status_text = (status == 200) ? "OK" :
                              (status == 204) ? "No Content" : "Bad Request";

    int body_len = body ? (int)strlen(body) : 0;

    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);

    write(client, header, strlen(header));
    if (body && body_len > 0)
        write(client, body, body_len);
}

/* Extract query parameter value: /gc?algo=refcount -> "refcount" */
static void get_query_param(const char *request, const char *key, char *val, int maxlen) {
    val[0] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(request, pat);
    if (!p) return;
    p += strlen(pat);
    int i = 0;
    while (*p && *p != ' ' && *p != '&' && *p != '\r' && *p != '\n' && i < maxlen - 1)
        val[i++] = *p++;
    val[i] = 0;
}

/* Handle one HTTP request */
static void handle_request(int client, char *buffer, int bytes) {
    /* Check if OPTIONS (CORS preflight) */
    if (strncmp(buffer, "OPTIONS", 7) == 0) {
        send_http(client, 204, NULL);
        return;
    }

    /* Only accept POST /gc */
    if (strncmp(buffer, "POST", 4) != 0 || !strstr(buffer, "/gc")) {
        send_http(client, 400, "{\"error\":\"Use POST /gc?algo=refcount|marksweep|generational\"}");
        return;
    }

    /* Get algorithm from query string */
    char algo[32];
    get_query_param(buffer, "algo", algo, sizeof(algo));

    /* Find request body (after \r\n\r\n) */
    char *body = strstr(buffer, "\r\n\r\n");
    if (body) body += 4;

    /* Parse the JSON body */
    parse_body(body);

    printf("  Parsed: %d objects, %d edges, algo=%s\n", numObjects, numEdges, algo);

    /* Run the selected GC algorithm */
    char response[RES_SIZE];
    memset(response, 0, sizeof(response));

    if (strcmp(algo, "marksweep") == 0) {
        run_marksweep(response);
    } else if (strcmp(algo, "generational") == 0) {
        run_generational(response);
    } else {
        run_refcount(response);
    }

    /* Send result */
    send_http(client, 200, response);
    printf("  Response sent (%d bytes)\n", (int)strlen(response));
}

/* ══════════════════════════════════════════
 *  SECTION 7: Main — Start Server
 * ══════════════════════════════════════════ */

int main() {
    /* Get port from environment (Render sets PORT) */
    const char *port_env = getenv("PORT");
    int port = port_env ? atoi(port_env) : 8080;

    /* Create socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    /* Allow port reuse */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    /* Listen */
    if (listen(server_fd, 10) < 0) {
        perror("listen"); return 1;
    }

    printf("============================================\n");
    printf("  GC Visualizer — C Backend Server\n");
    printf("  Listening on port %d\n", port);
    printf("  Algorithms: refcount, marksweep, generational\n");
    printf("============================================\n");

    /* Accept loop */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0) { perror("accept"); continue; }

        char buffer[BUF_SIZE];
        memset(buffer, 0, sizeof(buffer));
        int bytes = read(client, buffer, BUF_SIZE - 1);

        if (bytes > 0) {
            printf("Request received (%d bytes)\n", bytes);
            handle_request(client, buffer, bytes);
        }

        close(client);
    }

    close(server_fd);
    return 0;
}
