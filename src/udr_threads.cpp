/*****************************************************************************
Copyright 2012 Laboratory for Advanced Computing at the University of Chicago

This file is part of UDR.

Licensed under the Apache License, Version 2.0 (the "License"); 
you may not use this file except in compliance with the License. 
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an "AS IS" BASIS, 
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
See the License for the specific language governing permissions 
and limitations under the License.
*****************************************************************************/

#include <pthread.h>
#include <sstream>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <udt.h>
#include "udr.h"
#include "udr_threads.h"

//the timeout can be used as a way to get around the issues happening with the final handle_to_udt thread hanging because can't get the signaling to work properly
//nope the timeout causes premature timeouts...
//int timeout = 10000;
int ppid_poll = 2;
bool thread_log = false;
string local_logfile_dir = "/home/aheath/projects/udr/log/thread_";

void print_bytes(FILE* file, const void *object, size_t size) {
  size_t i;

  fprintf(file, "[ ");
    for(i = 0; i < size; i++)
    {
      fprintf(file, "%02x ", ((const unsigned char *) object)[i] & 0xff);
    }
    fprintf(file, "]\n");
  }

string convert_int(int number) {
  stringstream ss;
  ss << number;
  return ss.str();
}

string udt_recv_string( int udt_handle ) {
  char buf[ 2 ];
  buf[ 1 ] = '\0';
  
  string str = "";
  
  for( ;; ) {
    int bytes_read = UDT::recv( udt_handle , buf , 1 , 0 );
    if ( bytes_read == UDT::ERROR ){
      cerr << "recv:" << UDT::getlasterror().getErrorMessage() << endl;
      break;
    }
    if ( bytes_read == 1 ) {
      if ( buf[ 0 ] == '\0' )
        break;
      str += buf;
    } 
    else {
      sleep(1);
    }
  }
  return str;
}

//Something bad is happening with the threading / signaling to interrupt the blocking read here, get stack smashing if try to handle gracefully
//This is the root of the rsync error that is thrown at the end.
void *handle_to_udt(void *threadarg) {
  struct thread_data *my_args = (struct thread_data *) threadarg;
  char indata[max_block_size];
  char outdata[max_block_size];
  FILE*  logfile;

  if(my_args->log) {
    string filename = my_args->logfile_dir + convert_int(my_args->id) + "_log.txt";
    logfile = fopen(filename.c_str(), "w");
  }

  while(true) {
    int ss;

    if(my_args->log) {
      fprintf(logfile, "%d: Should be reading from process...\n", my_args->id);
      fflush(logfile);
    }

    int bytes_read;
    if(my_args->crypt != NULL)
      bytes_read = read(my_args->fd, indata, max_block_size);
    else
      bytes_read = read(my_args->fd, outdata, max_block_size);

    if(bytes_read < 0){
      if(my_args->log){
        fprintf(logfile, "Error: %s\n", strerror(errno));
        fclose(logfile);
      }
      my_args->is_complete = true;
      return NULL;
    }
    if(bytes_read == 0) {
      if(my_args->log){
        fprintf(logfile, "%d Got %d bytes_read, exiting\n", my_args->id, bytes_read);
        fclose(logfile);
      }
      my_args->is_complete = true;
      return NULL;
    }

    if(my_args->crypt != NULL)
      my_args->crypt->encrypt(indata, outdata, bytes_read);

    if(my_args->log){
      fprintf(logfile, "%d bytes_read: %d\n", my_args->id, bytes_read);
      // print_bytes(logfile, outdata, bytes_read);
      fflush(logfile);
    }

    int ssize = 0;
    while(ssize < bytes_read) {
    	if (UDT::ERROR == (ss = UDT::send(*my_args->udt_socket, outdata + ssize, bytes_read - ssize, 0))) {

        if(my_args->log) {
          fprintf(logfile, "%d send error: %s\n", my_args->id, UDT::getlasterror().getErrorMessage());
          fclose(logfile);
        }
        my_args->is_complete = true;
        return NULL;
      }

      ssize += ss;
      if(my_args->log) {
        fprintf(logfile, "%d sender on socket %d bytes read: %d ssize: %d\n", my_args->id, *my_args->udt_socket, bytes_read, ssize);
        fflush(logfile);
      }
    }
  }
  my_args->is_complete = true;
}

void *udt_to_handle(void *threadarg) {
  struct thread_data *my_args = (struct thread_data *) threadarg;
  char indata[max_block_size];
  char outdata[max_block_size];
  FILE* logfile;
  
  if(my_args->log) {
    string filename = my_args->logfile_dir + convert_int(my_args->id) + "_log.txt";
    logfile = fopen(filename.c_str(), "w");
  }

  while(true) {
    int rs;

    if(my_args->log) {
      fprintf(logfile, "%d: Should now be receiving from udt...\n", my_args->id);
      fflush(logfile);
    }

    if (UDT::ERROR == (rs = UDT::recv(*my_args->udt_socket, indata, max_block_size, 0))) {
      if(my_args->log){
        fprintf(logfile, "%d recv error: %s\n", my_args->id, UDT::getlasterror().getErrorMessage());
        fclose(logfile);
      }
      my_args->is_complete = true;
      return NULL;
    }

    int written_bytes;
    if(my_args->crypt != NULL) {
      my_args->crypt->encrypt(indata, outdata, rs);
      written_bytes = write(my_args->fd, outdata, rs);
    }
    else {
      written_bytes = write(my_args->fd, indata, rs);
    }

    if(my_args->log) {
      fprintf(logfile, "%d recv on socket %d rs: %d written bytes: %d\n", my_args->id, *my_args->udt_socket, rs, written_bytes);
      fflush(logfile);
    }

    if(written_bytes < 0) {
      if(my_args->log){
        fprintf(logfile, "Error: %s\n", strerror(errno));
        fclose(logfile);
      }
      my_args->is_complete = true;
      return NULL;
    }
  }
  my_args->is_complete = true;
} 


int run_sender(char* receiver, char* receiver_port, bool encryption, unsigned char * passphrase, bool verbose_mode, const char* cmd, int argc, char ** argv) { 
  UDT::startup();
  struct addrinfo hints, *local, *peer;

  memset(&hints, 0, sizeof(struct addrinfo));

  hints.ai_flags = AI_PASSIVE;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  if (0 != getaddrinfo(NULL, receiver_port, &hints, &local)) {
    cerr << "Sender: incorrect network address.\n" << endl;
    return 1;
  }

  UDTSOCKET client = UDT::socket(local->ai_family, local->ai_socktype, local->ai_protocol);
  
  freeaddrinfo(local);

  if (0 != getaddrinfo(receiver, receiver_port, &hints, &peer)) {
    cerr << "Sender: incorrect server/peer address. " << receiver << ":" << receiver_port << endl;
    return 1;
  }

  // connect to the server, implict bind
  if (UDT::ERROR == UDT::connect(client, peer->ai_addr, peer->ai_addrlen)) {
    cerr << "Sender: connect: " << UDT::getlasterror().getErrorMessage() << endl;
    return 1;
  }

  freeaddrinfo(peer);

  // not using CC method -- perhaps an option later?
  //CUDPBlast* cchandle = NULL;
  int value;
  int temp;

  //UDT::getsockopt(client, 0, UDT_SNDBUF, &value, &temp);
  //UDT::setsockopt(client, 0, UDT_SNDTIMEO, "1000", 4);
  //cerr << "UDT_SNDBUF value: " << (int)value << endl;
  //UDT::getsockopt(client, 0, UDT_RCVBUF, &value, &temp);
  //cerr << "UDT_RCVBUF value: " << (int)value << endl;
  
  char* data = new char[max_block_size];

  ssize_t n;

  //very first thing we send is the rsync argument so that the rsync server can be started and piped to from the UDT connection
  n = strlen(cmd) + 1;
  int ssize = 0;
  int ss;
  while(ssize < n) {
    if (UDT::ERROR == (ss = UDT::send(client, cmd + ssize, n - ssize, 0)))
    {
      cerr << "Sender: Send:" << UDT::getlasterror().getErrorMessage() << endl;
      break;
    }

    ssize += ss;
  }

  //set the recv timeout?
  //UDT::setsockopt(client, 0, UDT_RCVTIMEO, &timeout, sizeof(int)); 

  struct thread_data sender_to_udt;
  sender_to_udt.udt_socket = &client;
  sender_to_udt.fd = STDIN_FILENO; //stdin of this process, from stdout of rsync
  sender_to_udt.id = 0;
  sender_to_udt.log = thread_log;
  sender_to_udt.logfile_dir = local_logfile_dir;
  sender_to_udt.is_complete = false;
  
  struct thread_data udt_to_sender;
  udt_to_sender.udt_socket = &client;
  udt_to_sender.fd = STDOUT_FILENO; //stdout of this process, going to stdin of rsync, rsync defaults to set this is non-blocking
  udt_to_sender.id = 1;
  udt_to_sender.log = thread_log;
  udt_to_sender.logfile_dir = local_logfile_dir;
  udt_to_sender.is_complete = false;

  if(encryption){
    crypto encrypt(BF_ENCRYPT, PASSPHRASE_SIZE, (unsigned char *) passphrase);
    crypto decrypt(BF_DECRYPT, PASSPHRASE_SIZE, (unsigned char *) passphrase);
    // free_key(passphrase);
    sender_to_udt.crypt = &encrypt;
    udt_to_sender.crypt = &decrypt;
  }
  else{
    sender_to_udt.crypt = NULL;
    udt_to_sender.crypt = NULL;
  }
  
  pthread_t sender_to_udt_thread;
  pthread_create(&sender_to_udt_thread, NULL, handle_to_udt, (void *)&sender_to_udt);

  pthread_t udt_to_sender_thread;
  pthread_create(&udt_to_sender_thread, NULL, udt_to_handle, (void*)&udt_to_sender);   

  int rc1 = pthread_join(udt_to_sender_thread, NULL);

  if(verbose_mode)
    fprintf(stderr, "Sender: joined on udt_to_sender_thread %d\n", rc1);

  pthread_kill(sender_to_udt_thread, SIGUSR1);
  
  int rc2 = pthread_join(sender_to_udt_thread, NULL);

  if(verbose_mode)
    fprintf(stderr, "Sender: joined on sender_to_udt_thread %d\n", rc2);

  UDT::close(client);
  UDT::cleanup();

  delete [] data;
  return 0;  
}

int run_receiver(int start_port, int end_port, bool encryption, bool verbose_mode) {
  int orig_ppid = getppid();

  UDT::startup();

  addrinfo hints;
  addrinfo* res;

  memset(&hints, 0, sizeof(struct addrinfo));

  hints.ai_flags = AI_PASSIVE;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char receiver_port[NI_MAXSERV];
  UDTSOCKET serv;

  bool bad_port = false;

  for(int port_num = start_port; port_num < end_port; port_num++) {
    bad_port = false;
    snprintf(receiver_port, sizeof(receiver_port), "%d", port_num);
    
    if (0 != getaddrinfo(NULL, receiver_port, &hints, &res)) {
      //fprintf(stderr, "Illegal port number or port is busy.\n");
      bad_port = true;
    }
    else {
      serv = UDT::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (UDT::ERROR == UDT::bind(serv, res->ai_addr, res->ai_addrlen)) {
        bad_port = true;
      }
    }

    freeaddrinfo(res);

    if(!bad_port)
      break;
  }

  if(bad_port){
    fprintf(stderr, "Receiver: ERROR: could not bind to any port in range %d - %d\n", start_port, end_port);
    return 0;
  }
  
  unsigned char rand_pp[PASSPHRASE_SIZE];
  int success = RAND_bytes((unsigned char *) rand_pp, PASSPHRASE_SIZE);

  //std out port number and password -- to send back to the client
  printf("%s ", receiver_port);  
  
  for(int i = 0; i < PASSPHRASE_SIZE; i++) {
    printf("%02x", rand_pp[i]);
  }
  printf(" \n");
  fflush(stdout);

  if(verbose_mode)
    fprintf(stderr, "Receiver: server is ready at port %s\n", receiver_port);

  if (UDT::ERROR == UDT::listen(serv, 10)) {
    cerr << "Receiver: listen: " << UDT::getlasterror().getErrorMessage() << endl;
    return 0;
  }

  sockaddr_storage clientaddr;
  int addrlen = sizeof(clientaddr);

  UDTSOCKET recver;

  if (UDT::INVALID_SOCK == (recver = UDT::accept(serv, (sockaddr*)&clientaddr, &addrlen))) {
    cerr << "Receiver: accept: " << UDT::getlasterror().getErrorMessage() << endl;
    return 0;
  }

  char clienthost[NI_MAXHOST];
  char clientservice[NI_MAXSERV];
  getnameinfo((sockaddr *)&clientaddr, addrlen, clienthost, sizeof(clienthost), clientservice, sizeof(clientservice), NI_NUMERICHOST|NI_NUMERICSERV);

  string cmd_str = udt_recv_string(recver);

  //parse the string into an array of argv for exec
  vector<char*> args;

  char *p = strtok( strdup(cmd_str.c_str()) , " " );
  while (p) {
    args.push_back( p );
    p = strtok( NULL , " " );
  }

  args.push_back( p );

  //set the receiving timeout?
  //UDT::setsockopt(recver, 0, UDT_RCVTIMEO, &timeout, sizeof(int)); 
  
  //now fork and exec the rsync server

  int child_to_parent, parent_to_child;
  int rsync_pid = run_rsync(&args[0], &parent_to_child, &child_to_parent);
  if(verbose_mode)
    fprintf(stderr, "Receiver: rsync pid: %d\n", rsync_pid);

  struct thread_data recv_to_udt;
  recv_to_udt.udt_socket = &recver;
  recv_to_udt.fd = child_to_parent; //stdout of rsync server process
  recv_to_udt.id = 2;
  recv_to_udt.log = thread_log;
  recv_to_udt.logfile_dir = local_logfile_dir;
  recv_to_udt.is_complete = false;

  struct thread_data udt_to_recv;
  udt_to_recv.udt_socket = &recver;
  udt_to_recv.fd = parent_to_child; //stdin of rsync server process
  udt_to_recv.id = 3;
  udt_to_recv.log = thread_log;
  udt_to_recv.logfile_dir = local_logfile_dir;
  udt_to_recv.is_complete = false;

  if(encryption){
    crypto encrypt(BF_ENCRYPT, PASSPHRASE_SIZE, rand_pp);
    crypto decrypt(BF_DECRYPT, PASSPHRASE_SIZE, rand_pp);
    recv_to_udt.crypt = &encrypt;
    udt_to_recv.crypt = &decrypt;
  }
  else{
    recv_to_udt.crypt = NULL;
    udt_to_recv.crypt = NULL;
  }

  pthread_t recv_to_udt_thread;
  pthread_create(&recv_to_udt_thread, NULL, handle_to_udt, (void *)&recv_to_udt);

  pthread_t udt_to_recv_thread;
  pthread_create(&udt_to_recv_thread, NULL, udt_to_handle, (void*)&udt_to_recv);

  if(verbose_mode){
    fprintf(stderr, "Receiver: waiting to join on recv_to_udt_thread\n");
    fprintf(stderr, "Receiver: ppid %d pid %d\n", getppid(), getpid());
  }

  //going to poll if the ppid changes then we know it's exited and then we exit all of our threads and exit as well
  //also check if the threads have exited to break, still have the joins to ensure any cleanup is also completed
  while(true){
    if(getppid() != orig_ppid){
      pthread_kill(recv_to_udt_thread, SIGUSR1);
      pthread_kill(udt_to_recv_thread, SIGUSR1);
      break;
    }
    if(recv_to_udt.is_complete || udt_to_recv.is_complete)
      break;
    sleep(ppid_poll);
  }

  int rc1 = pthread_join(recv_to_udt_thread, NULL);
  if(verbose_mode)
    fprintf(stderr, "Receiver: Joined recv_to_udt_thread %d\n", rc1);

  UDT::close(recver);

  if(verbose_mode)
    fprintf(stderr, "Receiver: Closed recver\n");


  UDT::close(serv);

  if(verbose_mode)
    fprintf(stderr, "Receiver: Closed serv\n");

  // use this function to release the UDT library
  UDT::cleanup();

  if(verbose_mode)
    fprintf(stderr, "Receiver: UDT cleaned up\n");

  int rc2 = pthread_join(udt_to_recv_thread, NULL);
  
  if(verbose_mode)
    fprintf(stderr, "Receiver: Joined udt_to_recv_thread %d Should be closing recver now\n", rc2);

  return 1;
}