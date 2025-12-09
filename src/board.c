#include "board.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

#define BUF_SIZE 1024


FILE * debugfile;

// Helper private function to find and kill pacman at specific position
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {
    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

// Helper private function for getting board position index
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Helper private function for checking valid position
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height); // Inside of the board boundaries
}

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int move_pacman(board_t* board, int pacman_index, command_t* command) {
    if (pacman_index < 0 || !board->pacmans[pacman_index].alive) {
        return DEAD_PACMAN; // Invalid or dead pacman
    }

    pacman_t* pac = &board->pacmans[pacman_index];
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    // check passo
    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;        
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    pac->current_move+=1;

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);
    char target_content = board->board[new_index].content;

    if (board->board[new_index].has_portal) {
        board->board[old_index].content = ' ';
        board->board[new_index].content = 'P';
        return REACHED_PORTAL;
    }

    // Check for walls
    if (target_content == 'W') {
        return INVALID_MOVE;
    }

    // Check for ghosts
    if (target_content == 'M') {
        kill_pacman(board, pacman_index);
        return DEAD_PACMAN;
    }

    // Collect points
    if (board->board[new_index].has_dot) {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    board->board[old_index].content = ' ';
    pac->pos_x = new_x;
    pac->pos_y = new_y;
    board->board[new_index].content = 'P';

    return VALID_MOVE;
}

// Helper private function for charged ghost movement in one direction
static int move_ghost_charged_direction(board_t* board, ghost_t* ghost, char direction, int* new_x, int* new_y) {
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    *new_x = x;
    *new_y = y;
    
    switch (direction) {
        case 'W': // Up
            if (y == 0) return INVALID_MOVE;
            *new_y = 0; // In case there is no colision
            for (int i = y - 1; i >= 0; i--) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i + 1; // stop before colision
                    return VALID_MOVE;
                }
                else if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'S': // Down
            if (y == board->height - 1) return INVALID_MOVE;
            *new_y = board->height - 1; // In case there is no colision
            for (int i = y + 1; i < board->height; i++) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'A': // Left
            if (x == 0) return INVALID_MOVE;
            *new_x = 0; // In case there is no colision
            for (int j = x - 1; j >= 0; j--) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j + 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'D': // Right
            if (x == board->width - 1) return INVALID_MOVE;
            *new_x = board->width - 1; // In case there is no colision
            for (int j = x + 1; j < board->width; j++) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;
        default:
            debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
            return INVALID_MOVE;
    }
    return VALID_MOVE;
}   

int move_ghost_charged(board_t* board, int ghost_index, char direction) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    int new_x = x;
    int new_y = y;

    ghost->charged = 0; //uncharge
    int result = move_ghost_charged_direction(board, ghost, direction, &new_x, &new_y);
    if (result == INVALID_MOVE) {
        debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
        return INVALID_MOVE;
    }

    // Get board indices
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one
    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    // Update board - set new position
    board->board[new_index].content = 'M';
    return result;
}

int move_ghost(board_t* board, int ghost_index, command_t* command) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    // check passo
    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;
    
    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'C': // Charge
            ghost->current_move += 1;
            ghost->charged = 1;
            return VALID_MOVE;
        case 'T': // Wait
            if (command->turns_left == 1) {
                ghost->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    ghost->current_move++;
    if (ghost->charged)
        return move_ghost_charged(board, ghost_index, direction);

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    // Check board position
    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    char target_content = board->board[new_index].content;

    // Check for walls and ghosts
    if (target_content == 'W' || target_content == 'M') {
        return INVALID_MOVE;
    }

    int result = VALID_MOVE;
    // Check for pacman
    if (target_content == 'P') {
        result = find_and_kill_pacman(board, new_x, new_y);
    }

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one

    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;

    // Update board - set new position
    board->board[new_index].content = 'M';
    return result;
}

void kill_pacman(board_t* board, int pacman_index) {
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t* pac = &board->pacmans[pacman_index];
    int index = pac->pos_y * board->width + pac->pos_x;

    // Remove pacman from the board
    board->board[index].content = ' ';

    // Mark pacman as dead
    pac->alive = 0;
}

// Static Loading
int load_pacman(board_t* board, int points) {
    char *path = board->pacman_file;
    pacman_t *pacman = &board->pacmans[board->n_pacmans-1];
    const int fd = open(path, O_RDONLY);

    if (fd<0) {
        perror("Load pacman file error!");
        return EXIT_FAILURE;
    }

    fseek(fd,0,SEEK_END);
    const long fsize = ftell(fd);
    fseek(fd,0,SEEK_SET);

    char buf[BUF_SIZE];
    int readChars;
    
    char *fileText = calloc(fsize+1,sizeof(char));
    if (fileText==NULL) {
        perror("Unable to allocate memory for pacman content");
        return EXIT_FAILURE;
    }

    while ((readChars=read(fd,buf,BUF_SIZE-1))>0) {
        strcat(fileText, buf);
    }

    strcat(fileText, '\0'); //maybe not needed

    char *line_saveptr;
    char *line = strtok_r(fileText, "\n", &line_saveptr);
    while (line != NULL) {
        if (line[0]=='#' || line[0]=='\0') { //Ignore comments and empty lines
            line = strtok_r(NULL, "\n", &line_saveptr);
            continue;
        }

        char *args[MAX_GHOSTS]; //Large enough
        int arg_count = 0;

        char *word_saveptr;
        char *word = strtok_r(line, " ", &word_saveptr);
        while (word != NULL && arg_count < MAX_GHOSTS) {
            args[arg_count++] = word; //Store the pointer, don't copy the string
            word = strtok_r(NULL, " ", &word_saveptr);
        }

        if (!strcmp(args[0], "PASSO")) { //parse PASSO
            pacman->passo=atoi(args[1]);
        }
        else if (!strcmp(args[0], "POS")) { //parse POS
            pacman->pos_x=atoi(args[1]);
            pacman->pos_y=atoi(args[2]);
            board->board[pacman->pos_x * board->width + pacman->pos_y].content='P';
        }
        else {
            //parse commands
            pacman->moves[pacman->n_moves].command=atoi(args[0]);
            if (arg_count>1 && !strcmp(args[0],"T")) {
                pacman->moves[pacman->n_moves].turns=atoi(args[1]);
                pacman->moves[pacman->n_moves].turns_left=atoi(args[1]);
            }
            else {
                pacman->moves[pacman->n_moves].turns=1;
                pacman->moves[pacman->n_moves].turns_left=1;
            }
            pacman->n_moves++;
        }
    }

    pacman->alive=1;
    pacman->points=points;
    return EXIT_FAILURE;
}

// Static Loading
int load_ghost(board_t* board) {
    // Ghost 0
    board->board[3 * board->width + 1].content = 'M'; // Monster
    board->ghosts[0].pos_x = 1;
    board->ghosts[0].pos_y = 3;
    board->ghosts[0].passo = 0;
    board->ghosts[0].waiting = 0;
    board->ghosts[0].current_move = 0;
    board->ghosts[0].n_moves = 16;
    for (int i = 0; i < 8; i++) {
        board->ghosts[0].moves[i].command = 'D';
        board->ghosts[0].moves[i].turns = 1; 
    }
    for (int i = 8; i < 16; i++) {
        board->ghosts[0].moves[i].command = 'A';
        board->ghosts[0].moves[i].turns = 1; 
    }

    // Ghost 1
    board->board[2 * board->width + 4].content = 'M'; // Monster
    board->ghosts[1].pos_x = 4;
    board->ghosts[1].pos_y = 2;
    board->ghosts[1].passo = 1;
    board->ghosts[1].waiting = 1;
    board->ghosts[1].current_move = 0;
    board->ghosts[1].n_moves = 1;
    board->ghosts[1].moves[0].command = 'R'; // Random
    board->ghosts[1].moves[0].turns = 1; 
    
    return 0;
}

int load_level(board_t *board, int points, char *level_file) {
    int fd = open("test.txt", O_RDONLY); //TODO change test.txt

    if (fd < 0) {
        perror("Load level file error!");
        return EXIT_FAILURE;
    }

    fseek(fd, 0, SEEK_END);
    long fsize = ftell(fd); //get file size by putting the cursor at the end
    fseek(fd, 0, SEEK_SET); //get cursor back to beginning

    char buf[BUF_SIZE];
    int readChars;
    int BoardRowParser = 0; //needed for board parsing - track row position
    
    char *fileText = calloc(fsize+1,sizeof(char));
    if (fileText==NULL) {
        perror("Unable to allocate memory for level content");
        close(fd);
        return EXIT_FAILURE;
    }
    while ((readChars=read(fd, buf, BUF_SIZE-1))>0) {
        strcat(fileText, buf);
    }

    strcat(fileText, '\0'); //maybe not needed

    char *line_saveptr;
    char *line = strtok_r(fileText, "\n", &line_saveptr);
    while (line != NULL) {
        if (line[0] == '#' || line[0] == '\0') { // Ignore comments and empty lines
            line = strtok_r(NULL, "\n", &line_saveptr);
            continue;
        }

        char *args[MAX_GHOSTS+2]; //Sufficiently large for MON command
        int arg_count=0;
        char *word_saveptr;

        char *token = strtok_r(line, " ", &word_saveptr);

        while (token != NULL) {
            args[arg_count++] = token; // Store the pointer, don't copy the string
            token = strtok_r(NULL, " ", &word_saveptr);
        }

        if (!strcmp(args[0], "D")) { //parse 'DIM'
            board->width = atoi(args[1]);
            board->height = atoi(args[2]);
            board->board = calloc(board->width * board->height, sizeof(board_pos_t));
        }
        else if (!strcmp(args[0], "T")) { //parse TEMPO
            board->tempo = atoi(args[1]);
        }
        else if (!strcmp(args[0], "P")) { //parse PAC
            strcpy(board->pacman_file, args[1]);
            board->n_pacmans = 1;
            board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));

            load_pacman(board, points);
        }
        else if (!strcmp(args[0], "M")) { //parse MON
            board->n_ghosts = arg_count-1;
            board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));
            for (int i=1; i<arg_count; i++) {
                strcpy(board->ghosts_files[i-1], args[i]);
            }
            load_ghost(board); //TODO This will need to be dynamic later
        }
        else { //parse board matrix
            char *board_line_str = args[0];
            for (size_t i = 0; i < strlen(board_line_str) && i < board->width; i++) {
                board_pos_t *pos = &board->board[BoardRowParser * board->width + i];
                switch (board_line_str[i]) {
                    case 'X':
                        pos->content = 'W';
                        break;
                    case 'o':
                        pos->content = ' ';
                        pos->has_dot = 1;
                        pos->has_portal = 0;
                        break;
                    case '@':
                        pos->content = ' ';
                        pos->has_dot = 0;
                        pos->has_portal = 1;
                        break;
                }
            }
            BoardRowParser++;
        }
        line = strtok_r(NULL, "\n", &line_saveptr);
    }

    free(fileText);
    return EXIT_SUCCESS;
}

void unload_level(board_t * board) {
    free(board->board);
    free(board->pacmans);
    free(board->ghosts);
}

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}

void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    // Large buffer to accumulate the whole output
    char buffer[8192];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== BOARD ===\n");

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");

    buffer[offset] = '\0';

    debug("%s", buffer);
}
