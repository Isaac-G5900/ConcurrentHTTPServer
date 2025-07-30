# Concurrent HTTP Server

A multithreaded **HTTP/1.1** server written in C. It supports `GET` and `PUT` (file upload) and remains safe under heavy parallel load thanks to a lock‑free dispatch queue and per‑URI reader/writer locking.

---

## Key Features

* **Thread‑pool dispatcher** – fixed pool of worker threads spun up at start‑up.
* **Lock‑free work queue** – single‑producer / multi‑consumer ring buffer built with POSIX semaphores.
* **Configurable reader/writer locks** – custom implementation offering reader‑priority, writer‑priority, or *N‑way* batching. Each requested URI gets its own lock, so unrelated files are served concurrently.
* **Scalable hash table** – maps URIs to their locks, keeping look‑ups `O(1)` on average.
* **Audit logging** – every completed request is written to `stderr` in CSV form: `OP,URI,STATUS,REQUEST‑ID`.

---

## Repository Layout

```
concurrenthttpserver/
├── httpserver.c         # main entry point
├── queue.c / queue.h    # lock‑free dispatch queue
├── rwlock.c / rwlock.h  # reader/writer lock primitives
├── listener_socket.*    # BSD‑socket wrapper (helper library)
├── connection.*         # HTTP parsing and response helpers (helper library)
├── Makefile             # builds the `httpserver` binary
└── tests/               # functional test scripts
```

---

## Build

```bash
cd concurrenthttpserver
make              # produces ./httpserver
```

**Requirements**

* GCC or Clang with C11 support
* POSIX threads & semaphores (Linux/macOS)
* GNU Make ≥ 4.0

---

## Run

```bash
./httpserver [-t THREADS] <port>
# example
./httpserver -t 8 8080
```

The server listens on `<port>` and serves files from the current working directory. All diagnostics and audit lines are printed to `stderr` so they won’t mix with HTTP payloads.

### Concurrency Model

1. The **dispatcher** accepts TCP connections and enqueues them.
2. **Worker** threads dequeue, parse, and fulfil requests:

   * `GET /path` → acquires a *reader* lock for `/path` and streams the file.
   * `PUT /path` → acquires a *writer* lock for `/path` and atomically replaces the file via a temporary upload.

Because locks are scoped per URI, simultaneous requests for different files run fully in parallel.

---

## Tests

Functional scripts exercise both happy‑path and edge‑case behaviour (@author Andrew Quinn)

```bash
./test_repo.sh          # run the entire suite
./tests/get_basic.sh    # run an individual case
```

Scripts rely on `curl` and `diff` to verify status codes and payload content.
