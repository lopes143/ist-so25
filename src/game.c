#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <sys/wait.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4
#define PACMAN_DIED 5

#define OUT_BACKUP 0
#define IN_BACKUP 1

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
    if(game_board->tempo != 0)
        sleep_ms(game_board->tempo);       
}

int play_board(board_t * game_board) {
    pacman_t* pacman = &game_board->pacmans[0];
    command_t* play;
    command_t c;
    if (pacman->n_moves == 0) { // if is user input
        c.command = get_input();

        if(c.command == '\0')
            return CONTINUE_PLAY;

        c.turns = 1;
        play = &c;
    }
    else { // else if the moves are pre-defined in the file
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the pacman
        play = &pacman->moves[pacman->current_move%pacman->n_moves];
    }

    debug("KEY %c\n", play->command);

    if (play->command == 'Q') {
        return QUIT_GAME;
    }

    if (play->command == 'G') {
        // Save game state
        return CREATE_BACKUP;
    }

    if (play->command == 'L') {
        // Load game state
        return LOAD_BACKUP;
    }

    int result = move_pacman(game_board, 0, play);
    if (result == REACHED_PORTAL) {
        // Next level
        return NEXT_LEVEL;
    }

    if(result == DEAD_PACMAN) {
        return PACMAN_DIED;
    }
    
    for (int i = 0; i < game_board->n_ghosts; i++) {
        ghost_t* ghost = &game_board->ghosts[i];
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the ghost
        move_ghost(game_board, i, &ghost->moves[ghost->current_move%ghost->n_moves]);
    }

    if (!game_board->pacmans[0].alive) {
        return QUIT_GAME;
    }      

    return CONTINUE_PLAY;  
}

int main(int argc, char** argv) {
    int pid, estado, isInBackup=OUT_BACKUP;

    if (argc != 2) {
        printf("Usage: %s <level_directory>\n", argv[0]);
        return EXIT_FAILURE;
    }
    DIR *dir = opendir(argv[1]);
    if (dir==NULL) {
        perror("Failed to open directory");
        return EXIT_FAILURE;
    }

    char levels[MAX_LEVELS][MAX_FILENAME] = {0};
    int level_count = 0;
    while (true) {
        struct dirent *dp = readdir(dir);
        if (dp==NULL) break;
        if (!strcmp(dp->d_name,".") || !strcmp(dp->d_name,"..")) {
            continue; //Skip . and ..
        }

        char *ext = strrchr(dp->d_name, '.');
        if (ext!=NULL && !strcmp(ext, ".lvl")) {
            strcpy(levels[level_count++],dp->d_name);
        }
    }

    //print filenames
    for (int i=0; i<level_count; i++) {
        printf("%s\n", levels[i]);
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    open_debug_file("debug.log");

    terminal_init();
    
    int accumulated_points = 0,
        current_level = 0;
    bool end_game = false;
    board_t game_board;

    while (!end_game && current_level<level_count) {
        load_level(&game_board, accumulated_points, levels[current_level]);
        draw_board(&game_board, DRAW_MENU);
        refresh_screen();

        while(true) {
            int result = play_board(&game_board); 

            if(result == NEXT_LEVEL) {
                screen_refresh(&game_board, DRAW_WIN);
                sleep_ms(game_board.tempo);
                current_level++;
                break;
            }

            if(result == PACMAN_DIED) {
                if (isInBackup == IN_BACKUP) {
                    exit(0);
                }else 
                {
                    result = QUIT_GAME;
                }
            }

            if(result == QUIT_GAME) {
                screen_refresh(&game_board, DRAW_GAME_OVER); 
                sleep_ms(game_board.tempo);
                end_game = true;
                break;
            }

            if(result == CREATE_BACKUP && isInBackup == OUT_BACKUP) {
                pid = fork();
                if (pid == 0) {
                    isInBackup=IN_BACKUP;
                } else if (pid > 0) {
                    pid = wait(&estado);
                } else {
                    // Fork failed
                    perror("Fork failed");
                }
            }
    
            screen_refresh(&game_board, DRAW_MENU); 

            accumulated_points = game_board.pacmans[0].points;      
        }
        print_board(&game_board);
        unload_level(&game_board);
    }    

    terminal_cleanup();

    close_debug_file();

    return 0;
}
