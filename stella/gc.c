#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

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

/** Total count gc collect (over the entire duration of the program). */
int total_gc_collect = 0;

#define MAX_GC_ROOTS 1024
#define MAX_ALLOC_SIZE (16 * 80)
#define DEBUG_LOGS

int gc_roots_max_size = 0;
int gc_roots_top = 0;
void **gc_roots[MAX_GC_ROOTS];

void init_generation();
void alloc_stat_update(size_t size_in_bytes);
bool is_in_heap(const void* ptr, const void* heap, size_t heap_size);
void gc_collect();
int get_size(const stella_object *obj);

struct generation {
  int number;
  int collect_count;
  int size;
  void* from_space_next;
  void* from_space;
  void* to_space;
  void* to_space_next;
  void* scan;
} g0;

void print_state(struct generation* g);
bool is_here(struct generation* g, const void* ptr);
void* try_alloc(struct generation* g, size_t size_in_bytes);

void* gc_alloc(const size_t size_in_bytes) {
  init_generation();

  void *result = try_alloc(&g0, size_in_bytes);
  if (result == NULL) {
    gc_collect();

    result = try_alloc(&g0, size_in_bytes);
  }

  if (result != NULL) {
    alloc_stat_update(size_in_bytes);
  } else {
    printf("Out of memory!");
    exit(1); // выход? как обрабатывать отсутствие памяти
  }

  return result;
}

void print_gc_roots() {
  printf("ROOTS: ");
  for (int i = 0; i < gc_roots_top; i++) {
    printf("%p ", gc_roots[i]);
  }
  printf("\n");
}

void print_separator() {
  printf("=====================================================================================\n");
}

void print_gc_alloc_stats() {
  print_separator();
  printf("STATS");
  print_separator();

  printf("Total memory allocation: %d bytes (%d objects)\n", total_allocated_bytes, total_allocated_objects);
  printf("Total garbage collecting: %d\n", total_gc_collect);
  printf("Maximum residency:       %d bytes (%d objects)\n", max_allocated_bytes, max_allocated_objects);
  printf("Total memory use:        %d reads and %d writes\n", total_reads, total_writes);
  printf("Max GC roots stack size: %d roots\n", gc_roots_max_size);

  print_separator();
}

void print_gc_state() {
  print_state(&g0);

  printf("ROOTS:\n");
  print_separator();
  for (int i = 0; i < gc_roots_top; i++) {
    printf(
      "\tIDX: %d, ADDRESS: %p, VALUE: %p\n",
      i,
      gc_roots[i],
      *gc_roots[i]);
  }

  print_separator();
}

void gc_read_barrier(void *object, int field_index) {
  total_reads += 1;
}

void gc_write_barrier(void *object, int field_index, void *contents) {
  total_writes += 1;
}

void gc_push_root(void **ptr){
  gc_roots[gc_roots_top++] = ptr;
  if (gc_roots_top > gc_roots_max_size) { gc_roots_max_size = gc_roots_top; }
}

void gc_pop_root(void **ptr){
  gc_roots_top--;
}

// private

void init_generation() {
  if (g0.from_space != NULL) return;

  void* g0_from_space = malloc(MAX_ALLOC_SIZE);
  void* g0_to_space = malloc(MAX_ALLOC_SIZE);

  g0.number = 0;
  g0.size = MAX_ALLOC_SIZE;
  g0.from_space = g0_from_space;
  g0.from_space_next = g0_from_space;
  g0.to_space = g0_to_space;
  g0.to_space_next = g0_to_space;
}

void gc_collect_stat_update() {
  total_gc_collect += 1;
}

bool is_in_heap(const void* ptr, const void* heap, const size_t heap_size) {
  return ptr >= heap && ptr < heap + heap_size;
}

bool is_here(struct generation* g, const void* ptr) {
  return is_in_heap(ptr, g->from_space, g->size);
}

bool has_enough_space(struct generation* g, const size_t heap_size) {
  return g->from_space_next + heap_size <= g->from_space + g->size;
}

void* get_space(struct generation* g, const size_t size) {
  void *result = g->from_space_next;
  g->from_space_next += size;

  return result;
}

void print_state(struct generation* g) {
  print_separator();
  printf("G_%d STATE\n", g->number);
  print_separator();

  printf("COLLECT COUNT %d\n", g->collect_count);
  printf("OBJECTS:\n");
  for (void *start = g->from_space; start < g->from_space_next; start += get_size(start)) {
    stella_object *st_obj = start;
    const int tag = STELLA_OBJECT_HEADER_TAG(st_obj->object_header);
    printf("\tADDRESS: %p | TAG: %d | FIELDS: ", st_obj, tag);

    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(st_obj->object_header);
    for (int i = 0; i < field_count; i++) {
      printf("%p", st_obj->object_fields[i]);
      if (i < field_count - 1) {
        printf(", ");
      }
    }

    printf("\n");
  }

  // Кол-во выделенной памяти
  printf("BOUNDARIES  | FROM: %p | TO: %p | TOTAL: %d bytes\n", g->from_space, g->from_space + g->size, g->size);
  printf("FREE MEMORY | FROM: %p | TO: %p | TOTAL: %ld bytes\n",
    g->from_space_next,
    g->from_space + g->size,
    g->from_space + g->size - g->from_space_next);
  printf("SCAN: %p, NEXT: %p, LIMIT: %p\n", g->scan, g->to_space_next, g->to_space + g->size);

  print_separator();
}

void* try_alloc(struct generation* g, const size_t size_in_bytes) {
  if (has_enough_space(g, size_in_bytes)) {
    return get_space(g, size_in_bytes);
  }

  return NULL;
}

void chase(struct generation* g, stella_object *p) {
  do {
    stella_object *q = g->to_space_next;
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(p->object_header);
    g->to_space_next += get_size(p);
    void *r = NULL;

    q->object_header = p->object_header;
    for (int i = 0; i < field_count; i++) {
      q->object_fields[i] = p->object_fields[i];

      stella_object *potentially_forwarded = q->object_fields[i];
      if (is_here(g, q->object_fields[i]) &&
          !is_in_heap(potentially_forwarded->object_fields[0], g->to_space, g->size)) {
        r = potentially_forwarded;
      }
    }

    p->object_fields[0] = q;
    p = r;
  } while (p != NULL);
}

void* forward(struct generation* g, stella_object* p) {
  if (!is_in_heap(p, g->from_space, MAX_ALLOC_SIZE)) {
    return p;
  }

  if (is_in_heap(p->object_fields[0], g->to_space, MAX_ALLOC_SIZE)) {
    return p->object_fields[0];
  }

  chase(g, p);
  return p->object_fields[0];
}

void collect(struct generation* g) {
  g->collect_count++;

#ifdef DEBUG_LOGS
  print_separator();
  printf("COLLECTING G_%d - COLLECTING NUMBER %d\n", g->number, g->collect_count);
  print_separator();
#endif

  g->scan = g->to_space_next;

  for (int i = 0; i < gc_roots_top; i++) {
    void **root_ptr = gc_roots[i];
    *root_ptr = forward(g, *root_ptr);
  }

  while (g->scan < g->to_space_next) {
    stella_object *obj = g->scan;
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    for (int i = 0; i < field_count; i++) {
      obj->object_fields[i] = forward(g, obj->object_fields[i]);
    }

    g->scan += get_size(obj);
  }

  void *buff = g->from_space;
  g->from_space = g->to_space;
  g->to_space = buff;

  g->from_space_next = g->to_space_next;
  g->to_space_next = g->to_space;

  gc_collect_stat_update();
}

void alloc_stat_update(const size_t size_in_bytes) {
  total_allocated_bytes += size_in_bytes;
  total_allocated_objects += 1;
  max_allocated_bytes = total_allocated_bytes;
  max_allocated_objects = total_allocated_objects;
}

void gc_collect() {
#ifdef DEBUG_LOGS
  print_gc_state();
#endif

  collect(&g0);

#ifdef DEBUG_LOGS
  print_gc_state();
#endif
}

int get_size(const stella_object *obj) {
  const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
  return (1 + field_count) * sizeof(void*);
}