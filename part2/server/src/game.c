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
    int slot_used;
    pthread_t t_client;
    int req_pipe_fd;
    char req_pipe_path[40];
    int notif_pipe_fd;
    char notif_pipe_path[40];
} client_data;

typedef struct {
    board_t *board;
    client_data *client;
} pacman_thread_arg_t;

typedef struct {
    board_t *board;
    int ghost_index;
    client_data *client;
} ghost_thread_arg_t;

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
        char elem;
        if (game_board->board[i].has_dot) elem = '.';
        else if (game_board->board[i].has_portal) elem = '@';
        else if (game_board->board[i].content=='W') elem = '#';
        else if (game_board->board[i].content=='P') elem = 'C';
        else elem = game_board->board[i].content;
        memcpy(buf+25+i, &elem, 1);
    }
    write(not_pipe_fd, buf, total_len);
    free(buf);
}

void* pacman_thread(void *arg) {
    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t*) arg;

    board_t *board = pacman_arg->board;
    pacman_t *pacman = &board->pacmans[0];
    int req_pipe_fd = pacman_arg->client->req_pipe_fd;
    int not_pipe_fd = pacman_arg->client->notif_pipe_fd;

    int *retval = malloc(sizeof(int));
    free(pacman_arg);

    while (true) {
        if (!pacman->alive) {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        
        //get command from pipe
        char buf[2];
        read(req_pipe_fd, buf, 2);
            
        c.command = buf[1];

        c.turns = 1;
        play = &c;

        // QUIT
        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        pthread_rwlock_rdlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        send_board(board, DRAW_MENU, not_pipe_fd);

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
    int not_pipe_fd = ghost_arg->client->notif_pipe_fd;


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
        send_board(board, DRAW_MENU, not_pipe_fd);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* manage_client_thread(void *arg) {
    client_data *cdata = &clients[(int)(intptr_t)arg];

    char* req_pipe_path = cdata->req_pipe_path;
    char* notif_pipe_path = cdata->notif_pipe_path;

    if ((cdata->req_pipe_fd = open(req_pipe_path, O_RDONLY)) < 0)
        goto exit;

    if ((cdata->notif_pipe_fd = open(notif_pipe_path, O_WRONLY)) < 0)
        goto exit;

    //write result to notif pipe
    write(cdata->notif_pipe_fd, "10", 2);

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

                pacman_thread_arg_t *arg = malloc(sizeof(pacman_thread_arg_t));
                arg->board = &game_board;
                arg->client = cdata;
                pthread_create(&pacman_tid, NULL, pacman_thread, (void*)arg);

                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    arg->client = cdata;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*)arg);
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
                    send_board(&game_board, DRAW_WIN, cdata->notif_pipe_fd);
                    sleep_ms(game_board.tempo);
                    break;
                }
                if (result == QUIT_GAME) {
                    send_board(&game_board, DRAW_GAME_OVER, cdata->notif_pipe_fd);
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
    close(cdata->notif_pipe_fd);
    close(cdata->req_pipe_fd);
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

    strcpy(levels_path, argv[1]);
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
            if (clients[i].slot_used==0) {
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
