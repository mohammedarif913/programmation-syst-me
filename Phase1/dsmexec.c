#include "common_impl.h"
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
#include <fcntl.h>
#include <errno.h>

/* variables globales */

/* un tableau gerant les infos d'identification */
/* des processus dsm */
dsm_proc_t *proc_array = NULL; 

/* le nombre de processus effectivement crees */
volatile int num_procs_creat = 0;

void usage(void)
{
  fprintf(stdout,"Usage : dsmexec machine_file executable arg1 arg2 ...\n");
  fflush(stdout);
  exit(EXIT_FAILURE);
}

void sigchld_handler(int sig)
{
   /* on traite les fils qui se terminent */
   /* pour eviter les zombies */
   int status;
   pid_t pid;
   while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      if (WIFEXITED(status)) {
         fprintf(stderr, "[DEBUG][SIGCHLD] Child %d terminated with status %d\n", pid, WEXITSTATUS(status));
         if (WEXITSTATUS(status) == 127) {
            fprintf(stderr, "[ERROR][SIGCHLD] Command not found. Check PATH and DSM_BIN\n");
            fprintf(stderr, "[DEBUG][SIGCHLD] PATH=%s\n", getenv("PATH"));
            fprintf(stderr, "[DEBUG][SIGCHLD] DSM_BIN=%s\n", getenv("DSM_BIN"));
         }
      }
   }
}

void recup_local_addr(char *addr_ip){
   struct ifaddrs *ifaddr, *ifa;
   int family;

   // recupere la liste des interfaces réseaux
   if(getifaddrs(&ifaddr) == -1){
      perror("getifaddrs");
      exit(EXIT_FAILURE);
   }

   // parcourt les interfaces pour trouver une adresse (autre que loopback)
   for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next){
      if(ifa->ifa_addr==NULL){ // ignore les interfaces sans adresse
         continue;
      }

      family = ifa->ifa_addr->sa_family;
      if(family == AF_INET){
         struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
         const char* ip = inet_ntoa(addr->sin_addr);
         if(strcmp(ip, "127.0.0.1")!=0){ // pas de loopback
            strncpy(addr_ip, ip, INET_ADDRSTRLEN);
            fprintf(stderr, "[DEBUG][NET] Found IP: %s\n", addr_ip);
            break;
         }
      }
   }

   freeifaddrs(ifaddr);
}

/*******************************************************/
/*********** ATTENTION : BIEN LIRE LA STRUCTURE DU *****/
/*********** MAIN AFIN DE NE PAS AVOIR A REFAIRE *******/
/*********** PLUS TARD LE MEME TRAVAIL DEUX FOIS *******/
/*******************************************************/

int main(int argc, char *argv[])
{
  if (argc < 3){
    usage();
  } else {       
      pid_t pid;
      int i;
      int nb_args = argc-3; // nb d'arguments pour le programme a executé
      int sock_fd;

      signal(SIGPIPE, SIG_IGN); //masquer le signal SIGPIPE
      
      /* Mise en place d'un traitant pour recuperer les fils zombies*/      
      /* XXX.sa_handler = sigchld_handler; */
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_handler = sigchld_handler;
      if (sigaction(SIGCHLD, &sa, NULL) == -1) {
         ERROR_EXIT("sigaction");
      }
      
      /* lecture du fichier de machines */
      /* 1- on recupere le nombre de processus a lancer */
      FILE* machine_file = fopen(argv[1], "r");
      if(machine_file == NULL){
         fprintf(stderr, "[ERROR] Could not open machine_file\n");
         return EXIT_FAILURE;
      }
      maxstr_t buff;
      while(fgets(buff, sizeof(buff), machine_file)!=NULL){
         if(strlen(buff)>1){
            num_procs_creat++;
         }
      }
      fprintf(stderr, "[DEBUG] Found %d machines\n", num_procs_creat);

      /* 2- on recupere les noms des machines : le nom de */
      /* la machine est un des elements d'identification */
      rewind(machine_file);
      char list_machines[num_procs_creat][MAX_STR];
      for(i = 0; i<num_procs_creat; i++){
         fgets(buff, sizeof(buff), machine_file);
         if(strlen(buff)>1){
            buff[strcspn(buff, "\n")] = '\0';
            strncpy(list_machines[i], buff, MAX_STR);
            fprintf(stderr, "[DEBUG] Machine %d: %s\n", i, list_machines[i]);
         }
         else{
            i--;
         }
      }
      fclose(machine_file);
     
      /* creation de la socket d'ecoute */
      /* + ecoute effective */ 
      sock_fd = socket(AF_INET, SOCK_STREAM, 0);
      if (sock_fd == -1) ERROR_EXIT("socket");

      /* Preparation de l'adresse de la socket */
      struct sockaddr_in sock_addr;
      memset(&sock_addr, 0, sizeof(sock_addr));
      sock_addr.sin_family = AF_INET;
      sock_addr.sin_addr.s_addr = INADDR_ANY;  /* Accept any incoming messages */
      sock_addr.sin_port = 0;                  /* Port choisi automatiquement */ 

      /* Bind de la socket */
      if (bind(sock_fd, (struct sockaddr *)&sock_addr, sizeof(sock_addr)) == -1) 
         ERROR_EXIT("bind");

      /* Recuperation du port assigne */
      socklen_t len = sizeof(sock_addr);
      if (getsockname(sock_fd, (struct sockaddr *)&sock_addr, &len) == -1)
         ERROR_EXIT("getsockname");

      /* Mise en ecoute */
      if (listen(sock_fd, num_procs_creat) == -1) 
         ERROR_EXIT("listen");

      char port_str[6];
      snprintf(port_str, sizeof(port_str), "%d", ntohs(sock_addr.sin_port));
      char addr_ip[INET_ADDRSTRLEN];
      recup_local_addr(addr_ip);
     
      /* Tableau des processus */
      dsm_proc_t proc_array[num_procs_creat];

      /* Tableau pour poll */
      struct pollfd poll_fds[2*num_procs_creat];
     
      /* creation des fils */
      for(i = 0; i < num_procs_creat ; i++) {
         int fd_stdout[2];
         int fd_stderr[2];

         /* creation du tube pour rediriger stdout */
         if (pipe(fd_stdout) == -1) ERROR_EXIT("pipe");
         /* creation du tube pour rediriger stderr */
         if (pipe(fd_stderr) == -1) ERROR_EXIT("pipe");

         pid = fork();
         if(pid == -1) ERROR_EXIT("fork");
      
         if (pid == 0) { /* fils */
            /* redirection stdout */	      
            if (close(fd_stdout[0]) == -1) ERROR_EXIT("close"); // Ferme l'extrémité lecture
            if (dup2(fd_stdout[1], STDOUT_FILENO) == -1) ERROR_EXIT("dup2"); // Redirige stdout
            /* redirection stderr */	      	      
            if (close(fd_stderr[0]) == -1) ERROR_EXIT("close"); // Ferme l'extrémité lecture
            if (dup2(fd_stderr[1], STDERR_FILENO) == -1) ERROR_EXIT("dup2"); // Redirige stderr
           
            /* Creation du tableau d'arguments pour le ssh */
            char *newargv[7 + nb_args];
            newargv[0] = "ssh";
            newargv[1] = list_machines[i];
            newargv[2] = "dsmwrap";
            newargv[3] = addr_ip;
            newargv[4] = port_str;
            newargv[5] = argv[2];
            for(i = 0; i<nb_args; i++){
               newargv[6+i] = argv[3+i];
            }
            newargv[6 + nb_args] = NULL;

           /* jump to new prog : */
           /* execvp("ssh",newargv); */
           execvp("ssh", newargv);
            
           if(execvp("ssh", newargv) == -1){
               printf("[ERROR][CHILD-%d] execvp failed: \n", i);
               exit(EXIT_FAILURE);
           }

         } else  if(pid > 0) { /* pere */		      
            /* fermeture des extremites des tubes non utiles */
            if (close(fd_stdout[1]) == -1) ERROR_EXIT("close stdout write");
            if (close(fd_stderr[1]) == -1) ERROR_EXIT("close stderr write");
           
            proc_array[i].pid = pid;
            proc_array[i].connect_info.rank = i;
            poll_fds[2*i].fd = fd_stdout[0];
            poll_fds[2*i].events = POLLIN;
            poll_fds[2*i].revents = 0;
            poll_fds[2*i+1].fd = fd_stderr[0];
            poll_fds[2*i+1].events = POLLIN;
            poll_fds[2*i+1].revents = 0;
            fcntl(poll_fds[2*i].fd,F_SETFL,O_NONBLOCK);
            fcntl(poll_fds[2*i+1].fd,F_SETFL,O_NONBLOCK);
            
            fprintf(stdout, "[DEBUG] Created process %d with pid %d\n", i, pid);
         }
      }
     
      /* Accept connections */
      int connfd;

      for(i = 0; i < num_procs_creat; i++){
         struct sockaddr_in cli_addr;
         socklen_t len = sizeof(cli_addr);

         fprintf(stdout, "[DEBUG] Waiting for connection %d/%d\n", i+1, num_procs_creat);
         /* on accepte les connexions des processus dsm */
         while(1){
            connfd = accept(sock_fd, (struct sockaddr*)&cli_addr, &len);
            if(connfd >= 0){
               break;
            }
            if(errno == EINTR){
               fprintf(stdout, "[DEBUG] Accept interrupted, retrying\n");
               continue;
            }
            ERROR_EXIT("accept");
         }

         /* On recupere le nom de la machine distante */
         /* les chaines ont une taille de MAX_STR */
         maxstr_t machine_name;
         pid_t remote_pid;
         int remote_port;

         if (dsm_recv_all(connfd, machine_name, MAX_STR, 0) != MAX_STR) {
            ERROR_EXIT("recv machine name");
         }

         /* On recupere le pid du processus distant */
         if (dsm_recv_all(connfd, &remote_pid, sizeof(pid_t), 0) != sizeof(pid_t)) {
            ERROR_EXIT("recv pid");
         }
         
         /* On recupere le numero de port de la socket */
         /* d'ecoute des processus distants */
         if (dsm_recv_all(connfd, &remote_port, sizeof(int), 0) != sizeof(int)) {
            ERROR_EXIT("recv port");
         }

         fprintf(stdout, "[DEBUG] Connected process %d: %s pid=%d port=%d\n",i, machine_name, remote_pid, remote_port);

         strncpy(proc_array[i].connect_info.machine, machine_name, MAX_STR);
         proc_array[i].connect_info.port_num = remote_port;
         proc_array[i].connect_info.fd = connfd;
      }

     /***********************************************************/ 
     /********** ATTENTION : LE PROTOCOLE D'ECHANGE *************/
     /********** DECRIT CI-DESSOUS NE DOIT PAS ETRE *************/
     /********** MODIFIE, NI DEPLACE DANS LE CODE   *************/
     /***********************************************************/
     
      /* 1- envoi du nombre de processus aux processus dsm*/
      /* On envoie cette information sous la forme d'un ENTIER */
      /* (IE PAS UNE CHAINE DE CARACTERES */
      for(i = 0; i < num_procs_creat; i++) {
         if (dsm_send_all(proc_array[i].connect_info.fd, (void*)&num_procs_creat, sizeof(int), 0) == -1)
            ERROR_EXIT("send num_procs");

         /* 2- envoi des rangs aux processus dsm */
         /* chaque processus distant ne reçoit QUE SON numéro de rang */
         /* On envoie cette information sous la forme d'un ENTIER */
         /* (IE PAS UNE CHAINE DE CARACTERES */
         if (dsm_send_all(proc_array[i].connect_info.fd, &i, sizeof(int), 0) == -1)
            ERROR_EXIT("send rank");

         /* 3- envoi des infos de connexion aux processus */
         /* Chaque processus distant doit recevoir un nombre de */
         /* structures de type dsm_proc_conn_t égal au nombre TOTAL de */
         /* processus distants, ce qui signifie qu'un processus */
         /* distant recevra ses propres infos de connexion */
         /* (qu'il n'utilisera pas, nous sommes bien d'accords). */
         for(int j = 0; j<num_procs_creat; j++){
            if (dsm_send_all(proc_array[i].connect_info.fd, &proc_array[j].connect_info,
                  sizeof(dsm_proc_conn_t), 0) == -1)
            ERROR_EXIT("send proc_array");
         }
         
            
         fprintf(stdout, "[DEBUG] Sent init data to process %d\n", i);
      }
     /***********************************************************/
     /********** FIN DU PROTOCOLE D'ECHANGE DES DONNEES *********/
     /********** ENTRE DSMEXEC ET LES PROCESSUS DISTANTS ********/
     /***********************************************************/
     
     /* gestion des E/S : on recupere les caracteres */
     /* sur les tubes de redirection de stdout/stderr */ 
      fprintf(stdout, "[DEBUG] Entering main I/O loop\n");
      int open_fds = 2*num_procs_creat;
      while(open_fds>0) {
         int ret = poll(poll_fds, 2*num_procs_creat, -1);
         fprintf(stdout, "\n[DEBUG] ret: %d, open_fds: %d\n", ret, open_fds);
         if(ret == -1) {
            if(errno == EINTR) continue;
            ERROR_EXIT("poll");
         }
         for(i = 0; i < 2*num_procs_creat; i++) {
            if(poll_fds[i].revents & POLLIN) {

               ssize_t count;
               char character;

               fprintf(stdout, "\n[Proc %d : %s : %s]\n", 
                       i/2,
                       proc_array[i/2].connect_info.machine,
                       (i % 2 == 0) ? "stdout" : "stderr");

               do {
                  count = read(poll_fds[i].fd, &character, 1);
                  if(!((-1 == count) && (errno == EAGAIN)) && count!=0)
                     printf("%c", character);
               } while(count>0);

               if(count == -1 && errno != EAGAIN){
                  ERROR_EXIT("read");
               } // revoir

               if(count == -1) {
                  continue;
               }

               fflush(stdout);
               
               if(count == 0) {
               	fprintf(stdout, "[DEBUG] Process %d closed %d (EOF)\n", i/2, i%2);
                  open_fds--;
                  poll_fds[i].fd = -1;
                  continue;
               }
            }
            if(poll_fds[i].revents & POLLHUP){
               fprintf(stdout, "[DEBUG] Process %d closed %d (POLLHUP)\n", i/2, i%2);
               open_fds--;
               poll_fds[i].fd = -1;
            }
         }
      }

      /* on attend les processus fils */
      int status;
      for(int i=0; i<num_procs_creat; i++){
         wait(&status);
      }
     
      /* on ferme les descripteurs proprement */
      for(i = 0; i < num_procs_creat; i++) {
         close(proc_array[i].connect_info.fd);
         close(poll_fds[2*i].fd);
         close(poll_fds[2*i+1].fd);
      }
     
      /* on ferme la socket d'ecoute */
      close(sock_fd);
      printf("Socket fermée\n");
  }   
  exit(EXIT_SUCCESS);  
}
              
