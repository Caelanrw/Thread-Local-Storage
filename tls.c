#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#define HASH_SIZE 128



typedef struct thread_local_storage{
        pthread_t tid;
        unsigned int size;  /*size in bytes*/
        unsigned int page_num;    /*number of pages*/
        struct page **pages;    /*array of pointers to pages*/
}TLS;



struct page{
        unsigned long int address;      /*start address of page*/
        int ref_count;  /*counter for shared pages*/
};



struct hash_element{
        pthread_t tid;
        TLS *tls;
        struct hash_element *next;
};




struct hash_element* hash_table[HASH_SIZE];
int initialized = 0;
int PAGE_SIZE;


int tls_create(unsigned int size){
        //INITIALIZE
        if(!initialized){
                tls_init();
        }

        //ERROR HANDLING
        if(size <= 0){
                return -1;
        }

        pthread_t threadID = pthread_self();
        int hash = threadID % HASH_SIZE;
        struct hash_element *current = hash_table[hash];
        struct hash_element *last = current;

        while(current != NULL){
                if(threadID == current->tid){
                        return -1;      //TLS ALREADY EXISTS
                }

                last = current;
                current = current->next;
        }


        //ALLOCATE TLS
        TLS *tls = calloc(1, sizeof(TLS));


        //INITIALIZE TLS
        tls->tid = threadID;
        tls->size = size;
        int page_size = getpagesize();
        tls->page_num = (size + (page_size-1)) / page_size;


        //ALLOCATE TLS PAGES ARRAY
        tls->pages = calloc(tls->page_num, sizeof(struct page*));


        //ALLOCATE TLS PAGES
        int i;
        for(i=0; i<tls->page_num; i++){
                struct page *p = malloc(sizeof(struct page));
                p->address = (unsigned long int)mmap(0, PAGE_SIZE, PROT_NONE, MAP_ANON | MAP_PRIVATE, 0,0);
                p->ref_count = 1;
                tls->pages[i] = p;
        }


        //ADD THREADID AND TLS TO GLOBAL DATA STRUCTURE

        struct hash_element *new_elem = malloc(sizeof(struct hash_element));
        new_elem->next = NULL;
        new_elem->tid = threadID;
        new_elem->tls = tls;

        if(last == NULL){
                hash_table[hash] = new_elem;
        }
        else{
                last->next = new_elem;
        }

        return 0;
}
