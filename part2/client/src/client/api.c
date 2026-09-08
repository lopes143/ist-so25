#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdbool.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  unlink(req_pipe_path);
  unlink(notif_pipe_path);

  //Create pipes
  if (mkfifo(notif_pipe_path, 0777)<0) goto fail_notif; //Notification pipe (server->client)
  if (mkfifo(req_pipe_path, 0777)<0) goto fail_req; //Request pipe (client->server)

  //open server pipe
  int server_pipe = open(server_pipe_path, O_WRONLY);
  if (server_pipe<0) goto fail_server; //pipe does not exist

  //send request
  char buf[81] = {0};
  buf[0] = '1'; // opcode
  strncpy(buf + 1, req_pipe_path, 40);
  strncpy(buf + 41, notif_pipe_path, 40);
  write(server_pipe, buf, 81);

  if ((session.req_pipe=open(req_pipe_path,O_WRONLY))<0) goto fail_open_req;
  if ((session.notif_pipe=open(notif_pipe_path,O_RDONLY))<0) goto fail_open_notif;
  strcpy(session.notif_pipe_path, notif_pipe_path);

  //recieve confirmation
  char result[2] = {0};
  while (1) {
    if (read(session.notif_pipe,result,2)==2) {
      if (result[0]=='1' && result[1]=='0') { //10: succeded
        goto success;
      }
    }
  }

  fail_open_notif:
  close(session.req_pipe);
  fail_open_req:
  close(session.notif_pipe);
  fail_server:
  close(server_pipe);
  fail_req:
  unlink(req_pipe_path);
  fail_notif:
  unlink(notif_pipe_path);

  return EXIT_FAILURE;

  success:
  close(server_pipe);
  return EXIT_SUCCESS;
}

void pacman_play(char command) {
  write(session.req_pipe, "3", 1);
  write(session.req_pipe, &command, 1);
}

int pacman_disconnect() {
  write(session.req_pipe, "2", 1);
  close(session.notif_pipe);
  close(session.req_pipe);
  return EXIT_SUCCESS;
}

Board receive_board_update(void) {
  Board board;
  char buf = '\0';
  char data[24] = {0};

  while (true) {
    read(session.notif_pipe, &buf, 1);
    if (buf=='4') break; //board received
  }

  read(session.notif_pipe, data, 24);

  memcpy(&board.width, data, 4);
  memcpy(&board.height, data + 4, 4);
  memcpy(&board.tempo, data + 8, 4);
  memcpy(&board.victory, data + 12, 4);
  memcpy(&board.game_over, data + 16, 4);
  memcpy(&board.accumulated_points, data + 20, 4);
  
  board.data = malloc(board.width * board.height);
  read(session.notif_pipe, board.data, board.width*board.height);

  return board;
}