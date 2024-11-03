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
#define MAX_ALLOC_SIZE (24 * 40)
#define MAX_CHANGED_NODES 1024
#define DEBUG_LOGS

int gc_roots_max_size = 0;
int gc_roots_top = 0;
void **gc_roots[MAX_GC_ROOTS];

int changed_nodes_top = 0;
void *changed_nodes[MAX_CHANGED_NODES];

struct gc_object {
  void* moved_to;
  stella_object stella_object;
};

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

// common funcs
void init_generation();
void alloc_stat_update(size_t size_in_bytes);
void gc_collect();
void print_separator();
void gc_collect_stat_update();
bool is_in_heap(const void* ptr, const void* heap, size_t heap_size);
size_t get_stella_object_size(const stella_object *obj);
void exit_with_out_memory_error();

// gc_object
size_t get_gc_object_size(const struct gc_object *obj);
struct gc_object* get_gc_object(void* st_ptr);
stella_object* get_stella_object(struct gc_object* gc_ptr);

// space
bool is_in_place(const struct space* space, const void* ptr);
bool has_enough_space(const struct space* space, size_t requested_size);
struct gc_object* alloc_in_space(struct space* space, size_t size_in_bytes);
void print_space(const struct space* space);

// generation
void print_state(const struct generation* g);
void* try_alloc(struct generation* g, size_t size_in_bytes);
void collect(struct generation* g);
bool chase(struct generation* g, struct gc_object *p);
void* forward(struct generation* g, void* p);

// public
void* gc_alloc(const size_t size_in_bytes) {
  init_generation();

  void *result = try_alloc(&g0, size_in_bytes);
  if (result == NULL) {
    gc_collect();

    result = try_alloc(&g0, size_in_bytes);
  }

  if (result != NULL) {
    alloc_stat_update(size_in_bytes); // todo add pointer
  } else {
    exit_with_out_memory_error();
  }

  return result;
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

void print_gc_alloc_stats() {
  print_separator();
  printf("STATS\n");
  print_separator();

  printf("Total memory allocation:  %d bytes (%d objects)\n", total_allocated_bytes, total_allocated_objects);
  printf("Total garbage collecting: %d\n", total_gc_collect);
  printf("Maximum residency:        %d bytes (%d objects)\n", max_allocated_bytes, max_allocated_objects);
  printf("Total memory use:         %d reads and %d writes\n", total_reads, total_writes);
  printf("Max GC roots stack size:  %d roots\n", gc_roots_max_size);

  print_separator();
}

void print_gc_state() {
  print_state(&g0);
  print_state(&g1);
  print_gc_roots();
}

void print_gc_roots() {
  printf("ROOTS:\n");
  print_separator();

  for (int i = 0; i < gc_roots_top; i++) {
    printf(
      "\tIDX: %-5d | ADDRESS: %-15p | FROM: %-5s | VALUE: %-15p\n",
      i,
      gc_roots[i],
      is_in_place(g0.from, *gc_roots[i])
        ? "G_0"
        : is_in_place(g1.from, *gc_roots[i])
          ? "G_1"
          : "OTHER",
      *gc_roots[i]
    );
  }

  print_separator();
}

// common
void init_generation() {
  if (g0.from != NULL) return;

  g0_space_from.gen = 0;
  g0_space_from.size = MAX_ALLOC_SIZE;
  g0_space_from.heap = malloc(MAX_ALLOC_SIZE);
  g0_space_from.next = g0_space_from.heap;

  const int g1_space_size = MAX_ALLOC_SIZE * 4;
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

void print_separator() {
  printf("=====================================================================================\n");
}

void gc_collect_stat_update() {
  total_gc_collect += 1;
}

bool is_in_heap(const void* ptr, const void* heap, const size_t heap_size) {
  return ptr >= heap && ptr < heap + heap_size;
}

void exit_with_out_memory_error() {
  printf("Out of memory!");
  exit(137); // выход? как обрабатывать отсутствие памяти
}

// gc_object
size_t get_stella_object_size(const stella_object *obj) {
  const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
  return (1 + field_count) * sizeof(void*);
}

size_t get_gc_object_size(const struct gc_object *obj) {
  const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->stella_object.object_header);
  return (2 + field_count) * sizeof(void*);
}

struct gc_object* get_gc_object(void* st_ptr) {
  return st_ptr - sizeof(void*);
}

stella_object* get_stella_object(struct gc_object* gc_ptr) {
  return &gc_ptr->stella_object;
}

// space
bool is_in_place(const struct space* space, const void* ptr) {
  return is_in_heap(ptr, space->heap, space->size);
}

bool has_enough_space(const struct space* space, const size_t requested_size) {
  return space->next + requested_size <= space->heap + space->size;
}

struct gc_object* alloc_in_space(struct space* space, const size_t size_in_bytes) {
  const size_t size = size_in_bytes + sizeof(void*);
  if (has_enough_space(space, size)) {
    struct gc_object *result = space->next;
    result->moved_to = NULL;
    result->stella_object.object_header = 0;
    space->next += size;

    return result;
  }

  return NULL;
}

void print_space(const struct space* space) {
  printf("OBJECTS:\n");
  for (void *start = space->heap; start < space->next; start += get_gc_object_size(start)) {
      const struct gc_object *gc_ptr = start;
      const int tag = STELLA_OBJECT_HEADER_TAG(gc_ptr->stella_object.object_header);
      printf("\tGC ADDRESS: %-15p | ST ADDRESS: %-15p | MOVED: %-15p | TAG: %-2d | FIELDS: ",
             gc_ptr,
             &gc_ptr->stella_object,
             gc_ptr->moved_to,
             tag);

      const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(gc_ptr->stella_object.object_header);
      for (int i = 0; i < field_count; i++) {
          printf("%-15p", gc_ptr->stella_object.object_fields[i]);
          if (i < field_count - 1) {
              printf(" ");
          }
      }

      printf("\n");
  }

  // Кол-во выделенной памяти
  printf("BOUNDARIES  | FROM: %-15p | TO: %-15p | TOTAL: %d bytes\n",
         space->heap,
         space->heap + space->size,
         space->size);
  printf("FREE MEMORY | FROM: %-15p | TO: %-15p | TOTAL: %ld bytes\n",
         space->next,
         space->heap + space->size,
         space->heap + space->size - space->next);

}

// generation
void print_state(const struct generation* g) {
  print_separator();
  printf("G_%d STATE\n", g->number);
  print_separator();

  printf("COLLECT COUNT %d\n", g->collect_count);
  print_space(g->from);

  if (g->to->gen == g->from->gen) {
    printf("TO SPACE\n");
    print_space(g->to);
  }

  printf("SCAN: %-15p | NEXT: %-15p | LIMIT: %-15p\n", g->scan, g->to->next, g->to->heap + g->to->size);

  print_separator();
}

void* try_alloc(struct generation* g, const size_t size_in_bytes) {
  void* allocated = alloc_in_space(g->from, size_in_bytes);
  if (allocated == NULL) {
    return NULL;
  }

  return allocated + sizeof(void*);
}

bool chase(struct generation* g, struct gc_object *p) {
  do {
    struct gc_object *q = alloc_in_space(g->to, get_stella_object_size(&p->stella_object));
    if (q == NULL) {
      return false;
    }

    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(p->stella_object.object_header);
    void *r = NULL;

    q->moved_to = NULL;
    q->stella_object.object_header = p->stella_object.object_header;
    for (int i = 0; i < field_count; i++) {
      q->stella_object.object_fields[i] = p->stella_object.object_fields[i];

      if (is_in_place(g->from, q->stella_object.object_fields[i])) {
        struct gc_object *potentially_forwarded = get_gc_object(q->stella_object.object_fields[i]);

        if (!is_in_place(g->to, potentially_forwarded->moved_to)) {
          r = potentially_forwarded;
        }
      }
    }

    p->moved_to = q;
    p = r;
  } while (p != NULL);

  return true;
}

void* forward(struct generation* g, void* p) {
  if (!is_in_place(g->from, p)) {
    return p;
  }

  struct gc_object* gc_object = get_gc_object(p);

  if (is_in_place(g->to, gc_object->moved_to)) {
    return get_stella_object(gc_object->moved_to);
  }

  bool chase_result = chase(g, gc_object);
  if (!chase_result) {
    if (g->to->gen == g->from->gen) {
      exit_with_out_memory_error();
    }

    collect(generations[g->to->gen]);

    chase_result = chase(g, gc_object);
    if (!chase_result) {
      exit_with_out_memory_error();
    }
  }
  return get_stella_object(gc_object->moved_to);
}

void collect(struct generation* g) {
  g->collect_count++;
  gc_collect_stat_update();

#ifdef DEBUG_LOGS
  print_separator();
  printf("COLLECTING G_%d - COLLECTING NUMBER %d\n", g->number, g->collect_count);
  print_gc_state();
#endif

  g->scan = g->to->next;

  for (int i = 0; i < gc_roots_top; i++) {
    void **root_ptr = gc_roots[i];
    *root_ptr = forward(g, *root_ptr);
  }

#ifdef DEBUG_LOGS
  print_separator();
  printf("FORWARD ALL ROOTS\n");
  print_gc_state();
#endif

  // run for all objects in prev generations and try find link to collected generation
  for (int i = 0; i < g->number; i++) {
    const struct generation* past_gen = generations[i];

    for (void *ptr = past_gen->from->heap; ptr < past_gen->from->next; ptr += get_gc_object_size(ptr)) {
      struct gc_object *obj = ptr;
      const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->stella_object.object_header);
      for (int j = 0; j < field_count; j++) {
        obj->stella_object.object_fields[j] = forward(g, obj->stella_object.object_fields[j]);
      }
    }
  }

#ifdef DEBUG_LOGS
  print_separator();
  printf("FORWARD FIELDS OF OBJECT FROM EARLY GEN\n");
  print_gc_state();
#endif

  // changed objects
  for (int i = 0; i < changed_nodes_top; i++) {
    stella_object *obj = changed_nodes[i];
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    for (int j = 0; j < field_count; j++) {
      obj->object_fields[j] = forward(g, obj->object_fields[j]);
    }

    changed_nodes_top = 0;
  }

#ifdef DEBUG_LOGS
  print_separator();
  printf("FORWARD CHANGED OBJECT FIELDS\n");
  print_gc_state();
#endif

  while (g->scan < g->to->next) {
    struct gc_object *obj = g->scan;
    const int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->stella_object.object_header);
    for (int i = 0; i < field_count; i++) {
      obj->stella_object.object_fields[i] = forward(g, obj->stella_object.object_fields[i]);
    }

    g->scan += get_gc_object_size(obj);
  }

#ifdef DEBUG_LOGS
  print_separator();
  printf("SCAN TO NEXT\n");
  print_gc_state();
#endif

  if (g->from->gen == g->to->gen) { // copying gc
    void *buff = g->from;
    g->from = g->to;
    g->to = buff;

    g->to->next = g->to->heap;

    struct generation* past = generations[g->from->gen - 1];
    past->to = g->from;
    past->scan = g->from->heap; // i hope its will work (run from start because struct of from can change)
  } else { // generations
    struct generation* current = generations[g->from->gen];
    const struct generation* next = generations[g->to->gen];
    current->from->next = g->from->heap;
    current->to = next->from;
  }

#ifdef DEBUG_LOGS
  print_separator();
  printf("END OF COLLECTING\n");
  print_gc_state();
#endif
}