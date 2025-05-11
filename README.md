# DistMemShare - Système de Mémoire Partagée Distribuée

## Vue d'ensemble

DistMemShare est une implémentation d'un système de mémoire partagée distribuée (DSM) qui permet à des processus s'exécutant sur différentes machines de partager un espace d'adressage commun. Le projet est divisé en deux phases principales:

1. **Phase 1**: Infrastructure de communication entre processus
2. **Phase 2**: Implémentation du mécanisme de DSM et gestion de la cohérence des pages

## Phase 1: Infrastructure de communication

La première phase établit l'infrastructure nécessaire pour lancer et coordonner les processus sur différentes machines.

### Composants

- **dsmexec.c**: Le programme lanceur qui démarre les processus DSM sur les machines distantes.
  - Lit un fichier de machines pour déployer les processus
  - Redirige les sorties standard et d'erreur des processus distants
  - Établit les connexions entre les processus
  - Distribue les informations nécessaires aux processus

- **dsmwrap.c**: Le programme intermédiaire qui s'exécute sur chaque machine distante.
  - Prépare l'environnement pour les processus utilisateurs
  - Établit la connexion avec le lanceur
  - Transmet les informations au processus local

- **common.c**: Fonctions utilitaires communes.
  - Fonctions pour l'envoi et la réception fiables de données
  - Structures de données partagées

### Fonctionnalités

- Déploiement automatique sur plusieurs machines via SSH
- Redirection des sorties standards et d'erreur des processus distants
- Communication fiable entre les processus
- Établissement des connexions entre tous les processus

## Phase 2: Mécanisme de DSM

La deuxième phase implémente le mécanisme de mémoire partagée distribuée proprement dit.

### Composants

- **dsm.c**: Implémentation des fonctions principales de DSM.
  - Initialisation du système DSM
  - Gestion des défauts de page
  - Traitement des requêtes de page
  - Finalisation du système

- **dsm.h** / **dsm_impl.h**: Déclarations et structures de données pour le mécanisme DSM.

### Fonctionnalités

- Abstraction d'un espace d'adressage partagé entre processus distants
- Allocation des pages mémoire en tourniquet entre les processus
- Traitement des fautes de segmentation pour accès aux pages distantes
- Transfert automatique des pages entre processus selon les besoins
- Protocole de cohérence simple pour la gestion des pages
- Synchronisation entre processus pour la terminaison

## Mécanisme d'accès DSM

1. Quand un processus tente d'accéder à une page dont il n'est pas propriétaire, une faute de segmentation se produit
2. Le gestionnaire de signal intercepte cette faute et identifie la page demandée
3. Une requête est envoyée au propriétaire actuel de la page
4. Le propriétaire envoie la page au demandeur et met à jour ses droits d'accès
5. Le demandeur reçoit la page, met à jour ses informations et peut désormais y accéder

## Utilisation

### Prérequis

- Système Linux (testé sur Ubuntu/Debian)
- Compilateur GCC
- Bibliothèques standard C (pthread, etc.)
- SSH configuré pour l'accès sans mot de passe entre les machines

### Compilation

```bash
make
```

### Exécution

1. Créez un fichier contenant la liste des machines (une par ligne)
2. Lancez votre application avec le lanceur:

```bash
./bin/dsmexec machines_file ./bin/mon_programme arg1 arg2 ...
```

### Exemple d'application

Un exemple simple est fourni dans `exemple.c`. Ce programme:
- Initialise le système DSM
- Effectue quelques accès mémoire
- Finalise le système DSM

## Structure du code

```
.
├── Makefile
├── bin/                # Binaires compilés
├── common.c            # Fonctions communes
├── common_impl.h       # Structures et déclarations communes
├── dsm.c               # Implémentation du mécanisme DSM
├── dsm.h               # Interface publique DSM
├── dsm_impl.h          # Implémentation interne DSM
├── dsmexec.c           # Lanceur de programmes
├── dsmwrap.c           # Processus intermédiaire
├── exemple.c           # Programme d'exemple
└── truc.c              # Programme de test minimal
```

## Notes techniques

- La taille de page est définie par la valeur système (généralement 4KB)
- L'espace d'adressage partagé commence à l'adresse `0x40000000`
- Le système supporte jusqu'à 100 pages partagées par défaut
- Un protocole de cohérence simple est implémenté (propriétaire unique par page)

