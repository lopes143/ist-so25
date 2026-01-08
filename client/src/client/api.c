#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  //open server pipe
  int server_pipe = open(server_pipe_path, O_WRONLY);
  if (server_pipe<0) return EXIT_FAILURE; //pipe does not exist

  //create notification pipe (server->client)
  if (mkfifo(notif_pipe_path, 777)<0) return EXIT_FAILURE;
  if ((session.notif_pipe=open(notif_pipe_path,O_RDONLY))<0) return EXIT_FAILURE;
  strcpy(session.notif_pipe_path, notif_pipe_path);

  //send request
  char req[40] = {0};
  char not[40] = {0};
  strcpy(req, req_pipe_path);
  strcpy(not, notif_pipe_path);
  write(server_pipe, "1", 1);   //opcode
  write(server_pipe, req, 40);
  write(server_pipe, not, 40);

  //recieve confirmation
  char buf[2] = {0};
  while (1) {
    if (read(session.notif_pipe,buf,2)==2) {
      if (!strcmp(buf,"10")) { //succeded
        break;
      }
    }
  }

  close(server_pipe);
  return EXIT_SUCCESS;
}

void pacman_play(char command) {
  if (write(session.req_pipe, "3", 1)<1 || write(session.req_pipe, command, 1)<1)
    return -1;
}

int pacman_disconnect() {
  write(session.req_pipe, "2", 1);
  close(session.notif_pipe);
  close(session.req_pipe);
  return EXIT_SUCCESS;
}

Board receive_board_update(void) {
  Board board;
  char buf;
  int data[6] = {0};

  read(session.notif_pipe, buf, 1);
  if (buf!='4'); //something is not right

  read(session.notif_pipe, data, 6);
  board.width = data[0];
  board.height = data[1];
  board.tempo = data[2];
  board.victory = data[3];
  board.game_over = data[4];
  board.accumulated_points = data[5];

  read(session.notif_pipe, board.data, board.width*board.height);
}