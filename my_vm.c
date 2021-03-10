#include "my_vm.h"

// "physical" memory
static void* memory = NULL;

static int is_initilized = 0;
static int virtual_pages = -1;
static int physical_pages = -1;
static unsigned char* virtual_bitmap = NULL;
static unsigned char* physical_bitmap = NULL;

// number of entries in table/directory
static int table_size = -1;
static int directory_size = -1;

// index bit counts
static int offset_size = -1;
static int lower_size = -1;
static int upper_size = -1;

// bit mask to get upper or lower index
static size_t upper_mask = -1;
static size_t lower_mask = -1;
static size_t offset_mask = -1;

// global data structure
static pde_t *pgdir = NULL;
static TLB* tlb_store = NULL;

static pthread_mutex_t bitmap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pagedir_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tlb_lock = PTHREAD_MUTEX_INITIALIZER;

TLB* init_TLB() {
	TLB* tlb = malloc(sizeof(TLB));
	tlb->count = 0;
    tlb->hit_count = 0;
    tlb->miss_count = 0;
	tlb->oldest = NULL;
	tlb->newest = NULL;
	return tlb;
}

TLB_node* init_TLB_Node(void* va, void* pa) {
	TLB_node* new_node = malloc(sizeof(TLB_node));
	new_node->va = va;
    new_node->pa = pa;
	new_node->next = NULL;
	new_node->prev = NULL;
	return new_node;
}

// create a bit mask to for upper or lower index
static size_t createMask(size_t a, size_t b) {
    return ((1ull << (b - a)) - 1ull) << a;
}

void SetPhysicalMem() {

    if (is_initilized) return;

    // allocate "physical" memory
    memory = malloc(MEMSIZE);
    
    // calculate number of virtual/physical pages
    virtual_pages = MAX_MEMSIZE / PGSIZE;
    physical_pages = MEMSIZE / PGSIZE;

    // allocate memory bitmaps
    virtual_bitmap = calloc(sizeof(char), virtual_pages);
    physical_bitmap = calloc(sizeof(char), physical_pages);

    // Number of bits for the offset, upper and lower indicies
    offset_size = log(PGSIZE) / log(2);
    upper_size = floor((32- offset_size) / 2);
    lower_size = floor((32- offset_size) / 2) + ((32 - offset_size)%2 != 0 ? 1 : 0);
    
    // calculate bit masks for indices 
    upper_mask = createMask(offset_size + lower_size, offset_size + lower_size + upper_size);
    lower_mask = createMask(offset_size, offset_size + lower_size);
    offset_mask = createMask(0, offset_size);

    // total number of directory and table entries possible
    directory_size = pow(2, upper_size);
    table_size = pow(2, lower_size);

    // Each table is initialized as NULL
    // Each table (ie 1 pde_t) is an array of pte_t (table entries)
    // Each table entry (1 pte_t) is a pointer to an address (physical address)
    pgdir = calloc(sizeof(pde_t), directory_size);

    // initilize translational lookaside buffer data structure
    tlb_store = init_TLB();

    is_initilized = true;

}

void add_TLB(void* va, void* pa) {

	if (tlb_store->count == TLB_SIZE) {
        // if tlb is full, remove the oldest entry
		tlb_store->oldest = tlb_store->oldest->next;
		tlb_store->count--;
	}
	
	TLB_node* new_node = init_TLB_Node(va, pa);

    // this new entry is the newest entry
	new_node->prev = tlb_store->newest;
	if (tlb_store->newest != NULL) tlb_store->newest->next = new_node;
	tlb_store->newest = new_node;

    // if there is no oldest, this new entry is the oldest
	if (tlb_store->oldest == NULL) tlb_store->oldest = new_node;
	
	tlb_store->count++;

}

pte_t check_TLB(void *va) {

	if (tlb_store == NULL) return NULL;

	TLB_node* curr = tlb_store->oldest;

    // walk through all entries in the tlb to find the virtual address
	while (curr != NULL) {
		if ((size_t) va == (size_t)curr->va) {
            // va has been found, it's a hit
            tlb_store->hit_count++;
            return curr->pa;
        }
		curr = curr->next;
	}

    // if the va has not yet been found, it's a miss
    tlb_store->miss_count++;
    return NULL;

}

void print_TLB_missrate() {
    double miss_rate = ((double) tlb_store->miss_count) / (tlb_store->hit_count + tlb_store->miss_count);
    fprintf(stderr, "TLB miss rate %lf \n", miss_rate);
}

pte_t Translate(void *va) {

    // Check TLB first
    pthread_mutex_lock(&tlb_lock);
    pte_t physical; 
    if ((physical = check_TLB(va)) != NULL) {
        pthread_mutex_unlock(&tlb_lock);
        return physical;
    }

    // get the index from the address
    size_t upper_index = ((size_t) va & upper_mask) >> (offset_size + lower_size);
    size_t lower_index = ((size_t) va & lower_mask) >> (offset_size); 
    size_t offset = (size_t) va & offset_mask; 

    // use the calculated index to check the bit map and check that the address has been allocated
    // if its bitmap is 0 then it hasnot been mapped yet
    if (virtual_bitmap[(upper_index * table_size) + lower_index] == 0) return NULL;

    // gets the base physical address from the page directory/table
    physical = pgdir[upper_index][lower_index];           // get the physical address upper & lower bits
    physical = (pte_t) ((unsigned long)physical + offset);   // attatch the offset value
    add_TLB(va, physical);
    pthread_mutex_unlock(&tlb_lock);

    return physical;
}

int PageMap(void *va, void *pa) {

    // get the index from the address
    size_t upper_index = ((size_t) va & upper_mask) >> (offset_size + lower_size);
    size_t lower_index = ((size_t) va & lower_mask) >> (offset_size); 
    size_t offset = (size_t) va & offset_mask; 

    // No need to lock bitmap mutex, it will be locked before PageMap is called
    int page_num = upper_index * table_size + lower_index;
    if (virtual_bitmap[page_num] == 0) return -1;

    pthread_mutex_lock(&pagedir_lock);
    // put the physical address into the page mapping
    if (pgdir[upper_index] == NULL) {
        pgdir[upper_index] = malloc(sizeof(pte_t) * table_size);
        if (!pgdir[upper_index]) return -1;
        for (int i = 0; i < table_size; i++) {
            pgdir[upper_index][i] = NULL;
        }
    }

    pgdir[upper_index][lower_index] = pa;
    pthread_mutex_unlock(&pagedir_lock);

    return 0;

}

int get_next_avail(int num_pages, void** first_page) {

    // Use virtual address bitmap to find the next free page
    // start at 1 to skip address 0
    for (int i = 1; i < virtual_pages; i++) {

        if (virtual_bitmap[i] == 0) {
            
            // when the first available page has been found
            // check that there is (num_pages - 1) more consecutive pages available
            int j = 1;
            for (j = 1; j <= (num_pages - 1); j++){
                if (virtual_bitmap[i + j] == 1) break;
            }
            
            if (j == num_pages) {

                // i is the index of next available page
                int upper_index = floor(i / table_size);
                int lower_index = floor(i % table_size);

                // append the indicies to get the va
                // upper_index + lower_index + offset(all 0)
                size_t address = 0;
                address = (upper_index << offset_size + lower_size) |  (lower_index << offset_size);

                // the virtual address of the available page
                void* va = (void*) address;

                (*first_page) = va;
                return 0;

            } // else there is not enough consecutive pages

        }
    }

    return -1;

}

int get_avail_physical(int num_pages, int pages[num_pages]) {

    int found = 0;
    // finds num_pages of physical pages, saves each of their page numbers in an array
    // pages do not have to be consecutive
    for (int i = 1; i < physical_pages && found < num_pages; i++) {
        if (physical_bitmap[i] == 0) {
            pages[found] = i;
            found++;
        }
    }

    if (found != num_pages) return -1;
    return 0;

}

void* page_num_to_address(int page_num) {
    size_t x = floor(page_num / table_size);
    size_t y = page_num % table_size;
    void* address = (void*) (x * table_size + y * PGSIZE);
}

int address_to_page_num(void* address, void* memory_offset) {
    size_t base_address = (size_t) address - (size_t) memory_offset; // get the base physical address, since our "physical" memory will not start at 0x0
    size_t upper_index = ((size_t) base_address & upper_mask) >> (offset_size + lower_size);
    size_t lower_index = ((size_t) base_address & lower_mask) >> (offset_size);
    int page_num = upper_index * table_size + lower_index;
}

void *myalloc(unsigned int num_bytes) {

    pthread_mutex_lock(&bitmap_lock);
    pthread_mutex_lock(&pagedir_lock);
    if (!is_initilized) SetPhysicalMem();
    pthread_mutex_unlock(&pagedir_lock);
    
    // minimum number of pages required to store the necessary bytes
    int num_pages = ceil(num_bytes * 1.0f / PGSIZE);
    
    void* va_first = NULL;
    if (get_next_avail(num_pages, &va_first) != 0) {
        // was not able to find a contiguous section of virtual memory to assign
        printf("[MyVM] failed to allocate, insufficient virtual memmory\n");
        return NULL;
    }

    int pa_nums[num_pages];
    if (get_avail_physical(num_pages, pa_nums) != 0) {
        // was not able to find enough physical memory for the requested number of bytes
        printf("[MyVM] failed to allocate, insufficient memmory\n");
        return NULL;
    } 

    int virtual_page_num = address_to_page_num(va_first, (void*) 0);
    for (int i = 0; i < num_pages; i++) {

        // mark the page maps as allocated
        virtual_bitmap[virtual_page_num + i] = 1;
        physical_bitmap[pa_nums[i]] = 1;
        
        void* pa = (void*)((size_t) page_num_to_address(pa_nums[i]) + (size_t) memory);
        void* va = va_first + (PGSIZE * i);

        if (PageMap(va, pa) != 0) {
            printf("[MyVM] memory error, failed to allocate\n");
            return NULL;
        }

    }

    pthread_mutex_unlock(&bitmap_lock);

    // return pointer to the first virtual memory page
    return va_first;
}

int myfree(void *va, int size) {

    if (!is_initilized) return -1;

    int num_pages = ceil(size * 1.0f / PGSIZE);
    void* physical_addresses[num_pages];

    pthread_mutex_lock(&bitmap_lock);
    pthread_mutex_lock(&pagedir_lock);

    // check that all virtual addresses va to va+(PGSIZE * i) are allocated
    for (int i = 0; i < num_pages; i++) {
        void* va_curr = va + (PGSIZE * i);
        physical_addresses[i] = Translate(va_curr);
        if (physical_addresses[i] == NULL) {
            printf("[MyVM] myfree(): attempting to free invalid address %p\n", va_curr);
            return -1;
        }
    }

    for (int i = 0; i < num_pages; i++) {

        void* va_curr = va + (PGSIZE * i);
        void* pa = physical_addresses[i];

        int physical_page_num = address_to_page_num(pa, memory);

        size_t upper_index = ((size_t) va_curr & upper_mask) >> (offset_size + lower_size);
        size_t lower_index = ((size_t) va_curr & lower_mask) >> (offset_size);
        int virtual_page_num = upper_index * table_size + lower_index;
        
        // remove the mapping from the page table
        pgdir[upper_index][lower_index] = NULL;

        // mark the page as no longer allocated
        physical_bitmap[physical_page_num] = 0;
        virtual_bitmap[virtual_page_num] = 0;

        // check if all table entries are empty, if they are, free the table
        for (int j = 0; j < table_size; j++) {
            if (pgdir[upper_index] != NULL) break;
            if (j == table_size - 1) {
                free(pgdir[upper_index]);
                break;
            }
        }

    }
    pthread_mutex_unlock(&pagedir_lock);
    pthread_mutex_unlock(&bitmap_lock);

}

int PutVal(void *va, void *val, int size) {

    if (!is_initilized) return -1;

    int num_pages = ceil(size * 1.0f / PGSIZE);
    int bytes_put = 0;
    for (int i = 0; i < num_pages; i++) {

        pthread_mutex_lock(&bitmap_lock);
        pte_t* entry = Translate(va + (PGSIZE * i));
        // check that the virtual address is valid
        if (entry == NULL) {
            printf("[MyVM] segmentation fault: attempting to write data to an invalid address %p\n", va + (PGSIZE * i));
            return -1;
        }
        // if the number of bytes to put in the page is less then the size of a page, only move that many bytes 
        int to_put = (size-bytes_put) < PGSIZE ? (size-bytes_put) : PGSIZE;
        // directly copy the bytes into the physical address
        memcpy(entry, val + bytes_put, to_put);
        bytes_put += to_put;
        pthread_mutex_unlock(&bitmap_lock);
    }

    return 0;

}


int GetVal(void *va, void *val, int size) {

    if (!is_initilized) return -1;

    int num_pages = ceil(size * 1.0f / PGSIZE);
    int bytes_got = 0;
    for (int i = 0; i < num_pages; i++) {

        pthread_mutex_lock(&bitmap_lock);
        pte_t* entry = Translate(va + (PGSIZE * i));
        // check that the virtual address is valid
        if (entry == NULL) {
            printf("[MyVM] segmentation fault: attempting to read data from an invalid address %p\n", va + (PGSIZE * i));
            return -1;
        }

        // if the number of bytes to get from the page is less then the size of a page, only move that many bytes 
        int to_get = (size-bytes_got) < PGSIZE ? (size-bytes_got) : PGSIZE;
        // directly copy the bytes into the pointer
        memcpy(val + bytes_got, entry, to_get);
        bytes_got += to_get;
        pthread_mutex_unlock(&bitmap_lock);
    }

    return 0;
}

void MatMult(void *mat1, void *mat2, int size, void *answer) {

    int i, j, k;
    for (i = 0; i < size; i++) {
        for(j = 0; j < size; j++) {
            for(k = 0; k < size; k++) {
                int i_k = 0;
                GetVal(mat1 + sizeof(int) * (i*size + k), &i_k, sizeof(int));
                
                int k_j = 0;
                GetVal(mat2 + sizeof(int) * (k*size + j), &k_j, sizeof(int));
                
                int i_j_init;
                GetVal(answer + sizeof(int) * (i*size + j), &i_j_init, sizeof(int));                

                int i_j = (i_k * k_j) + i_j_init;
                PutVal(answer + ((i * size * sizeof(int))) + (j * sizeof(int)), &i_j, sizeof(int));
            }
        } 
    }


} 
