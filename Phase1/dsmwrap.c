#include "common_impl.h"
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef MAX_CONNECT
#define MAX_CONNECT 10
#endif

int handle_connect(const char *server_addr, const char *server_port) {
    fprintf(stdout, "[DSMWRAP][DEBUG] Attempting to connect to %s:%s\n", server_addr, server_port);
    
    struct addrinfo hints, *result, *rp;
    int sfd;
    
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    
    int ret = getaddrinfo(server_addr, server_port, &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "[DSMWRAP][ERROR] getaddrinfo: %s\n", gai_strerror(ret));
        fflush(stdout);
        return -1;
    }
    
    /* Try each address until we successfully connect */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            fprintf(stderr, "[DSMWRAP][DEBUG] Socket creation failed, trying next address\n");
            continue;
        }
        
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            fprintf(stdout, "[DSMWRAP][DEBUG] Successfully connected to launcher\n");
            break;  /* Success */
        }
        
        fprintf(stderr, "[DSMWRAP][DEBUG] Connect failed, trying next address\n");
        close(sfd);
    }
    
    freeaddrinfo(result);
    
    if (rp == NULL) {
        fprintf(stderr, "[DSMWRAP][ERROR] Could not connect to launcher\n");
        fflush(stdout);
        return -1;
    }
    
    return sfd;
}


int main(int argc, char *argv[]) {   
    fprintf(stdout, "[DSMWRAP][DEBUG] Starting dsmwrap\n");
    /* Vérification des arguments */
    if (argc < 3) {
        fprintf(stderr, "[DSMWRAP][ERROR] Usage: %s addr port [prog args...]\n", argv[0]);
        fflush(stdout);
        exit(EXIT_FAILURE);
    }

    int nb_args = argc - 3;  // nombre d'arguments après dsmwrap addr_ip port_str

    /* processus intermediaire pour "nettoyer" */
    /* la liste des arguments qu'on va passer */
    /* a la commande a executer finalement  */
    
    /* Récupération des arguments pour la connexion */
    const char *server_addr = argv[1];
    const char *server_port = argv[2];
    fprintf(stdout, "[DSMWRAP][DEBUG] Server address: %s, port: %s\n", server_addr, server_port);
    
    /* creation d'une socket pour se connecter au */
    /* au lanceur et envoyer/recevoir les infos */
    /* necessaires pour la phase dsm_init */
    int sfd = handle_connect(server_addr, server_port);
    if(sfd == -1) {
        ERROR_EXIT("handle_connect");
    }

    /* Récupération du nom de la machine locale */
    maxstr_t machine_name;
    if(gethostname(machine_name, MAX_STR) < 0) {
        fprintf(stderr, "[DSMWRAP][ERROR] Failed to get hostname\n");
        fflush(stdout);
        ERROR_EXIT("gethostname");
    }
    fprintf(stdout, "[DSMWRAP][DEBUG] Local hostname: %s\n", machine_name);

    /* Envoi du nom de machine au lanceur */
    if (dsm_send_all(sfd, machine_name, MAX_STR, 0) == -1) {
        fprintf(stderr, "[DSMWRAP][ERROR] Failed to send hostname\n");
        fflush(stdout);
        ERROR_EXIT("send hostname");
    }
    fprintf(stdout, "[DSMWRAP][DEBUG] Sent hostname to launcher\n");

    /* Envoi du pid au lanceur */
    pid_t pid = getpid();
    fprintf(stdout, "[DSMWRAP][DEBUG] Local PID: %d\n", pid);
    
    if (dsm_send_all(sfd, &pid, sizeof(pid_t), 0) == -1) {
        fprintf(stderr, "[DSMWRAP][ERROR] Failed to send PID\n");
        fflush(stdout);
        ERROR_EXIT("send pid");
    }
    fprintf(stdout, "[DSMWRAP][DEBUG] Sent PID to launcher\n");


    /* Création socket d'écoute pour les autres processus DSM */
    fprintf(stdout, "[DSMWRAP][DEBUG] Creating listening socket for other DSM processes\n");

    int dsm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (dsm_sock == -1) ERROR_EXIT("socket");
    struct sockaddr_in dsm_addr;
    memset(&dsm_addr, 0, sizeof(dsm_addr));
    dsm_addr.sin_family = AF_INET;
    dsm_addr.sin_addr.s_addr = INADDR_ANY;
    dsm_addr.sin_port = 0;  // Port automatique

    if (bind(dsm_sock, (struct sockaddr *)&dsm_addr, sizeof(dsm_addr)) == -1) {
        fprintf(stderr, "[DSMWRAP][ERROR] Failed to bind DSM socket\n");
        fflush(stdout);
        ERROR_EXIT("bind");
    }
    
    socklen_t len = sizeof(dsm_addr);
    if (getsockname(dsm_sock, (struct sockaddr *)&dsm_addr, &len) == -1) {
        fprintf(stderr, "[DSMWRAP][ERROR] Failed to get socket name\n");
        fflush(stdout);
        ERROR_EXIT("getsockname");
    }

    if (listen(dsm_sock, MAX_CONNECT) == -1) {
        fprintf(stderr, "[DSMWRAP][ERROR] Failed to listen on DSM socket\n");
        fflush(stdout);
        ERROR_EXIT("listen");
    }

    /* Envoi du numéro de port au lanceur */
    int port = ntohs(dsm_addr.sin_port);
    fprintf(stdout, "[DSMWRAP][DEBUG] Local DSM port: %d\n", port);
    
    
    if (dsm_send_all(sfd, &port, sizeof(int), 0) == -1) {
        fprintf(stderr, "[DSMWRAP][ERROR] Failed to send port\n");
        fflush(stdout);
        ERROR_EXIT("send port");
    }
    fprintf(stdout, "[DSMWRAP][DEBUG] Sent port to launcher\n");
    
    /* on execute la bonne commande */
    /* attention au chemin à utiliser ! */
    char sfd_str[12];
    char dsm_sock_str[12];

    snprintf(sfd_str, sizeof(sfd_str), "%d", sfd);
    snprintf(dsm_sock_str, sizeof(dsm_sock_str), "%d", dsm_sock);

    setenv("DSMEXEC_FD", sfd_str, 1);
    setenv("MASTER_FD", dsm_sock_str, 1);
    fprintf(stdout, "[DSMWRAP][DEBUG] Sent DSMEXEC_FD & MASTER_FD\n");
    fflush(stdout);

    char *newargv[1 + nb_args];
    for(int i = 0; i<nb_args; i++){
        newargv[i] = argv[3+i];
    }
    newargv[nb_args] = NULL;

    if(execvp(argv[3], newargv) == -1){
        perror("execvp");
        exit(EXIT_FAILURE);
    }

    /************** ATTENTION **************/
    /* vous remarquerez que ce n'est pas   */
    /* ce processus qui récupère son rang, */
    /* ni le nombre de processus           */
    /* ni les informations de connexion    */
    /* (cf protocole dans dsmexec)         */
    /***************************************/
    
    return 0;  
}
