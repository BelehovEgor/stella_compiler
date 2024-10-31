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
#define MAX_ALLOC_SIZE (16 * 200)
#define MAX_CHANGED_NODES 1024
#define DEBUG_LOGS

int gc_roots_max_size = 0;
int gc_roots_top = 0;
void **gc_roots[MAX_GC_ROOTS];

int changed_nodes_top = 0;
void *changed_nodes[MAX_CHANGED_NODES];

struct space {
  int gen;
  int size;
  void* next;
  void* heap;
} g0_space_from, g1_space_from, g1_space_to;

struct generation {
  int number;
  int collect_count;

  struct space* from;
  struct space* to;

  void* scan;
} g0, g1;

int generation_count = 2;
struct generation* generations[] = { &g0, &g1 };

void init_generation();
void alloc_stat_update(size_t size_in_bytes);
void gc_collect();
int get_size(const stella_object *obj);
void print_state(const struct generation* g);
void* try_alloc(struct generation* g, size_t size_in_bytes);
bool is_in_place(const struct space* space, const void* ptr);

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
    exit(137); // выход? как обрабатывать отсутствие памяти
  }

  return result;
}

void print_separator() {
  printf("=====================================================================================\n");
}

void print_gc_alloc_stats() {
  print_separator();
  printf("STATS");
  print_separator();

  printf("Total memory allocation:  %d bytes (%d objects)\n", total_allocated_bytes, total_allocated_objects);
  printf("Total garbage collecting: %d\n", total_gc_collect);
  printf("Maximum residency:        %d bytes (%d objects)\n", max_allocated_bytes, max_allocated_objects);
  printf("Total memory use:         %d reads and %d writes\n", total_reads, total_writes);
  printf("Max GC roots stack size:  %d roots\n", gc_roots_max_size);

  print_separator();
}

void print_root_state() {
  printf("ROOTS:\n");
  print_separator();

  for (int i = 0; i < gc_roots_top; i++) {
    printf(
      "\tIDX: %d, ADDRESS: %p, FROM: %s, VALUE: %p, \n",
      i,
      gc_roots[i],
      is_in_place(&g0_space_from, *gc_roots[i])
        ? "  G_0"
        : is_in_place(&g1_space_from, *gc_roots[i])
          ? "  G_1"
          : "OTHER",
      *gc_roots[i]
    );
  }

  print_separator();
}

void print_gc_state() {
  print_state(&g0);
  print_state(&g1);
  print_root_state();
}

void gc_read_barrier(void *object, int field_index) {
  total_reads += 1;
}

void gc_write_barrier(void *object, int field_index, void *contents) {
  total_writes += 1;

  changed_nodes[changed_nodes_top] = object;
  changed_nodes_top++;
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
  if (g0.from != NULL) return;

  g0_space_from.gen = 0;
  g0_space_from.size = MAX_ALLOC_SIZE;
  g0_space_from.heap = malloc(MAX_ALLOC_SIZE);
  g0_space_from.next = g0_space_from.heap;

  const int g1_space_size = MAX_ALLOC_SIZE * 2;
  g1_space_from.gen = 1;
  g1_space_from.size = g1_space_size;
  g1_space_from.heap = malloc(g1_space_size);
  g1_space_from.next = g1_space_from.heap;
  g1_space_to.gen = 1;
  g1_space_to.size = g1_space_size;
  g1_space_to.heap = malloc(g1_space_size);
  g1_space_to.next = g1_space_to.heap;

  g0.number = 0;
  g0.from = &g0_space_from;
  g0.to = &g1_space_from;

  g1.number = 1;
  g1.from = &g1_space_from;
  g1.to = &g1_space_to;
}

void gc_collect_stat_update() {
  total_gc_collect += 1;
}

bool is_in_heap(const void* ptr, const void* heap, const size_t heap_size) {
  return ptr >= heap && ptr < heap + heap_size;
}

bool is_in_place(const struct space* space, const void* ptr) {
  return is_in_heap(ptr, space->heap, space->size);
}

bool has_enough_space(const struct space* space, const size_t requested_size) {
  return space->next + requested_size <= space->heap + space->size;
}

void print_state(const struct generation* g) {
  print_separator();
  printf("G_%d STATE\n", g->number);
  print_separator();

  printf("COLLECT COUNT %d\n", g->collect_count);
  printf("OBJECTS:\n");
  for (void *start = g->from->heap; start < g->from->next; start += get_size(start)) {
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
  printf("BOUNDARIES  | FROM: %p | TO: %p | TOTAL: %d bytes\n",
    g->from->heap,
    g->from->heap + g->from->size,
    g->from->size);
  printf("FREE MEMORY | FROM: %p | TO: %p | TOTAL: %ld bytes\n",
    g->from->next,
    g->from->heap + g->from->size,
    g->from->heap + g->from->size - g->from->next);
  printf("SCAN: %p, NEXT: %p, LIMIT: %p\n", g->scan, g->scan, g->to->next + g->to->size);

  print_separator();
}

void* alloc_in_space(struct space* space, const size_t size_in_bytes) {
  if (has_enough_space(space, size_in_bytes + sizeof(void*))) {
    void *result = space->next;
    space->next += size_in_bytes;

    return result;
  }

  return NULL;
}

void* try_alloc(struct generation* g, const size_t size_in_bytes) {
  return alloc_in_space(g->from, size_in_bytes);
}

void collect(struct generation* g);

stella_object* chase(struct generation* g, stella_object *p) {
  do {
    stella_object *q = alloc_in_space(g->to, get_size(p));
    if (q == NULL) {
      return p;
    }

    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(p->object_header);
    void *r = NULL;

    q->object_header = p->object_header;
    for (int i = 0; i < field_count; i++) {
      q->object_fields[i] = p->object_fields[i];

      stella_object *potentially_forwarded = q->object_fields[i];
      if (is_in_place(g->from, q->object_fields[i]) &&
          !is_in_place(g->to, potentially_forwarded->object_fields[0])) {
        r = potentially_forwarded;
      }
    }

    p->object_fields[0] = q;
    p = r;
  } while (p != NULL);

  return NULL;
}

void* forward(struct generation* g, stella_object* p) {
  if (!is_in_place(g->from, p)) {
    return p;
  }

  if (is_in_place(g->to, p->object_fields[0])) {
    return p->object_fields[0];
  }

  stella_object* not_chased = chase(g, p);
  if (not_chased != NULL) {
    collect(generations[g->to->gen]);

    not_chased = chase(g, not_chased);
    if (not_chased != NULL) {
      exit(137); // todo
    }
  }
  return p->object_fields[0];
}

void collect(struct generation* g) {
  g->collect_count++;
  gc_collect_stat_update();

#ifdef DEBUG_LOGS
  print_separator();
  printf("COLLECTING G_%d - COLLECTING NUMBER %d\n", g->number, g->collect_count);
  print_state(g);
  print_root_state();
#endif

  g->scan = g->to->next;

  for (int i = 0; i < gc_roots_top; i++) {
    void **root_ptr = gc_roots[i];
    *root_ptr = forward(g, *root_ptr);
  }

  // run for all objects in prev generations and try find link to collected generation
  for (int i = 0; i < g->number; i++) {
    const struct generation* past_gen = generations[i];

    for (stella_object *obj = past_gen->from->heap; obj < past_gen->from->next; obj += get_size(obj)) {
      const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
      for (int j = 0; j < field_count; j++) {
        obj->object_fields[j] = forward(g, obj->object_fields[j]);
      }
    }
  }

  // changed objects
  for (int i = 0; i < changed_nodes_top; i++) {
    stella_object *obj = changed_nodes[i];
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    for (int j = 0; j < field_count; j++) {
      obj->object_fields[j] = forward(g, obj->object_fields[j]);
    }

    changed_nodes_top = 0;
  }

  while (g->scan < g->to->next) {
    stella_object *obj = g->scan;
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    for (int i = 0; i < field_count; i++) {
      obj->object_fields[i] = forward(g, obj->object_fields[i]);
    }

    g->scan += get_size(obj);
  }

  if (g->from->gen == g->to->gen) { // copiyng gc
    void *buff = g->from;
    g->from = g->to;
    g->to = buff;

    g->to->next = g->to->heap;

    struct generation* past = generations[g->from->gen - 1];
    past->to = g->from;
  } else { // generations
    struct generation* current = generations[g->from->gen];
    const struct generation* next = generations[g->to->gen];
    current->from->next = g->from->heap;
    current->to = next->from;
  }
}

void alloc_stat_update(const size_t size_in_bytes) {
  total_allocated_bytes += size_in_bytes;
  total_allocated_objects += 1;
  max_allocated_bytes = total_allocated_bytes;
  max_allocated_objects = total_allocated_objects;
}

void gc_collect() {
  collect(&g0);

#ifdef DEBUG_LOGS
  printf("AFTER COLLECTING\n");
  print_gc_state();
#endif
}

int get_size(const stella_object *obj) {
  const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
  return (1 + field_count) * sizeof(void*);
}