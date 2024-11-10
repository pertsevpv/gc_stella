#include <stdlib.h>
#include <stdio.h>

#include "runtime.h"
#include "gc.h"

/** Total allocated number of bytes (over the entire duration of the program). */
int total_allocated_bytes = 0;

/** Total allocated number of objects (over the entire duration of the program). */
int total_allocated_objects = 0;

int max_allocated_bytes = 0;
int max_allocated_objects = 0;

int total_reads = 0;
int total_writes = 0;

#define MAX_GC_ROOTS 1024
#define MAX_CHANGED (1 << 12)
#define MAX_ALLOC_SIZE (1 << 16)
#define G1_ALLOC_SIZE (MAX_ALLOC_SIZE << 1)

int gc_roots_max_size = 0;
int gc_roots_top = 0;
void **gc_roots[MAX_GC_ROOTS];

int changed_ptr = 0;
void *changed[MAX_CHANGED];

typedef struct {
    void *f;
    stella_object obj;
} gc_object;

typedef struct {
    int gen;
    void *from;
    int size;
    void *next;
} space;

typedef struct {
    int gc_cnt;
    void *scan;
    space *from;
    space *to;
} gen;

gen G0, G1;
int gc_inited = 0;

void collect(gen *g);

space* space_init(int gen_num, size_t size) {
  space* sp = malloc(sizeof(space));
  sp->gen = gen_num;
  sp->from = malloc(size);
  sp->size = size;
  sp->next = sp->from;
  return sp;
}

void gc_init() {
  G0.from = space_init(0, MAX_ALLOC_SIZE);
  G1.from = space_init(1, G1_ALLOC_SIZE);
  G0.to = G1.from;
  G1.to = space_init(1, G1_ALLOC_SIZE);
  gc_inited = 1;
}

void out_of_memory() {
  printf("Out of memory");
  exit(137);
}

gc_object *alloc_space(space *sp, const size_t size_in_bytes) {
  if (sp->next + size_in_bytes <= sp->from + sp->size) {
    gc_object *q = sp->next;
    sp->next += size_in_bytes;
    q->f = NULL;
    q->obj.object_header = 0;
    return q;
  }
  return NULL;
}

void collect_and_print(gen* g) {
  printf("Before GC\n");
  print_gc_state();
  collect(g);
  printf("After GC\n");
  print_gc_state();
  print_gc_alloc_stats();
  printf("\n\n\n");
}

void *gc_alloc(const size_t size_in_bytes) {
  if (!gc_inited) gc_init();

  void *q = alloc_space(G0.from, size_in_bytes + sizeof(void *));
  if (q == NULL) {
    collect_and_print(&G0);
    q = alloc_space(G0.from, size_in_bytes + sizeof(void *));
    if (q == NULL) {
      out_of_memory();
      return NULL;
    }
  }
  total_allocated_bytes += size_in_bytes;
  total_allocated_objects += 1;
  max_allocated_bytes = total_allocated_bytes;
  max_allocated_objects = total_allocated_objects;
  return q + sizeof(void *);
}

void print_gc_roots() {
  printf("Roots:\n");
  printf("\t%-5s | %-16s\n", "Index", "Address");
  for (int i = 0; i < gc_roots_top; i++)
    printf("\t%-5d | %-16p\n", i, *gc_roots[i]);
}

size_t gc_obj_size(const gc_object *obj) {
  return sizeof(gc_object) + STELLA_OBJECT_HEADER_FIELD_COUNT(obj->obj.object_header) * sizeof(void *);
}

void print_space(space *sp) {
  printf("\t%-16s | %-16s | %-3s | Fields\n", "GC Address", "St. Address", "Tag");
  for (void *p = sp->from; p < sp->next; p += gc_obj_size(p)) {
    gc_object *gc_obj = p;
    stella_object *obj = &gc_obj->obj;
    const int header = STELLA_OBJECT_HEADER_TAG(obj->object_header);
    const int field_cnt = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    printf("\t%-16p | %-16p | %-3d |", gc_obj, obj, header);
    for (int i = 0; i < field_cnt; i++)printf(" %-16p", obj->object_fields[i]);
    printf("\n");
  }
  printf("GC vars:\n");
  printf("%-3s | %-16s | %-16s | %-16s | %-16s\n", "Gen", "From", "To", "Next", "Size");
  printf("%-3d | %-16p | %-16p | %-16p | %-16d\n", sp->gen, sp->from, sp->from + sp->size, sp->next, sp->size);
}

void print_gc_alloc_stats() {
  printf("Garbage collector (GC) statistics:\n");
  printf("Total memory allocation:  %d bytes (%d objects)\n", total_allocated_bytes, total_allocated_objects);
  printf("Maximum residency:        %d bytes (%d objects)\n", max_allocated_bytes, max_allocated_objects);
  printf("Total memory use:         %d reads and %d writes\n", total_reads, total_writes);
  printf("Max GC roots stack size: %d roots\n", gc_roots_max_size);
}

void print_gc_state() {
  print_gc_roots();
  printf("From\n");
  printf("G0, %d garbage collecting\n", G0.gc_cnt);
  print_space(G0.from);
  printf("G1, %d garbage collecting\n", G1.gc_cnt);
  print_space(G1.from);
  printf("To\n");
  print_space(G0.to->gen == G0.from->gen ? G0.to : G1.to);
  printf("\n");
}

void gc_read_barrier(void *object, int field_index) {
  total_reads += 1;
}

void gc_write_barrier(void *object, int field_index, void *contents) {
  total_writes += 1;

  changed[changed_ptr] = object;
  changed_ptr++;
}

void gc_push_root(void **ptr) {
  gc_roots[gc_roots_top++] = ptr;
  if (gc_roots_top > gc_roots_max_size) { gc_roots_max_size = gc_roots_top; }
}

void gc_pop_root(void **ptr) {
  gc_roots_top--;
}

int points_to(const space *sp, void *ptr) {
  return ptr >= sp->from && ptr < sp->from + sp->size;
}

int chase(const gen *g, gc_object *p) {
  do {
    gc_object *q = alloc_space(g->to, gc_obj_size(p));
    if (q == NULL)return 0;

    void *r = NULL;
    q->f = NULL;
    q->obj.object_header = p->obj.object_header;
    int field_cnt = STELLA_OBJECT_HEADER_FIELD_COUNT(p->obj.object_header);
    for (int i = 0; i < field_cnt; i++) {
      q->obj.object_fields[i] = p->obj.object_fields[i];
      if (points_to(g->from, q->obj.object_fields[i])) {
        gc_object *qfi = q->obj.object_fields[i] - sizeof(void *);
        if (!points_to(g->to, qfi->f))r = qfi;
      }
    }
    p->f = q;
    p = r;
  } while (p != NULL);
  return 1;
}

void *forward(gen *g, void *p) {
  if (points_to(g->from, p)) {
    gc_object *p_obj = p - sizeof(void *);

    if (points_to(g->to, p_obj->f))return &((gc_object *) p_obj->f)->obj;

    if (!chase(g, p_obj)) {
      if (g->to->gen == g->from->gen) out_of_memory();
      collect(g->to->gen == 0 ? &G0 : &G1);
      if (!chase(g, p_obj)) out_of_memory();
    }
    return &((gc_object *) p_obj->f)->obj;
  } else return p;
}

void forward_roots(gen *g) {
  for (int i = 0; i < gc_roots_top; i++) {
    void **root_ptr = gc_roots[i];
    *root_ptr = forward(g, *root_ptr);
  }
}

void forward_g0(gen *g) {
  for (void *ptr = G0.from->from; ptr < G0.from->next; ptr += gc_obj_size(ptr)) {
    gc_object *gc_obj = ptr;
    const int field_cnt = STELLA_OBJECT_HEADER_FIELD_COUNT(gc_obj->obj.object_header);
    for (int j = 0; j < field_cnt; j++)gc_obj->obj.object_fields[j] = forward(g, gc_obj->obj.object_fields[j]);
  }
}

void forward_changed(gen *g) {
  for (int i = 0; i < changed_ptr; i++) {
    stella_object *obj = changed[i];
    const int field_cnt = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    for (int j = 0; j < field_cnt; j++)obj->object_fields[j] = forward(g, obj->object_fields[j]);
  }
  changed_ptr = 0;
}

void forward_scan_to_next(gen *g) {
  for (gc_object *obj; g->scan < g->to->next; g->scan += gc_obj_size(obj)) {
    obj = g->scan;
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->obj.object_header);
    for (int i = 0; i < field_count; i++)obj->obj.object_fields[i] = forward(g, obj->obj.object_fields[i]);
  }
}

void collect(gen *g) {
  g->gc_cnt++;
  g->scan = g->to->next;
  forward_roots(g);
  if (g->from->gen == 1) forward_g0(g);
  forward_changed(g);
  forward_scan_to_next(g);

  if (g->from->gen == g->to->gen) {
    void *tmp = g->from;
    g->from = g->to;
    g->to = tmp;
    g->to->next = g->to->from;
    G0.to = g->from;
    G0.scan = g->from->from;
  } else {
    gen *cur = g->from->gen == 0 ? &G0 : &G1;
    const gen *next = g->to->gen == 0 ? &G0 : &G1;
    cur->from->next = g->from->from;
    cur->to = next->from;
  }
}
