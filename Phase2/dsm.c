#include "dsm_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>    
#include <netinet/in.h>   
#include <arpa/inet.h>     
#include <netdb.h>
#include <ifaddrs.h>
#include <poll.h>
#include <errno.h>

int DSM_NODE_NUM; /* nombre de processus dsm */
int DSM_NODE_ID;  /* rang (= numero) du processus */
int DSMEXEC_FD;
int MASTER_FD;
int count_fini;

// Déclarations des fonctions statiques nécessaires pour dsm_comm_daemon
static int dsm_send(int dest,void *buf,size_t size);
static int dsm_recv(int from,void *buf,size_t size);


static dsm_proc_conn_t *procs = NULL;
static dsm_page_info_t table_page[PAGE_NUMBER];
static pthread_t comm_daemon;


/* indique l'adresse de debut de la page de numero numpage */
static char *num2address( int numpage )
{ 
   char *pointer = (char *)(BASE_ADDR+(numpage*(PAGE_SIZE)));
   
   if( pointer >= (char *)TOP_ADDR ){
      fprintf(stderr,"[%i] Invalid address !\n", DSM_NODE_ID);
      return NULL;
   }
   else return pointer;
}

/* cette fonction permet de recuperer un numero de page */
/* a partir  d'une adresse  quelconque */
static int address2num( char *addr )
{
  return (((intptr_t)(addr - BASE_ADDR))/(PAGE_SIZE));
}

/* cette fonction permet de recuperer l'adresse d'une page */
/* a partir d'une adresse quelconque (dans la page)        */
static char *address2pgaddr( char *addr )
{
  return  (char *)(((intptr_t) addr) & ~(PAGE_SIZE-1)); 
}

/* fonctions pouvant etre utiles */
static void dsm_change_info( int numpage, dsm_page_state_t state, dsm_page_owner_t owner)
{
   if ((numpage >= 0) && (numpage < PAGE_NUMBER)) {	
	if (state != NO_CHANGE )
	table_page[numpage].status = state;
      if (owner >= 0 )
	table_page[numpage].owner = owner;
      return;
   }
   else {
	fprintf(stderr,"[%i] Invalid page number !\n", DSM_NODE_ID);
      return;
   }
}

static dsm_page_owner_t get_owner( int numpage)
{
   return table_page[numpage].owner;
}

static dsm_page_state_t get_status( int numpage)
{
   return table_page[numpage].status;
}

/* Allocation d'une nouvelle page */
static void dsm_alloc_page( int numpage )
{
   char *page_addr = num2address( numpage );
   mmap(page_addr, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   return ;
}

/* Changement de la protection d'une page */
static void dsm_protect_page( int numpage , int prot)
{
   char *page_addr = num2address( numpage );
   mprotect(page_addr, PAGE_SIZE, prot);
   return;
}

static void dsm_free_page( int numpage )
{
   char *page_addr = num2address( numpage );
   munmap(page_addr, PAGE_SIZE);
   return;
}

static void *dsm_comm_daemon( void *arg)
{  
   printf("[DAEMON-%d] Démarrage du démon de communication\n", DSM_NODE_ID);
   fflush(stdout);
   while(1)
     {

       for(int i = 0; i < DSM_NODE_NUM; i++) {
            
            
            struct pollfd pfd = {
                .fd = procs[i].fd,
                .events = POLLIN,
                .revents = 0
            };
            
            if(poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {

               dsm_req_t req;
               int recv_size = dsm_recv(procs[i].fd, &req, sizeof(dsm_req_t));

               if(req.type == DSM_PAGE){
                  if(get_owner(req.page_num) != DSM_NODE_ID) {
                     // Si on n'est pas le propriétaire, on ignore la requête
                     continue;
                  }
                  if(recv_size != sizeof(dsm_req_t)) {
                     printf("[DAEMON-%d] Réception incomplète ou erreur (reçu %d/%zu octets)\n", 
                              DSM_NODE_ID, recv_size, sizeof(dsm_req_t));
                     fflush(stdout);
                     continue;
                  }

                  // Afficher l'état après avoir reçu la requête
                  printf("[DAEMON-%d] État de la page %d : propriétaire=%d, état=%d\n",
                        DSM_NODE_ID, req.page_num, get_owner(req.page_num),
                        table_page[req.page_num].status);
                  fflush(stdout);
                  
                  printf("[DAEMON-%d] Requête reçue de %d pour la page %d\n", 
                        DSM_NODE_ID, req.source, req.page_num);
                  fflush(stdout);

                  if(get_owner(req.page_num) != DSM_NODE_ID) {
                     printf("[DAEMON-%d] Je ne suis pas propriétaire de la page %d\n", DSM_NODE_ID, req.page_num);
                     continue;
                  }

                  if(!(req.source >= 0 && req.source < DSM_NODE_NUM)){
                     printf("[DAEMON-%d] ERROR: Source invalide %d\n", DSM_NODE_ID, req.source);
                     continue;
                  }

                  char *page_addr = num2address(req.page_num);
                  printf("[DAEMON-%d] Envoi de la page %d à %d\n", DSM_NODE_ID, req.page_num, req.source);
                  printf("[DAEMON-%d] DEBUG: Envoi à proc %d via fd %d\n", DSM_NODE_ID, req.source, procs[req.source].fd);
                  fflush(stdout);

                  // Envoyer d'abord la confirmation
                  if(dsm_send(procs[req.source].fd, &req, sizeof(dsm_req_t)) != sizeof(dsm_req_t)){
                     printf("[DAEMON-%d] ERROR: Échec de l'envoi de la confirmation à proc %d\n", DSM_NODE_ID, req.source);
                     continue;
                  }
                  printf("[DAEMON-%d] DEBUG: Confirmation envoyée\n", DSM_NODE_ID);
                        
                  // Puis envoyer la page
                  if(dsm_send(procs[req.source].fd, page_addr, PAGE_SIZE) == PAGE_SIZE){
                        printf("[DAEMON-%d] DEBUG: Envoi réussi, mise à jour des infos\n", DSM_NODE_ID);
                        dsm_change_info(req.page_num, READ_ONLY, req.source);
                        dsm_protect_page(req.page_num, PROT_READ);
                        printf("[DAEMON-%d] Mise à jour propriétaire page %d : %d\n", 
                           DSM_NODE_ID, req.page_num, get_owner(req.page_num));
                        fflush(stdout);
                  } 
                  else{
                     printf("[DAEMON-%d] ERROR: Échec de l'envoi de la page à proc %d\n", DSM_NODE_ID, req.source);
                  }
               }
               else if(req.type == DSM_FINALIZE){
                  count_fini++;
                  printf("[%i] %i attendent la fermeture\n", DSM_NODE_ID, count_fini);
               }
            }
         }
	

   }
   return NULL;
}

static int dsm_send(int dest,void *buf,size_t size)
{
   size_t sent = 0;
   while (sent < size) {
      ssize_t ret = send(dest, (char*)buf + sent, size - sent, 0);
      if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return sent;
            return -1;
      }
      sent += ret;
   }
   return sent;
}

static int dsm_recv(int from,void *buf,size_t size)
{
   size_t received = 0;
   while (received < size) {
      ssize_t ret = recv(from, (char*)buf + received, size - received, 0);
      if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return received;
            return -1;
      }
      if (ret == 0) { // Connection closed
            return received;
      }
      received += ret;
   }
   return received;
}

static void dsm_handler( void* page_addr )
{  
   // 1. Identification de la page demandée et de son propriétaire
   int num_page = address2num(page_addr);
   int current_owner = get_owner(num_page);

   // Logs initiaux pour le diagnostic
   printf("[%i] FAULTY ACCESS : Tentative d'accès à une page non possédée\n", DSM_NODE_ID);
   printf("[%i] Page demandée : %d\n", DSM_NODE_ID, num_page);
   printf("[%i] Propriétaire actuel : %d\n", DSM_NODE_ID, current_owner);
   fflush(stdout);

   // 2. Préparation de la requête
   dsm_req_t request = {
      .type = DSM_PAGE,
      .source = DSM_NODE_ID,
      .page_num = num_page
   };

   // 3. Envoi de la requête au propriétaire actuel
   printf("[%i] Envoi de la requête au processus %d\n", DSM_NODE_ID, current_owner);
   fflush(stdout);

   if(dsm_send(procs[current_owner].fd, &request, sizeof(dsm_req_t)) != sizeof(dsm_req_t)) {
       printf("[%i] Échec de l'envoi de la requête\n", DSM_NODE_ID);
   fflush(stdout);

       return;
   }
   printf("[%i] Contenu de la requête envoyée : source=%d demande la page %d au processus %d\n", 
          DSM_NODE_ID, request.source, request.page_num, current_owner);
   fflush(stdout);
   
   // 4. Attente de la confirmation de réception
   dsm_req_t ack;
   printf("[%i] Attente de la confirmation de réception...\n", DSM_NODE_ID);
   if(dsm_recv(procs[current_owner].fd, &ack, sizeof(dsm_req_t)) != sizeof(dsm_req_t)) {
       printf("[%i] Pas de confirmation reçue\n", DSM_NODE_ID);
       fflush(stdout);

       return;
   }
   printf("[%i] Confirmation reçue du processus %d\n", DSM_NODE_ID, current_owner);
   fflush(stdout);     
   

   // 5. Allocation et préparation de la page
    char* page = num2address(num_page);
    printf("[%i] Allocation de la page %d à l'adresse %p\n", DSM_NODE_ID, num_page, page);
    fflush(stdout);
    dsm_alloc_page(num_page);
    dsm_protect_page(num_page, PROT_READ | PROT_WRITE);  // Protection temporaire pour permettre l'écriture

    
    // 6. Réception de la page
    printf("[%i] Réception de la page...\n", DSM_NODE_ID);
    fflush(stdout);
    int received = dsm_recv(procs[current_owner].fd, page, PAGE_SIZE);
    if(received != PAGE_SIZE) {
        printf("[%i] ERROR: Échec de réception de la page (%d/%ld octets)\n", 
               DSM_NODE_ID, received, PAGE_SIZE);
               fflush(stdout);
        dsm_free_page(num_page);
        return;
    }

    // 7. Mise à jour des informations
    printf("[%i] Page reçue, mise à jour des droits\n", DSM_NODE_ID);
    fflush(stdout);
    dsm_change_info(num_page, WRITE, DSM_NODE_ID);
    printf("[%i] Transfert terminé avec succès\n", DSM_NODE_ID);
    fflush(stdout);
}

/* traitant de signal adequat */
static void segv_handler(int sig, siginfo_t *info, void *context)
{
   /* A completer */
   /* adresse qui a provoque une erreur */
   void  *addr = info->si_addr;   
   printf("[SEGV] Signal reçu pour l'adresse : %p\n", addr);
   fflush(stdout);

  /* Si ceci ne fonctionne pas, utiliser a la place :*/
  /*
   #ifdef __x86_64__
   void *addr = (void *)(context->uc_mcontext.gregs[REG_CR2]);
   #elif __i386__
   void *addr = (void *)(context->uc_mcontext.cr2);
   #else
   void  addr = info->si_addr;
   #endif
   */
   /*
   pour plus tard (question ++):
   dsm_access_t access  = (((ucontext_t *)context)->uc_mcontext.gregs[REG_ERR] & 2) ? WRITE_ACCESS : READ_ACCESS;   
  */   
   

   /* adresse de la page dont fait partie l'adresse qui a provoque la faute */
   void  *page_addr  = (void *)(((unsigned long) addr) & ~(PAGE_SIZE-1));
   printf("[SEGV] Adresse de la page concernée : %p\n", page_addr);
    fflush(stdout);

   if ((addr >= (void *)BASE_ADDR) && (addr < (void *)TOP_ADDR))
     {
      printf("[SEGV] Adresse dans la plage DSM - Appel du gestionnaire\n");
      fflush(stdout);
      dsm_page_access_t access = (((ucontext_t *)context)->uc_mcontext.gregs[6] & 2) 
                                  ? WRITE_ACCESS 
                                  : READ_ACCESS;
        
        printf("[SEGV] Type d'accès : %s\n", 
               (access == WRITE_ACCESS) ? "écriture" : "lecture");
        fflush(stdout);
	dsm_handler(page_addr);
     }
   else
     {
	/* SIGSEGV normal : ne rien faire*/
      printf("[SEGV] Adresse hors DSM - SIGSEGV normal\n");
      fflush(stdout);

        struct sigaction act;
        act.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &act, NULL);
        kill(getpid(), SIGSEGV);
     }
}

/* Seules ces deux dernieres fonctions sont visibles et utilisables */
/* dans les programmes utilisateurs de la DSM                       */
char *dsm_init(int argc, char *argv[])
{   
   struct sigaction act;
   int index;
   int count_fini = 0;

   /* Récupération de la valeur des variables d'environnement */
   /* DSMEXEC_FD et MASTER_FD                                 */
   fprintf(stdout, "[DSM][DEBUG] Receiving data from wrap\n");
   DSMEXEC_FD = atoi(getenv("DSMEXEC_FD"));
   MASTER_FD = atoi(getenv("MASTER_FD"));

   if(!DSMEXEC_FD || !MASTER_FD){
      fprintf(stderr, "[DSM][ERROR] Failed to receive DSMEXEC_FD et/ou MASTER_FD\n");
      ERROR_EXIT("recv DSMEXEC_FD & MASTER_FD");
   }
   
   fprintf(stdout, "[DSM][DEBUG] Waiting for initialization data from launcher\n");
   /* reception du nombre de processus dsm envoye */
   /* par le lanceur de programmes (DSM_NODE_NUM) */
   if(dsm_recv(DSMEXEC_FD, &DSM_NODE_NUM, sizeof(int)) == -1) {
      fprintf(stderr, "[DSM][ERROR] Failed to receive num_procs\n");
      ERROR_EXIT("recv DSM_NODE_NUM");
   }
   fprintf(stdout, "[DSM][DEBUG] Received num_procs: %d\n", DSM_NODE_NUM);
   
   /* reception de mon numero de processus dsm envoye */
   /* par le lanceur de programmes (DSM_NODE_ID)      */
   if(dsm_recv(DSMEXEC_FD, &DSM_NODE_ID, sizeof(int)) == -1) {
      fprintf(stderr, "[DSM][ERROR] Failed to receive rank\n");
      ERROR_EXIT("recv DSM_NODE_ID");
   }
   fprintf(stdout, "[DSM][DEBUG] Received rank: %d\n", DSM_NODE_ID);
   

   /* reception des informations de connexion des autres */
   /* processus envoyees par le lanceur :                */
   /* nom de machine, numero de port, etc.               */
   
   //dsm_proc_conn_t procs[DSM_NODE_NUM];
   
   procs = malloc(DSM_NODE_NUM * sizeof(dsm_proc_conn_t));
   if (procs == NULL) {
      fprintf(stderr, "[%i] Erreur : Impossible d'allouer la mémoire pour procs\n", DSM_NODE_ID);
      exit(EXIT_FAILURE);
   }
   for(int j = 0; j<DSM_NODE_NUM; j++){
      if(dsm_recv(DSMEXEC_FD, &procs[j], sizeof(dsm_proc_conn_t)) == -1) {
         fprintf(stderr, "[DSM][ERROR] Failed to receive informations de connexion\n");
         ERROR_EXIT("recv informations de connexion");
      }
      fprintf(stdout, "[DSM][DEBUG] Received informations de connexion: rank: %d, machine: %s, nom de port: %d\n", procs[j].rank, procs[j].machine, procs[j].port_num);
   }
   
   /* initialisation des connexions              */ 
   /* avec les autres processus : connect/accept */

   // Mise en écoute initiale pour tous les processus sauf le dernier
   printf("[DEBUG][%d] Configuration initiale...\n", DSM_NODE_ID);
   if (DSM_NODE_ID < DSM_NODE_NUM - 1) {
      printf("[DEBUG][%d] Mise en écoute sur MASTER_FD=%d...\n", DSM_NODE_ID, MASTER_FD);
      if (listen(MASTER_FD, DSM_NODE_NUM) < 0) {
         printf("[ERROR][%d] Échec listen: %s\n", DSM_NODE_ID, strerror(errno));
         ERROR_EXIT("listen");
      }
   }

   // Phase 1: Établir les connexions vers les processus de rang inférieur
   for(int i = 0; i < DSM_NODE_ID; i++) {
      printf("[DEBUG][%d] Phase connect: connexion vers processus %d\n", DSM_NODE_ID, i);
      
      int max_retries = 7;
      int retry = 0;
      int connected = 0;
      
      while (retry < max_retries && !connected) {
         int sock = socket(AF_INET, SOCK_STREAM, 0);
         if(sock < 0) {
               printf("[ERROR][%d] Échec création socket pour processus %d: %s\n",
                     DSM_NODE_ID, i, strerror(errno));
               ERROR_EXIT("socket");
         }

         struct sockaddr_in server_addr;
         memset(&server_addr, 0, sizeof(server_addr));
         server_addr.sin_family = AF_INET;
         server_addr.sin_port = htons(procs[i].port_num);
         
         if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
               printf("[ERROR][%d] Erreur conversion IP pour processus %d\n", DSM_NODE_ID, i);
               close(sock);
               retry++;
               sleep(1);
               continue;
         }

         printf("[DEBUG][%d] Tentative %d de connexion à processus %d (port %d)\n",
                  DSM_NODE_ID, retry + 1, i, procs[i].port_num);

         if(connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
               printf("[DEBUG][%d] Échec tentative %d vers processus %d: %s\n",
                     DSM_NODE_ID, retry + 1, i, strerror(errno));
               close(sock);
               retry++;
               sleep(1);
               continue;
         }
         
         procs[i].fd = sock;
         connected = 1;
         printf("[DEBUG][%d] Connexion établie avec processus %d (FD=%d)\n",
                  DSM_NODE_ID, i, sock);
      }

      if (!connected) {
         printf("[ERROR][%d] Impossible de se connecter au processus %d après %d tentatives\n",
                  DSM_NODE_ID, i, max_retries);
         ERROR_EXIT("connect-failed");
      }
   }

   // Phase 2: Accepter les connexions des processus de rang supérieur
   int expected_connections = DSM_NODE_NUM - (DSM_NODE_ID + 1);
   printf("[DEBUG][%d] En attente de %d connexions entrantes\n", DSM_NODE_ID, expected_connections);

   for(int i = DSM_NODE_ID + 1; i < DSM_NODE_NUM; i++) {
      printf("[DEBUG][%d] Attente connexion du processus %d\n", DSM_NODE_ID, i);
      
      struct sockaddr_in client_addr;
      socklen_t len = sizeof(client_addr);
      
      while(1) {
         int sock = accept(MASTER_FD, (struct sockaddr*)&client_addr, &len);
         if(sock < 0) {
               if(errno == EINTR) continue; // Réessayer si interrompu
               printf("[ERROR][%d] Échec accept pour processus %d: %s\n",
                     DSM_NODE_ID, i, strerror(errno));
               ERROR_EXIT("accept");
         }
         
         procs[i].fd = sock;
         printf("[DEBUG][%d] Accepté connexion du processus %d (FD=%d)\n",
                  DSM_NODE_ID, i, sock);
         break;
      }
   }

   printf("[DEBUG][%d] Toutes les connexions établies avec succès\n", DSM_NODE_ID);


   /* Allocation des pages en tourniquet */
   for(index = 0; index < PAGE_NUMBER; index ++){	
     if ((index % DSM_NODE_NUM) == DSM_NODE_ID)
       dsm_alloc_page(index);	     
     dsm_change_info( index, WRITE, index % DSM_NODE_NUM);
   }
   
   /* mise en place du traitant de SIGSEGV */
   act.sa_flags = SA_SIGINFO; 
   act.sa_sigaction = segv_handler;
   sigaction(SIGSEGV, &act, NULL);
   
   /* creation du thread de communication           */
   /* ce thread va attendre et traiter les requetes */
   /* des autres processus                          */
   pthread_create(&comm_daemon, NULL, dsm_comm_daemon, NULL);
   
   /* Adresse de début de la zone de mémoire partagée */
   return ((char *)BASE_ADDR);
}

void dsm_finalize(void) {
   
   // previenir les autres processus que celui-ci a fini
   dsm_req_t request = {
      .type = DSM_FINALIZE,
      .source = DSM_NODE_ID
   };

   for(int i = 0; i < DSM_NODE_NUM-1; i++){
      dsm_send(procs[i].fd, &request, sizeof(dsm_req_t));
   }
   count_fini++;

   /* On s'assure que tous les processus sont arrivés avant de continuer */
   printf("[%i] Attente pour fermeture\n", DSM_NODE_ID);
   fflush(stdout);
   while(count_fini<DSM_NODE_NUM){
      usleep(1000); // Petite pause pour éviter de surcharger le CPU
   }

    /* Une fois tous synchronisés, on peut fermer les connexions */
    for(int i = 0; i < DSM_NODE_NUM; i++) {
        if(i != DSM_NODE_ID) {
            printf("[DSM] Fermeture de la connexion avec le processus %d\n", i);
            if(procs[i].fd != -1) {
                close(procs[i].fd);
            }
        }
    }

    /* Arrêt propre du thread de communication */
    printf("[DSM] Arrêt du thread de communication\n");
    pthread_cancel(comm_daemon);
    pthread_join(comm_daemon, NULL);
    printf("[DSM] Début de la finalisation du processus %d\n", DSM_NODE_ID);
    /* Libération des ressources mémoire */
    for(int i = 0; i < PAGE_NUMBER; i++) {
        if(get_owner(i) == DSM_NODE_ID) {
            printf("[DSM] Libération de la page %d\n", i);
            dsm_free_page(i);
        }
    }

    printf("[DSM] Finalisation terminée pour le processus %d\n", DSM_NODE_ID);
}

