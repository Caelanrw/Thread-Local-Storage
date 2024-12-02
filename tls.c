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


void tls_init(){
        PAGE_SIZE = getpagesize();

        //set up the signal handler

        struct sigaction sa;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;       /*tells sigaction to use sa_sigaction instead of handler*/
        sa.sa_sigaction = handle_pf;

        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);

        initialized = 1;
        return;
}


void tls_protect(struct page *p){
        if(mprotect((void*)p->address, PAGE_SIZE, PROT_NONE)){
                fprintf(stderr, "tls_protect: could not protect page\n");
                exit(1);
        }
}



void tls_unprotect(struct page *p){
        if(mprotect((void*)p->address, PAGE_SIZE, PROT_READ | PROT_WRITE)){
                fprintf(stderr, "tls_unprotect: could not unprotect page\n");
                exit(1);
        }
}




void handle_pf(int sig, siginfo_t *si, void *context){
        unsigned long int p_fault = ((unsigned long int)si->si_addr) & ~(PAGE_SIZE - 1);

        int i;
        struct hash_element *current;
        for(i=0; i<HASH_SIZE; i++){
                current = hash_table[i];
                while(current->next != NULL){
                        int pageIndex;
                        for(pageIndex=0; pageIndex < current->tls->page_num; pageIndex++){
                                if(current->tls->pages[pageIndex]->address == p_fault){
                                        pthread_exit(NULL);
                                }
                        }
                        current = current->next;
                }
                int pageIndex;
                for(pageIndex=0; pageIndex < current->tls->page_num; pageIndex++){
                        if(current->tls->pages[pageIndex]->address == p_fault){
                                pthread_exit(NULL);
                        }
                }
        }

        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        raise(sig);

        return;
}
