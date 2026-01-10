#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <semaphore.h>


#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2

#define MAX_PIPE_LEN 81

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    int slot_used;
    pthread_t t_client;
    char req_pipe_path[40];
    char notif_pipe_path[40];
} client_data;

int thread_shutdown = 0;

sem_t clients_semaphore;
client_data *clients;
char levels_path[MAX_FILENAME];

void send_board(board_t *game_board, int mode, int not_pipe_fd) {
    char *buf = calloc(7 + game_board->width*game_board->height, sizeof(char));
    int width = game_board->width;
    int height = game_board->height;
    int tempo = game_board->tempo;
    int victory = (mode == DRAW_WIN) ? 1 : 0;
    int game_over = (mode == DRAW_GAME_OVER) ? 1 : 0;
    int points = game_board->pacmans[0].points;

    size_t board_len = (size_t)(width * height);

    // 1 byte for opcode + 24 bytes for 6 integers + board data
    size_t total_len = 1 + 24 + board_len;

    buf[0] = '4';
    memcpy(buf+1, &width, 4);
    memcpy(buf+5, &height, 4);
    memcpy(buf+9, &tempo, 4);
    memcpy(buf+13, &victory, 4);
    memcpy(buf+17, &game_over, 4);
    memcpy(buf+21, &points, 4);
    
    for (size_t i=0; i<board_len; i++) {
        memcpy(buf+25+i, &game_board->board[i].content, 1);
    }
    write(not_pipe_fd, buf, total_len);
    free(buf);
}

void* pacman_thread(void *arg) {
    board_t *board = (board_t*) arg;

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));

    while (true) {
        if (!pacman->alive) {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        if (pacman->n_moves == 0) {
            c.command = get_input();

            if(c.command == '\0') {
                continue;
            }

            c.turns = 1;
            play = &c;
        }
        else {
            play = &pacman->moves[pacman->current_move%pacman->n_moves];
        }

        debug("KEY %c\n", play->command);

        // QUIT
        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        pthread_rwlock_rdlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            // Next level
            *retval = NEXT_LEVEL;
            break;
        }

        if(result == DEAD_PACMAN) {
            // Restart from child, wait for child, then quit
            *retval = QUIT_GAME;
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    pthread_rwlock_unlock(&board->state_lock);
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* manage_client_thread(void *arg) {
    client_data *cdata = &clients[(int)(intptr_t)arg];

    char* req_pipe_path = cdata->req_pipe_path;
    char* notif_pipe_path = cdata->notif_pipe_path;

    int fd_req, fd_notif;

    if ((fd_req = open(req_pipe_path, O_RDONLY)) < 0)
        goto exit;

    if ((fd_notif = open(notif_pipe_path, O_WRONLY)) < 0)
        goto exit;

    //write result to notif pipe
    write(fd_notif, "10", 2);

    DIR* level_dir = opendir(levels_path);
    if (level_dir == NULL)
        goto exit;

    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            load_level(&game_board, entry->d_name, levels_path, accumulated_points);

            while(true) {
                pthread_t pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                thread_shutdown = 0;

                pthread_create(&pacman_tid, NULL, pacman_thread, (void*) &game_board);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }

                int *retval;
                pthread_join(pacman_tid, (void**)&retval);

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);

                int result = *retval;
                free(retval);

                if (result == NEXT_LEVEL) {
                    send_board(&game_board, DRAW_WIN, fd_notif);
                    sleep_ms(game_board.tempo);
                    break;
                }

                if (result == QUIT_GAME) {
                    send_board(&game_board, DRAW_GAME_OVER, fd_notif);
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }

                accumulated_points = game_board.pacmans[0].points;      
            }
            unload_level(&game_board);
        }
    }    

    closedir(level_dir);

    exit:
    cdata->slot_used = 0;
    sem_post(&clients_semaphore);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <nome_do_FIFO_de_registo>\n", argv[0]);
        return -1;
    }

    unlink(argv[3]);
    // Create the server Pipe
    if (mkfifo(argv[3], 0777) < 0) {
        exit(1);
    }

    int fdserv; //server pipe
    if ((fdserv = open (argv[3], O_RDONLY)) < 0) {
        exit(1);
    }

    int num_clients = atoi(argv[2]);
    clients = calloc(num_clients, sizeof(client_data));
    if (sem_init(&clients_semaphore,0,num_clients)<0) exit(1); //init semaphore
    for (int i=0; i<num_clients; i++) clients[i].slot_used=0;

    char buf[MAX_PIPE_LEN] = {0};

    while (true) {
        if (read(fdserv, buf, 81) != 81) {
            continue;
        }

        //received connection request
        sem_wait(&clients_semaphore);
        int slot = -1;

        //find first empty slot
        for (int i=0; i<num_clients; i++) {
            if (!clients->slot_used) {
                slot = i;
                break;
            }
        }

        clients[slot].slot_used = 1;
        //Copy pipe paths from buffer (skipping opcode at index 0)
        memcpy(clients[slot].req_pipe_path, buf + 1, 40);
        memcpy(clients[slot].notif_pipe_path, buf + 41, 40);

        pthread_create(&clients[slot].t_client,NULL, manage_client_thread, (void*)(intptr_t)slot);
        pthread_detach(clients[slot].t_client);
    }

    return 0;
}
