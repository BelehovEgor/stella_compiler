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

/** Scan variable of copying garbage collection algorithm */
void* scan;

#define MAX_GC_ROOTS 1024
#define MAX_ALLOC_SIZE (8 * 100)
#define MAX_G0_SIZE MAX_ALLOC_SIZE
#define MAX_G1_SIZE (MAX_G0_SIZE * 2)
#define MAX_G2_SIZE (MAX_G1_SIZE * 2)

int gc_roots_max_size = 0;
int gc_roots_top = 0;
void **gc_roots[MAX_GC_ROOTS];

void *from_space;
void *to_space;

void *from_space_next;
void *to_space_next;

void* try_alloc(size_t size_in_bytes);
void alloc_stat_update(size_t size_in_bytes);
bool is_in_heap(const void* ptr, const void* heap, size_t heap_size);
void gc_collect();
int get_size(const stella_object *obj);

void* gc_alloc(const size_t size_in_bytes) {
  if (from_space == NULL) {
    from_space = malloc(MAX_ALLOC_SIZE);
    from_space_next = from_space;
  }

  void *result = try_alloc(size_in_bytes);
  if (result == NULL) {
    gc_collect();

    result = try_alloc(size_in_bytes);
  }

  if (result != NULL) {
    alloc_stat_update(size_in_bytes);
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

void print_gc_alloc_stats() {
  printf("Total memory allocation: %d bytes (%d objects)\n", total_allocated_bytes, total_allocated_objects);
  printf("Total garbage collecting: %d\n", total_gc_collect);
  printf("Maximum residency:       %d bytes (%d objects)\n", max_allocated_bytes, max_allocated_objects);
  printf("Total memory use:        %d reads and %d writes\n", total_reads, total_writes);
  printf("Max GC roots stack size: %d roots\n", gc_roots_max_size);

  print_gc_state(); // for debug
}

void print_gc_state() {
  printf("Objects from G_0:\n");
  for (void *start = from_space; start < from_space_next; start += get_size(start)) {
    stella_object *st_obj = start;
    const int tag = STELLA_OBJECT_HEADER_TAG(st_obj->object_header);
    printf("\tAddress: %p; tag: %d; fields: ", st_obj, tag);

    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(st_obj->object_header);
    for (int i = 0; i < field_count; i++) {
      printf("%p", st_obj->object_fields[i]);
      if (i < field_count - 1) {
        printf(", ");
      } else {
        printf("; ");
      }
    }

    printf("beauty value: ");
    print_stella_object(st_obj);
    printf("\n");
  }

  printf("G_0 boundaries from: %p to: %p\n", from_space, from_space + MAX_ALLOC_SIZE);
  printf("G_0 free memory part from: %p to: %p\n", from_space_next, from_space + MAX_ALLOC_SIZE);
  printf("Scan: %p, Next: %p, Limit: %p\n", scan, to_space_next, to_space + MAX_ALLOC_SIZE);
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

void* try_alloc(const size_t size_in_bytes) {
  if (from_space_next + size_in_bytes <= from_space + MAX_ALLOC_SIZE) {
    void *result = from_space_next;
    from_space_next += size_in_bytes;

    return result;
  }

  exit(1); // выход? как обрабатывать отсутствие памяти
}

void alloc_stat_update(const size_t size_in_bytes) {
  total_allocated_bytes += size_in_bytes;
  total_allocated_objects += 1;
  max_allocated_bytes = total_allocated_bytes;
  max_allocated_objects = total_allocated_objects;
}

bool is_in_heap(const void* ptr, const void* heap, const size_t heap_size) {
  return ptr >= heap && ptr < heap + heap_size;
}

void chase(stella_object *p) {
  do {
    stella_object *q = to_space_next;
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(p->object_header);
    to_space_next += get_size(p);
    void *r = NULL;

    q->object_header = p->object_header;
    for (int i = 0; i < field_count; i++) {
      q->object_fields[i] = p->object_fields[i];

      stella_object *potentially_forwarded = q->object_fields[i];
      if (is_in_heap(q->object_fields[i], from_space, MAX_ALLOC_SIZE) &&
          !is_in_heap(potentially_forwarded->object_fields[0], to_space, MAX_ALLOC_SIZE)) {
        r = potentially_forwarded;
      }
    }

    p->object_fields[0] = q;
    p = r;
  } while (p != NULL);
}

void* forward(stella_object* p) {
  if (!is_in_heap(p, from_space, MAX_ALLOC_SIZE)) {
    return p;
  }

  if (is_in_heap(p->object_fields[0], to_space, MAX_ALLOC_SIZE)) {
    return p->object_fields[0];
  }

  chase(p);
  return p->object_fields[0];
}

void gc_collect_stat_update() {
  total_gc_collect += 1;
}

void gc_collect() {
  if (to_space == NULL) {
    to_space = malloc(MAX_ALLOC_SIZE);
    to_space_next = to_space;
  }

  scan = to_space_next;

  for (int i = 0; i < gc_roots_top; i++) {
    void **root_ptr = gc_roots[i];
    *root_ptr = forward(*root_ptr);
  }

  while (scan < to_space_next) {
    stella_object *obj = scan;
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    for (int i = 0; i < field_count; i++) {
      obj->object_fields[i] = forward(obj->object_fields[i]); // надо ли оно через define
    }

    scan += get_size(obj);
  }

  void *buff = from_space;
  from_space = to_space;
  to_space = buff;

  from_space_next = to_space_next;
  to_space_next = to_space;

  gc_collect_stat_update();
}

int get_size(const stella_object *obj) {
  const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
  return (1 + field_count) * sizeof(void*);
}