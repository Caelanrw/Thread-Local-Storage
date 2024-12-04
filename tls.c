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




int tls_read(unsigned int offset, unsigned int length, char *buffer){
        //ERROR CHECKING
        pthread_t threadid = pthread_self();
        int hash = threadid % HASH_SIZE;
        struct hash_element *current = hash_table[hash];

        if(current == NULL){
                return -1;
        }

        int hasTLS = 0;
        while(current != NULL){
                if(threadid == current->tid){
                        hasTLS = 1;
                        break;
                }

                current = current->next;
        }

        if(!hasTLS)
        {
                return -1;
        }


        if(offset+length > current->tls->page_num * PAGE_SIZE){
                return -1;
        }


        //UNPROTECT PAGES
        int i;
        for(i=0; i<current->tls->page_num; i++){
                tls_unprotect(current->tls->pages[i]);
        }


        //PERFORM READ OPERATION
        int cnt, idx;
        char* src;
        for(cnt = 0, idx = offset; idx < (offset + length); ++cnt, ++idx){
                struct page *p;
                unsigned int pn, poff;

                pn = idx / PAGE_SIZE;
                poff = idx % PAGE_SIZE;

                p = current->tls->pages[pn];
                src = ((char*) p->address) + poff;

                buffer[cnt] = *src;
        }


        //REPROTECT PAGES
        for(i=0; i<current->tls->page_num; i++){
                tls_protect(current->tls->pages[i]);
        }


        return 0;
}




int tls_write(unsigned int offset, unsigned int length, char *buffer){
        //ERROR CHECKING
        pthread_t threadid = pthread_self();
        int hash = threadid % HASH_SIZE;
        struct hash_element *current = hash_table[hash];

        if(current == NULL){
                return -1;
        }

        int hasTLS = 0;
        while(current != NULL){
                if(threadid == current->tid){
                        hasTLS = 1;
                        break;
                }

                current = current->next;
        }

        if(!hasTLS)
        {
                return -1;
        }


        if(offset+length > current->tls->page_num * PAGE_SIZE){
                return -1;
        }


        //UNPROTECT PAGES
        int i;
        for(i = 0; i < current->tls->page_num; i++){
                tls_unprotect(current->tls->pages[i]);
        }


        //PERFORM WRITE OPERATION
        int cnt, idx;
        char* dst;
        for(cnt= 0, idx = offset; idx < (offset+length); ++cnt, ++idx){
                struct page *p, *copy;
                unsigned int pn, poff;
                pn = idx / PAGE_SIZE;
                poff = idx % PAGE_SIZE;
                p = current->tls->pages[pn];
                if(p->ref_count > 1){
                        //CREATE PRIVATE COPY (COW)
                        copy = (struct page*) calloc(1, sizeof(struct page));
                        copy->address = (unsigned long int)mmap(0, PAGE_SIZE, PROT_WRITE, MAP_ANON | MAP_PRIVATE, 0, 0);
                        copy->ref_count = 1;
                        current->tls->pages[pn] = copy;

                        p->ref_count--;
                        tls_protect(p);
                        p = copy;
                }
                dst = ((char*)p->address) + poff;
                *dst = buffer[cnt];
        }


        //REPROTECT PAGES
        for(i = 0; i < current->tls->page_num; i++){
                tls_protect(current->tls->pages[i]);
        }


        return 0;
}




int tls_destroy(){
        //ERROR HANDLING
        pthread_t threadid = pthread_self();
        int hash = threadid % HASH_SIZE;
        struct hash_element *current = hash_table[hash];
        struct hash_element *prev = NULL;

        if(current == NULL){
                return -1;
        }

        int hasTLS = 0;
        while(current != NULL){
                if(threadid == current->tid){
                        hasTLS = 1;
                        break;
                }
                prev = current;
                current = current->next;
        }

        if(!hasTLS)
        {
                return -1;
        }


        //CLEAN UP ALL PAGES
        int i;
        for(i = 0; i < current->tls->page_num; i++){
                if ( current->tls->pages[i]->ref_count <= 1){
                        free(current->tls->pages[i]);
                }

                current->tls->pages[i]->ref_count--;
        }


        //CLEAN UP TLS
        free(current->tls->pages);
        free(current->tls);


        //REMOVE MAPPING FROM GLOBAL DATA STRUCTURE
        if(prev == NULL)
        {
                //current was at the head of tthe chain
                hash_table[hash] = current->next;
        }
        else
        {
                //remove node from linked list
                prev->next = current->next;
        }

        free(current);

        return 0;
}




int tls_clone(pthread_t tid){
        //ERROR HANDLING
        pthread_t threadID = pthread_self();
        int hash = threadID % HASH_SIZE;
        struct hash_element *current = hash_table[hash];

        while(current != NULL){
                if(threadID == current->tid){
                        return -1;      //TLS ALREADY EXISTS
                }

                current = current->next;
        }



        hash = tid % HASH_SIZE;
        current = hash_table[hash];

        if(current == NULL){
                //target thread has no tls
                return -1;
        }

        int hasTLS = 0;
        while(current != NULL){
                if(tid == current->tid){
                        hasTLS = 1;
                        break;
                }

                current = current->next;
        }

        if(!hasTLS)
        {
                //target thread has no tls
                return -1;
        }


        //DO CLONING, ALLOCATE TLS
        TLS *tls = calloc(1, sizeof(TLS));
        tls->tid = pthread_self();
        tls->size = current->tls->size;
        tls->page_num = current->tls->page_num;
        tls->pages = calloc(tls->page_num, sizeof(struct page*));


        //COPY PAGES, ADJUST REFERENCE COUNTS
        int i;
        for(i=0; i < tls->page_num; i++){
                tls->pages[i] = current->tls->pages[i];
                current->tls->pages[i]->ref_count++;
        }


        //ADD THREAD/TLS TO GLOBAL DATA STRUCTURE
        struct hash_element *new_elem = malloc(sizeof(struct hash_element));
        new_elem->next = NULL;
        new_elem->tid = pthread_self();
        new_elem->tls = tls;

        hash = new_elem->tid % HASH_SIZE;
        struct hash_element *last = hash_table[hash];

        if(last == NULL){
                hash_table[hash] = new_elem;
        }
        else{
                while(last->next != NULL){
                        last = last->next;
                }
                last->next = new_elem;
        }

        return 0;
}
