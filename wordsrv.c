#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 55879
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);
int broadcast(struct game_state *game, char *outbuf);
void disconnection(struct game_state *game, struct client **top, struct client *p);
int announce_turn(struct game_state *game);
int announce_winner(struct game_state *game);
void advance_turn(struct game_state *game);
int read_buf(struct client *p);
/* These are some of the function prototypes that we used in our solution
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */

/* Send the message in outbuf to all clients */
int broadcast(struct game_state *game, char *outbuf){
    struct client *p;
    for (p = game->head; p!=NULL; p = p->next){
        if (write(p->fd, outbuf, strlen(outbuf)) == -1){
            disconnection(game, &(game->head), p);
            return 1;
        }
    }
    return 0;
}

 int announce_turn(struct game_state *game){
    struct client *p;
    char msg[MAX_MSG];
    if (game->has_next_turn != NULL){
        struct client *cur = game->has_next_turn;
        strcpy(msg, "It is ");
        strcat(msg, cur->name);
        strcat(msg, "'s turn.\n");
        printf("%s", msg);
    }
    for (p = game->head; p != NULL; p = p->next){
        if (p != game->has_next_turn){
            if (write(p->fd, msg, strlen(msg)) == -1){
              disconnection(game, &(game->head), p);
              return 1;
            }
        } else {
          if (write(p->fd, "Your guess?\n", 12) == -1){
            disconnection(game, &(game->head), p);
            return 1;
          }
        }
    }
    return 0;
}

int announce_winner(struct game_state *game){
    struct client *p;
    for (p = game->head; p!= NULL; p=p->next){
        if (p == game->has_next_turn){
            if (write(p->fd, "Game Over! You win!\n", 20) == -1){
                disconnection(game, &(game->head), p);
                return 1;
            }
        } else {
            char outbuf[MAX_MSG];
            sprintf(outbuf, "Game Over! %s win!\n", (game->has_next_turn)->name);
            if (write(p->fd, outbuf, sizeof(outbuf)) == -1){
                disconnection(game, &(game->head), p);
                return 1;
            }
            printf("%s", outbuf);
        }
    }
    return 0;
}

/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game){
    if ((game->has_next_turn)->next != NULL){
        game->has_next_turn = (game->has_next_turn)->next;
    } else{
        game->has_next_turn = game->head;
    }
}


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
}

 /* Deal with the situation when a client disconnects
  */
 void disconnection(struct game_state *game, struct client **top, struct client *p){
    if (p == game->has_next_turn){
        advance_turn(game);
        announce_turn(game);
    }
    remove_player(top, p->fd);
 }

/* Helper function of read_buf, copied from lab10.
 */
int find_network_newline(const char *buf, int n) {
    for (int i = 0; i < n; i++){
        if (buf[i] == '\r'){
            if (buf[i+1] == '\n'){
                return i+2;
            }
        }
    }
    return -1;
}

/* Read the contends in socket descriptor into the client's fiele inbuf
 */
int read_buf(struct client *p){
    char buf[MAX_BUF] = {'\0'};
    int inbuf = 0;
    int room = sizeof(buf);
    char *after = buf;

    int nbytes;
    while ((nbytes = read(p->fd, after, room)) > 0) {
      inbuf += nbytes;
      int where;
      if ((where = find_network_newline(buf, inbuf)) > 0){
          printf("[%d] Read %d bytes\n", p->fd, nbytes);
          buf[where-2] = '\0';
          printf("[%d] Find new line %s\n", p->fd, buf);
          strcpy(p->inbuf, buf);
          return 1;
      }
      after = buf + inbuf;
      room = sizeof(buf) - inbuf;
    }
    return 0;
}


int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;

    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }

    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);

    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;

    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;

    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);

    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&(game.head), p->fd);
            };
        }

        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be
         * valid.
         */
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player

                for(p = game.head; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        //TODO - handle input from an active client
                        if (read_buf(p) == 0){
                          disconnection(&game, &(game.head), p);
                          break;
                        }
                        if (strlen(p->inbuf) != 0 && p != game.has_next_turn){
                            printf("Player %s tried to guess out of turn\n", p->name);
                            if (write(p->fd, "It is not your turn to guess\n", 29) == -1){
                              disconnection(&game, &(game.head), p);
                              break;
                            }
                        }
                        if (game.has_next_turn == p){
                          // Find the index of the letter
                          for (int i=0; i<26; i++){
                            if ((char)('a' + i) == p->inbuf[0]){
                              if (game.letters_guessed[i] == 1 || strlen(p->inbuf) != 1){
                                  if (write(p->fd, "Invalid guess\n", 14) == -1){
                                    disconnection(&game, &(game.head), p);
                                    break;
                                  }
                                  if (write(p->fd, "Your guess?\n", 12) == -1){
                                    disconnection(&game, &(game.head), p);
                                    break;
                                  }
                              } else {
                                game.letters_guessed[i] = 1;
                                int length = sizeof(game.word);
                                //Chenk if the letter is in the word
                                int exist = 0;
                                for (int i=0;i<length;i++){
                                    if (game.word[i] == p->inbuf[0]){
                                        game.guess[i] = game.word[i];
                                        exist++;
                                    }
                                }
                                // If it's not in the word.
                                if (exist == 0){
                                    game.guesses_left--;
                                    char msg[MAX_MSG];
                                    sprintf(msg, "%s is not in the word\n", p->inbuf);
                                    printf("%s", msg);
                                    if (write(p->fd, msg, strlen(msg)) == -1){
                                        disconnection(&game, &(game.head), p);
                                        break;
                                    }

                                    // Game over
                                    if (game.guesses_left == 0){
                                      char end[MAX_MSG];
                                      sprintf(end, "No more guesses. The word was %s\n", game.word);
                                      if (broadcast(&game, end) == 1){
                                          break;
                                      }
                                      if (broadcast(&game, "\n\nLet's start a new game\n\0") == 1){
                                          break;
                                      }

                                      // Restart a new game
                                      struct client *head = game.head;
                                      struct client *cur = game.has_next_turn;
                                      init_game(&game, argv[1]);
                                      printf("New game\n");
                                      game.head = head;
                                      if (cur->next == NULL){
                                        game.has_next_turn = head;
                                      } else {
                                        game.has_next_turn = cur->next;
                                      }
                                      char status[MAX_MSG];
                                      if (broadcast(&game, status_message(status, &game)) == 1){
                                          break;
                                      }
                                      if (announce_turn(&game) == 1){
                                          break;
                                      }
                                      break;
                                    }
                                    advance_turn(&game);
                                } else{
                                    if (strcmp(game.guess, game.word) == 0){
                                      announce_winner(&game);
                                      break;
                                    }
                                }
                                char msg[MAX_MSG];
                                sprintf(msg, "%s guesses: %s\n", p->name, p->inbuf);
                                if (broadcast(&game, msg) == 1){
                                    break;
                                }
                                char status[MAX_MSG];
                                if (broadcast(&game, status_message(status, &game)) == 1){
                                    break;
                                }
                                if (announce_turn(&game) == 1){
                                    break;
                                }
                              }
                          }
                        }
                      }
                      break;
                    }
                }

                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        // TODO - handle input from an new client who has
                        // not entered an acceptable name.
                        if (read_buf(p) == 0){
                          remove_player(&new_players, p->fd);
                          break;
                        }
                        int exist = 0;
                        for (struct client *q = game.head; q != NULL; q = q->next){
                            if (strcmp(q->name, p->inbuf) == 0){
                                exist++;
                            }
                        }
                        if (exist > 0 || strlen(p->inbuf) > MAX_NAME){
                            if (write(p->fd, "Invalid Name\n", 13) == -1){
                                remove_player(&new_players, p->fd);
                                break;
                            }
                        } else {
                            //Remove the player out of linked list and into game
                            strcpy(p->name, p->inbuf);
                            new_players = p->next;
                            p->next = game.head;
                            game.head = p;
                            //Send necessary message to players
                            char out[MAX_MSG];
                            sprintf(out, "%s has just joined.\n", p->name);
                            printf("%s has just joined.\n", p->name);
                            if (broadcast(&game, out) == 1){
                                break;
                            }
                            if (game.has_next_turn != NULL){
                              char status[MAX_MSG];
                              if (write(p->fd, status_message(status, &game), strlen(status)) == -1){
                                remove_player(&new_players, p->fd);
                                break;
                              }
                            }

                            // For the very first player only
                            if (game.has_next_turn == NULL){
                                game.has_next_turn = p;
                                char status[MAX_MSG];
                                if (broadcast(&game, status_message(status, &game)) == 1){
                                    break;
                                }
                            }
                            if (announce_turn(&game) == 1){
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
    return 0;
}
