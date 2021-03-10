#ifndef MY_VM_H_INCLUDED
#define MY_VM_H_INCLUDED
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <pthread.h>

//Assume the address space is 32 bits, so the max memory size is 4GB
//Page size is 4KB

//Add any important includes here which you may need

#define PGSIZE 4096

// Maximum size of your memory
#define MAX_MEMSIZE 4ULL*1024*1024*1024 //4GB

#define MEMSIZE 1024*1024*1024

// Represents a page table entry
typedef void* pte_t;    // THE ACTUAL PHYSICAL ADDRESS

// Represents a page directory entry
typedef pte_t* pde_t;   // A TABLE OF pte_t ENTRIES

// Represents the page directory
typedef pde_t* pgdir_t;

#define TLB_SIZE 120

//Structure to represents TLB

/* TLB ds
*					   | next | -> | next | ->
*	[ oldest ] <->	   |	  |	   |	  |		<-> [ newest ]
*					<- | prev | <- | prev |
*/

typedef struct _TLB_node {
	void* va;
    void* pa;
	struct _TLB_node* next;	// the value added before this one
	struct _TLB_node* prev; // the value added after this one
} TLB_node;

typedef struct _TLB {
    // TODO: add hash table in tlb to speed up look ups, not that it really matters since this is still slower than not using this, since we don't have access to a faster memory buffer
	TLB_node* oldest;
	TLB_node* newest;
	int count;
    int miss_count;
    int hit_count;
} TLB;

void PrintPageDir();
void SetPhysicalMem();
// pde_t Translate(pgdir_t pgdir, void *va);
// int PageMap(pgdir_t pgdir, void *va, void* pa);
pte_t Translate(void *va);
int PageMap(void *va, void* pa);
pte_t check_TLB(void *va);
void add_TLB(void *va, void *pa);
void *myalloc(unsigned int num_bytes);
int myfree(void *va, int size);
int PutVal(void *va, void *val, int size);
int GetVal(void *va, void *val, int size);
void MatMult(void *mat1, void *mat2, int size, void *answer);
void print_TLB_missrate();

#endif
