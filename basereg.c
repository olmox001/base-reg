#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================
   CONFIG
   ========================== */
#define MAX_ARRAYS 32
#define NAME_LEN 32
#define POOL_SIZE (1024 * 1024) /* 1MB */
#define MIN_BLOCK 32

/* ==========================
   BUDDY ALLOCATOR (CORRETTO)
   ========================== */

#define NUM_LEAVES (POOL_SIZE / MIN_BLOCK)
#define TREE_NODES (2 * NUM_LEAVES - 1)

static uint8_t memory_pool[POOL_SIZE];

/* Stati del Buddy Tree:
   0 = FREE (Libero)
   1 = SPLIT (Diviso, i figli contengono dati)
   2 = USED (Occupato interamente)
*/
static uint8_t buddy_tree[TREE_NODES];

size_t next_pow2(size_t x) {
  size_t p = MIN_BLOCK;
  while (p < x)
    p <<= 1;
  return p;
}

// Naviga l'albero per allocare un blocco e restituisce l'offset di memoria
int buddy_alloc_node(size_t node, size_t node_size, size_t node_offset,
                     size_t req_size, size_t *out_offset) {
  if (node >= TREE_NODES)
    return 0; // Guardia di sicurezza
  if (buddy_tree[node] == 2)
    return 0; // Nodo già in uso
  if (node_size < req_size)
    return 0; // Nodo troppo piccolo

  // Perfetto incastro trovato
  if (node_size == req_size && buddy_tree[node] == 0) {
    buddy_tree[node] = 2; // Contrassegna come USED
    *out_offset = node_offset;
    return 1;
  }

  // Se è libero, va diviso (SPLIT) per poter usare un figlio
  if (buddy_tree[node] == 0) {
    if (node_size <= MIN_BLOCK)
      return 0; // Impossibile dividere oltre il blocco minimo
    buddy_tree[node] = 1;
  }

  size_t child_size = node_size / 2;
  // Tenta di allocare nel ramo sinistro
  if (buddy_alloc_node(2 * node + 1, child_size, node_offset, req_size,
                       out_offset))
    return 1;
  // Se fallisce, tenta nel ramo destro
  if (buddy_alloc_node(2 * node + 2, child_size, node_offset + child_size,
                       req_size, out_offset))
    return 1;

  return 0; // Nessuno spazio disponibile in questo ramo
}

void *buddy_alloc(size_t size) {
  if (size == 0 || size > POOL_SIZE)
    return NULL;
  size_t req = next_pow2(size);

  size_t offset = 0;
  if (buddy_alloc_node(0, POOL_SIZE, 0, req, &offset)) {
    return memory_pool + offset;
  }
  return NULL; // Out of memory
}

// Libera un nodo a partire dall'offset e unisce ricorsivamente i buddy liberi
void buddy_free_node(size_t node, size_t node_size, size_t node_offset,
                     size_t target_offset) {
  if (node >= TREE_NODES)
    return;
  if (buddy_tree[node] == 0)
    return; // Già libero

  if (buddy_tree[node] == 2) { // Trovato il blocco occupato
    if (node_offset == target_offset) {
      buddy_tree[node] = 0; // Libera
    }
    return;
  }

  // Se è diviso (SPLIT), cerca nel ramo corretto
  size_t child_size = node_size / 2;
  if (target_offset < node_offset + child_size) {
    buddy_free_node(2 * node + 1, child_size, node_offset, target_offset);
  } else {
    buddy_free_node(2 * node + 2, child_size, node_offset + child_size,
                    target_offset);
  }

  // Coalescenza: Se entrambi i figli sono liberi, unisci i blocchi
  // contrassegnando il padre come libero
  if (buddy_tree[node] == 1) {
    if ((2 * node + 2) < TREE_NODES) {
      if (buddy_tree[2 * node + 1] == 0 && buddy_tree[2 * node + 2] == 0) {
        buddy_tree[node] = 0;
      }
    }
  }
}

void buddy_free(void *ptr) {
  if (!ptr)
    return;
  size_t offset = (uint8_t *)ptr - memory_pool;
  buddy_free_node(0, POOL_SIZE, 0, offset);
}

/* ==========================
   GENERIC VALUE
   ========================== */

typedef enum { TYPE_INT } ValueType;

typedef struct {
  ValueType type;
  void *data;
} Value;

const char *type_to_string(ValueType t) {
  switch (t) {
  case TYPE_INT:
    return "INT";
  default:
    return "UNKNOWN";
  }
}

/* ==========================
   DYNAMIC ARRAY
   ========================== */

typedef struct {
  char name[NAME_LEN];
  Value *items;
  size_t size;
  size_t capacity;
} DynArray;

DynArray *arrays[MAX_ARRAYS];
size_t array_count = 0;

void die(const char *msg) {
  fprintf(stderr, "Errore Critico: %s\n", msg);
  exit(EXIT_FAILURE);
}

void *xmalloc(size_t size) {
  void *p = buddy_alloc(size);
  if (!p)
    die("Buddy Alloc fallita (Memoria Piena o Frammentata)");
  memset(p, 0, size);
  return p;
}

void xfree(void *ptr) { buddy_free(ptr); }

DynArray *get_array(const char *name) {
  for (size_t i = 0; i < array_count; i++)
    if (strcmp(arrays[i]->name, name) == 0)
      return arrays[i];
  return NULL;
}

DynArray *create_array(const char *name) {
  DynArray *existing = get_array(name);
  if (existing)
    return existing;

  if (array_count >= MAX_ARRAYS)
    die("Limite Array Raggiunto");

  DynArray *arr = xmalloc(sizeof(DynArray));
  strncpy(arr->name, name, NAME_LEN - 1);
  arr->name[NAME_LEN - 1] = '\0';
  arr->capacity = 4;
  arr->size = 0;
  arr->items = xmalloc(arr->capacity * sizeof(Value));

  arrays[array_count++] = arr;
  return arr;
}

void ensure_capacity(DynArray *arr, size_t index) {
  if (index < arr->capacity)
    return;

  size_t newcap = arr->capacity;
  while (newcap <= index)
    newcap <<= 1;

  Value *new_items = xmalloc(newcap * sizeof(Value));
  memcpy(new_items, arr->items, arr->capacity * sizeof(Value));

  xfree(arr->items); // Ora funziona in modo sicuro grazie alla coalescenza
  arr->items = new_items;
  arr->capacity = newcap;
}

void set_value(DynArray *arr, size_t index, int val) {
  ensure_capacity(arr, index);

  if (!arr->items[index].data) {
    arr->items[index].data = xmalloc(sizeof(int));
  }

  *((int *)arr->items[index].data) = val;
  arr->items[index].type = TYPE_INT;

  if (index >= arr->size)
    arr->size = index + 1;
}

void delete_value(DynArray *arr, size_t index) {
  if (index >= arr->size || !arr->items[index].data)
    return;

  xfree(arr->items[index].data);
  arr->items[index].data = NULL;
}

void print_array(DynArray *arr) {
  printf("\n=== Informazioni Array: '%s' ===\n", arr->name);
  printf("[Indirizzo Registro: %p | Heap Struttura: %p]\n", (void *)&arr,
         (void *)arr);
  printf("[Capacità Allocata: %zu slots | Limite Massimo Index: %zu]\n\n",
         arr->capacity, arr->size);

  size_t active_count = 0;
  for (size_t i = 0; i < arr->size; i++) {
    if (arr->items[i].data) {
      active_count++;
      printf("  [%zu] %-4s = %-8d (Indirizzo Memoria: %p)\n", i,
             type_to_string(arr->items[i].type), *((int *)arr->items[i].data),
             arr->items[i].data);
    } else {
      printf("  [%zu] <NULL> (Spazio Riservato Vuoto)\n", i);
    }
  }
  printf("\n>>> Elementi validi trovati: %zu\n\n", active_count);
}

void list_arrays() {
  printf("\n============================ TABELLA HEAP ARRAY "
         "============================\n");
  printf("%-15s | %-6s | %-6s | %-6s | %-18s\n", "NOME", "SIZE", "CAP",
         "ATTIVI", "REGISTRO HEAP PNT");
  printf("---------------------------------------------------------------------"
         "-------\n");

  size_t total_active = 0;
  size_t total_cap = 0;

  for (size_t i = 0; i < array_count; i++) {
    size_t active = 0;
    for (size_t j = 0; j < arrays[i]->size; j++) {
      if (arrays[i]->items[j].data)
        active++;
    }

    printf("%-15s | %-6zu | %-6zu | %-6zu | %p\n", arrays[i]->name,
           arrays[i]->size, arrays[i]->capacity, active, (void *)arrays[i]);

    total_active += active;
    total_cap += arrays[i]->capacity;
  }
  printf("====================================================================="
         "=======\n");
  printf("Sommario Runtime: %zu array registrati | %zu valori attivi su %zu "
         "slots totali.\n\n",
         array_count, total_active, total_cap);
}

/* ==========================
   PARSER
   ========================== */

void trim(char *s) {
  char *p = s;
  while (isspace(*p))
    p++;
  memmove(s, p, strlen(p) + 1);

  for (int i = strlen(s) - 1; i >= 0 && isspace(s[i]); i--)
    s[i] = '\0';
}

void handle(char *line) {
  char name[NAME_LEN];
  size_t idx;
  int val;

  // Uso %31[^[] per prevenire buffer overflow su nome[]
  if (sscanf(line, "%31[^[][%zu] = %d", name, &idx, &val) == 3) {
    DynArray *a = create_array(name);
    set_value(a, idx, val);
    return;
  }

  if (sscanf(line, "del %31[^[][%zu]", name, &idx) == 2) {
    DynArray *a = get_array(name);
    if (a)
      delete_value(a, idx);
    return;
  }

  if (sscanf(line, "print %31s", name) == 1) {
    DynArray *a = get_array(name);
    if (a)
      print_array(a);
    else
      printf("Array '%s' non trovato.\n", name);
    return;
  }

  if (strcmp(line, "list") == 0) {
    list_arrays();
    return;
  }

  printf("Comando non valido\n");
}

/* ==========================
   MAIN
   ========================== */

int main() {
  char line[256];

  printf("Mini Runtime in esecuzione - allocatore: Buddy System (1MB)\n");
  printf("Comandi validi:\n");
  printf("  - nome[idx] = val\n");
  printf("  - del nome[idx]\n");
  printf("  - print nome\n");
  printf("  - list\n");
  printf("  - exit\n\n");

  while (1) {
    printf("> ");
    if (!fgets(line, sizeof(line), stdin))
      break;

    trim(line);
    if (strlen(line) == 0)
      continue;
    if (strcmp(line, "exit") == 0)
      break;

    handle(line);
  }

  return 0;
}
