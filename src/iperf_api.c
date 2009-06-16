#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include<fcntl.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <pthread.h> 
#include <stdint.h>

#include <sys/time.h>

#include "iperf_api.h"
#include "timer.h"
#include "net.h"
#include "units.h"
#include "tcp_window_size.h"

enum {
    Ptcp = SOCK_STREAM,
    Pudp = SOCK_DGRAM,
	
    uS_TO_NS = 1000,
	RATE = 1000000,
	MAX_BUFFER_SIZE =10,
    DEFAULT_UDP_BUFSIZE = 1470,
    DEFAULT_TCP_BUFSIZE = 8192
};
#define SEC_TO_NS 1000000000 /* too big for enum on some platforms */


static struct option longopts[] =
{
{ "client",         required_argument,      NULL,   'c' },
{ "server",         no_argument,            NULL,   's' },
{ "time",           required_argument,      NULL,   't' },
{ "port",           required_argument,      NULL,   'p' },
{ "parallel",       required_argument,      NULL,   'P' },
{ "udp",            no_argument,            NULL,   'u' },
{ "bandwidth",      required_argument,      NULL,   'b' },
{ "length",         required_argument,      NULL,   'l' },
{ "window",         required_argument,      NULL,   'w' },
{"interval",		required_argument,		NULL,	'i'},
{ NULL,             0,                      NULL,   0   }	
};


void Display(struct iperf_test *test);

int iperf_tcp_accept(struct iperf_test *test);
int iperf_udp_accept(struct iperf_test *test);
int iperf_tcp_recv(struct iperf_stream *sp);
int iperf_udp_recv(struct iperf_stream *sp);
int iperf_tcp_send(struct iperf_stream *sp);
int iperf_udp_send(struct iperf_stream *sp);

void iperf_defaults(struct iperf_test *testp);

struct iperf_test *iperf_new_test();
void iperf_init_test(struct iperf_test *test);
void iperf_free_test(struct iperf_test *test);
void *iperf_stats_callback(struct iperf_test *test);

struct iperf_stream *iperf_new_stream(struct iperf_test *testp);
struct iperf_stream *iperf_new_tcp_stream(struct iperf_test *testp);
struct iperf_stream * iperf_new_udp_stream(struct iperf_test *testp);
void iperf_init_stream(struct iperf_stream *sp, struct iperf_test *testp);
int iperf_add_stream(struct iperf_test *test, struct iperf_stream *sp);
struct iperf_stream * update_stream(struct iperf_test *test, int j, int add);
void iperf_free_stream(struct iperf_test *test, struct iperf_stream *sp);

void iperf_run_server(struct iperf_test *test);
void iperf_run_client(struct iperf_test *test);
int iperf_run(struct iperf_test *test);

void Display(struct iperf_test *test)
{
	struct iperf_stream *n;
	n= test->streams;
	int count=1;
	
	printf("===============DISPLAY==================\n");
	
	while(1)
	{	
		if(n)
		{
			if(test->role == 'c')
				printf("position-%d\tsp=%d\tsocket=%d\tbytes sent=%llu\n",count++, (int)n, n->socket, n->result->bytes_sent);
			else
				printf("position-%d\tsp=%d\tsocket=%d\tbytes received=%llu\n",count++, (int)n, n->socket, n->result->bytes_received);
			
			if(n->next==NULL)
			{
				printf("=================END====================\n");
				fflush(stdout);
				break;
			}
			n=n->next;
		}
	}
}

int iperf_tcp_recv(struct iperf_stream *sp)
{
	int result;
	int size = ((struct iperf_settings *) sp->settings)->bufsize;
	char* buf = (char *) malloc(size);
	if(!buf)
	{
		perror("malloc: unable to allocate receive buffer");
	}
			
	do{
		result = recv(sp->socket, buf, size, 0);	
		
		
	} while (result == -1 && errno == EINTR);
	
	sp->result->bytes_received+= result;
	
	return result;
}

int iperf_udp_recv(struct iperf_stream *sp)
{
	
	int result;
	int size = ((struct iperf_settings *) sp->settings)->bufsize;
	char* buf = (char *) malloc(size);

	if(!buf)
	{
		perror("malloc: unable to allocate receive buffer");
	}
	
	do{
		result = recv(sp->socket, buf, size, 0);
		
	} while (result == -1 && errno == EINTR);
		
	sp->result->bytes_received+= result;
	
	return result;	
}

int iperf_tcp_send(struct iperf_stream *sp)
{
	int result,i;
	int size = ((struct iperf_settings *) sp->settings)->bufsize;
	
	char *buf = (char *) malloc(size);
	if(!buf)
	{
		perror("malloc: unable to allocate transmit buffer");
	}
	for(i=0; i < size; i++)
		buf[i] = i % 37;
	
	result = send(sp->socket, buf, size , 0);	
	sp->result->bytes_sent+= size;
	
	return result;	
}

int iperf_udp_send(struct iperf_stream *sp)
{
	int result,i;
	int size = ((struct iperf_settings *) sp->settings)->bufsize;
	char *buf = (char *) malloc(size);
	if(!buf)
	{
		perror("malloc: unable to allocate transmit buffer");
	}	
	for(i=0; i < size; i++)
		buf[i] = i % 37;
	
	result = send(sp->socket, buf, size, 0);	
	sp->result->bytes_sent+= result;
	
	return result;
}

struct iperf_test *iperf_new_test()
{
	struct iperf_test *testp;
	
	testp = (struct iperf_test *) malloc(sizeof(struct iperf_test));
	if(!testp)
	{
		perror("malloc");
		return(NULL);
	}
	
	// initialise everything to zero
	memset(testp, 0, sizeof(struct iperf_test));

    testp->remote_addr = (struct sockaddr_storage *) malloc(sizeof(struct sockaddr_storage));
	memset(testp->remote_addr, 0, sizeof(struct sockaddr_storage));

    testp->local_addr = (struct sockaddr_storage *) malloc(sizeof(struct sockaddr_storage));
	memset(testp->local_addr, 0, sizeof(struct sockaddr_storage));

    testp->default_settings = (struct iperf_settings *) malloc(sizeof(struct iperf_settings));
    memset(testp->default_settings, 0, sizeof(struct iperf_settings));
	
	// return an empty iperf_test* with memory alloted.
	return testp;
}

void iperf_defaults(struct iperf_test *testp)
{
	testp->protocol = Ptcp;
	testp->role = 's';
	testp->listener_port = 5001;	
	testp->duration = 10;
	
	testp->stats_interval = 0;
	testp->reporter_interval = 0;
		
	testp->num_streams = 1;
	
	testp->default_settings->socket_bufsize = 1024*1024;
	testp->default_settings->bufsize = DEFAULT_TCP_BUFSIZE;
	testp->default_settings->rate = RATE;
	
	
}
	
void iperf_init_test(struct iperf_test *test)
{
	char ubuf[UNIT_LEN];
	struct iperf_stream *sp;
	int i, port,s=0;
	char client[512];
	
	
	if(test->role == 's')
	{		
		test->listener_sock = netannounce(test->protocol, NULL, test->listener_port);
		if( test->listener_sock < 0)
			exit(0);
				
		
		if(set_tcp_windowsize( test->listener_sock, test->default_settings->socket_bufsize, SO_RCVBUF) < 0) 
		{
			perror("unable to set window");
		}
		
		printf("-----------------------------------------------------------\n");
		printf("Server listening on %d\n",ntohs(((struct sockaddr_in *)(test->local_addr))->sin_port));		// need to change this port assignment
		int x;
		if((x = getsock_tcp_windowsize( test->listener_sock, SO_RCVBUF)) < 0) 
			perror("SO_RCVBUF");	
	
		
		unit_snprintf(ubuf, UNIT_LEN, (double) x, 'A');
		printf("%s: %s\n",
			   test->protocol == Ptcp ? "TCP window size" : "UDP buffer size", ubuf);
		printf("-----------------------------------------------------------\n");
		
	}
	
	else if( test->role == 'c')
	{
		FD_ZERO(&test->write_set);
		FD_SET(s, &test->write_set);
		
		inet_ntop(AF_INET, &((struct sockaddr_in *)(test->remote_addr))->sin_addr, client, sizeof(client));
		port = ntohs(((struct sockaddr_in *)(test->remote_addr))->sin_port);
		
		for(i = 0; i < test->num_streams; i++)
		{
			s = netdial(test->protocol, client, port);
			
			if(s < 0) 
			{
				fprintf(stderr, "netdial failed\n");
				exit(0);				
			}
			
			FD_SET(s, &test->write_set);
			test->max_fd = (test->max_fd < s) ? s : test->max_fd;
			
			set_tcp_windowsize(sp->socket, test->default_settings->socket_bufsize, SO_SNDBUF);
			
			if(s < 0)
				exit(0);
			
			//setting noblock causes error in byte count -kprabhu
			//setnonblocking(s);
			
			sp = test->new_stream(test);
			sp->socket = s;
			iperf_init_stream(sp, test);
			iperf_add_stream(test, sp);
			//connect_msg(sp);		
		}
	}						
}

void iperf_free_test(struct iperf_test *test)
{
	
	free(test->remote_addr);
	free(test->local_addr);
	free(test->default_settings);	
	free(test);
}

void *iperf_stats_callback(struct iperf_test *test)
{
	char ubuf[UNIT_LEN];
	
	test->streams->result->interval_results->bytes_transferred = test->streams->result->bytes_sent -  test->streams->result->interval_results->bytes_transferred;
	
	unit_snprintf(ubuf, UNIT_LEN, (double) (test->streams->result->interval_results->bytes_transferred / test->stats_interval), 'a');
	printf("interval === %llu bytes sent \t %s per sec \n", test->streams->result->interval_results->bytes_transferred , ubuf);
	
	
		
}


void iperf_free_stream(struct iperf_test *test, struct iperf_stream *sp)
{
	
	struct iperf_stream *prev,*start;
	prev = test->streams;
	start = test->streams;
	
	if(test->streams->socket == sp->socket)
	{		
		test->streams = test->streams->next;		
	}	
	else
	{	
		start= test->streams->next;
		
		while(1)
		{
			if(start->socket == sp->socket){
				
				prev->next = sp->next;				
				break;
			}
			
			if(start->next!=NULL){
				
				start=start->next;
				prev=prev->next;
			}
		}
	}
	sp->settings = NULL;
	free(sp->result);
	free(sp);
	   
}

struct iperf_stream *iperf_new_stream(struct iperf_test *testp)
{
	
	struct iperf_stream *sp;
	
    sp = (struct iperf_stream *) malloc(sizeof(struct iperf_stream));
    if(!sp)
	{
        perror("malloc");
        return(NULL);
    }
	
	
    memset(sp, 0, sizeof(struct iperf_stream));
	
	sp->settings = (struct iperf_settings *) malloc(sizeof(struct iperf_settings));
    memcpy(sp->settings, testp->default_settings, sizeof(struct iperf_settings));
	
	sp->result = (struct iperf_stream_result *) malloc(sizeof(struct iperf_stream_result));
	//memset(&sp->result, 0, sizeof(struct iperf_stream_result));
	
	sp->result->interval_results = (struct iperf_interval_results *) malloc(sizeof(struct iperf_interval_results));

    testp->remote_addr = (struct sockaddr_storage *) malloc(sizeof(struct sockaddr_storage));
	memset(testp->remote_addr, 0, sizeof(struct sockaddr_storage));

    testp->local_addr = (struct sockaddr_storage *) malloc(sizeof(struct sockaddr_storage));
	memset(testp->local_addr, 0, sizeof(struct sockaddr_storage));
		
	sp->socket = -1;
	
	sp->result->duration = testp->duration;
	sp->result->bytes_received = 0;
	sp->result->bytes_sent = 0;
	
	sp->result->interval_results->bytes_transferred = 0;
	
	return sp;
}

struct iperf_stream *iperf_new_tcp_stream(struct iperf_test *testp)
{	
	struct iperf_stream *sp;
		
	sp = (struct iperf_stream *) iperf_new_stream(testp);
    if(!sp) {
        perror("malloc");
        return(NULL);
    }
	
	sp->settings = testp->default_settings;

    sp->rcv = iperf_tcp_recv;
    sp->snd = iperf_tcp_send;
    //sp->update_stats = iperf_tcp_update_stats;
	
	return sp;		
}

struct iperf_stream * iperf_new_udp_stream(struct iperf_test *testp)
{	
	struct iperf_stream *sp;
		
	sp = (struct iperf_stream *) iperf_new_stream(testp);
    if(!sp) {
        perror("malloc");
        return(NULL);
    }
	
	sp->settings = testp->default_settings;
	
    sp->rcv = iperf_udp_recv;
    sp->snd = iperf_udp_send;
    //sp->update_stats = iperf_udp_update_stats;	
   
	return sp;
}

int iperf_udp_accept(struct iperf_test *test)
{
		
	struct iperf_stream *sp;
	struct sockaddr_in sa_peer;	
	char *buf;
	socklen_t len;
	int sz;
				
    buf = (char *) malloc(test->default_settings->bufsize);	
	len = sizeof sa_peer;
	
	sz = recvfrom(test->listener_sock, buf, test->default_settings->bufsize, 0, (struct sockaddr *) &sa_peer, &len);
		
	if(!sz)
		return -1;
		
	if(connect(test->listener_sock, (struct sockaddr *) &sa_peer, len) < 0)
	{
		perror("connect");
		return -1;
	}
	
	sp = test->new_stream(test);
	sp->socket = test->listener_sock;
	
	iperf_init_stream(sp, test);
	sp->result->bytes_received+= sz;
	iperf_add_stream(test, sp);	
		
	test->listener_sock = netannounce(test->protocol, NULL, test->listener_port);
	if(test->listener_sock < 0) 
		return -1;	
	
	
	FD_SET(test->listener_sock, &test->read_set);
	test->max_fd = (test->max_fd < test->listener_sock) ? test->listener_sock : test->max_fd;

	printf("%d socket created for new UDP stream \n", sp->socket);
	
	free(buf);
	
	return 0;
	
}	

int iperf_tcp_accept(struct iperf_test *test)
{
	socklen_t len;
	struct sockaddr_in addr;	
	int peersock;
		
	struct iperf_stream *sp;
	
	len = sizeof(addr);
	peersock = accept(test->listener_sock, (struct sockaddr *) &addr, &len);
	if (peersock < 0) 
	{
		printf("Error in accept(): %s\n", strerror(errno));
		return -1;
	}
	else 
	{	
		sp = test->new_stream(test);
		//setnonblocking(peersock);
		
		FD_SET(peersock, &test->read_set);
		test->max_fd = (test->max_fd < peersock) ? peersock : test->max_fd;
		
		sp->socket = peersock;		
		iperf_init_stream(sp, test);		
		iperf_add_stream(test, sp);					
		//connect_msg(sp);
		
		printf("%d socket created for new TCP client \n", sp->socket);
		
		return 0;
	}		
}

void iperf_init_stream(struct iperf_stream *sp, struct iperf_test *testp)
{
	socklen_t len;
	int x;
	len = sizeof(struct sockaddr_in);	
	
	sp->local_port = ((struct sockaddr_in *) (testp->local_addr))->sin_port;
	sp->remote_port = ((struct sockaddr_in *) (testp->remote_addr))->sin_port;
	
	sp->protocol = testp->protocol;
	
	if(getsockname(sp->socket, (struct sockaddr *) &sp->local_addr, &len) < 0) 
	{
        perror("getsockname");        
    }	
	
	//stores the socket id.
    if(getpeername(sp->socket, (struct sockaddr *) &sp->remote_addr, &len) < 0)
	{
        perror("getpeername");
        free(sp);        
    }
	
	if(set_tcp_windowsize(sp->socket, testp->default_settings->socket_bufsize, 
						  testp->role == 's' ? SO_RCVBUF : SO_SNDBUF) < 0)
        fprintf(stderr, "unable to set window size\n");
	
	
	x = getsock_tcp_windowsize(sp->socket, SO_RCVBUF);
    if(x < 0)
        perror("SO_RCVBUF");
	
   // printf("RCV: %d\n", x);
	
    x = getsock_tcp_windowsize(sp->socket, SO_SNDBUF);
    if(x < 0)
        perror("SO_SNDBUF");
	
   // printf("SND: %d\n", x);	
	
}

int iperf_add_stream(struct iperf_test *test, struct iperf_stream *sp)
{
	struct iperf_stream *n;
	
    if(!test->streams){
        test->streams = sp;
		return 1;
	}
    else {
        n = test->streams;
        while(n->next)
            n = n->next;
        n->next = sp;
		return 1;
    }
	
	return 0;
}

struct iperf_stream * update_stream(struct iperf_test *test, int j, int add)
{
	struct iperf_stream *n;
	n=test->streams;
		
	//find the correct stream for update
	while(1)
	{
		if(n->socket == j)
		{	
			n->result->bytes_received+= add;		
			break;
		}
		
		if(n->next==NULL)
			break;
		
		n = n->next;
	}
	
	return n;	
}

void iperf_run_server(struct iperf_test *test)
{
	printf("Server running now \n");
	struct timeval tv;
	struct iperf_stream *n;
	char ubuf[UNIT_LEN];
	int j;
	int result;
	
	FD_ZERO(&test->read_set);
	FD_SET(test->listener_sock, &test->read_set);
	
	test->max_fd = test->listener_sock;
	
	while(1)
	{	
		memcpy(&test->temp_set, &test->read_set,sizeof(test->temp_set));
		tv.tv_sec = 50;			
		tv.tv_usec = 0;
		
		// using select to check on multiple descriptors.
		result = select(test->max_fd + 1, &test->temp_set, NULL, NULL, &tv);
		
		if (result == 0) 
		{
			printf("select() timed out!\n");
			break;
		}
		else if (result < 0 && errno != EINTR)
			printf("Error in select(): %s\n", strerror(errno));
		
		// Accept a new connection
		else if(result >0)
		{			
			if (FD_ISSET(test->listener_sock, &test->temp_set))
			{			
				test->accept(test);
				
				FD_CLR(test->listener_sock, &test->temp_set);								
			}				
			
		// test end message ?
		
		//result_request message ?		
		
		//Process the sockets for read operation
			for (j=0; j< test->max_fd+1; j++)
			{				
				if (FD_ISSET(j, &test->temp_set))
				{					
					// find the correct stream
					n = update_stream(test,j,0);
					result = n->rcv(n);
				
					if(result == 0)
					{	
						// stream shutdown message
						unit_snprintf(ubuf, UNIT_LEN, (double) (n->result->bytes_received / n->result->duration), 'a');
						printf("%llu bytes received %s/sec for stream %d\n\n",n->result->bytes_received, ubuf,(int)n);						
						close(j);						
						iperf_free_stream(test, n);
						FD_CLR(j, &test->read_set);						
					}
					else if(result < 0)
					{
						printf("Error in recv(): %s\n", strerror(errno));
					}
				}      // end if (FD_ISSET(j, &temp_set))
			
			}// end for (j=0;...)
		}// end else (result>0)
		
	}// end while
	
	printf(" test ended\n");
}

void iperf_run_client(struct iperf_test *test)
{
	int i,result;
    struct iperf_stream *sp, *np;
    struct timer *timer, *interval;
	char *buf;
	int64_t delayns, adjustns, dtargns; 
    struct timeval before, after;
	struct timeval tv;
	int ret=0;
	tv.tv_sec = 15;			// timeout interval in seconds
	tv.tv_usec = 0;
	
	buf = (char *) malloc(test->default_settings->bufsize);
			
	if (test->protocol == Pudp)
	{
		dtargns = (int64_t)(test->default_settings->bufsize) * SEC_TO_NS * 8;
		dtargns /= test->default_settings->rate;
		
		assert(dtargns != 0);
		
		if(gettimeofday(&before, 0) < 0) {
			perror("gettimeofday");
		}
		
		delayns = dtargns;
		adjustns = 0;
		printf("%lld adj %lld delay\n", adjustns, delayns);			
	}	
	
    timer = new_timer(test->duration, 0);
	
	if(test->stats_interval != 0)
	interval = new_timer(test->stats_interval,0);
	
	//Display();
	// send data till the timer expires
    while(!timer->expired(timer))
	{
		ret = select(test->max_fd+1, NULL, &test->write_set, NULL, &tv);
		if(ret<0)
			continue;
		
		sp= test->streams;	
		
		for(i=0;i<test->num_streams;i++)
		{
			if(FD_ISSET(sp->socket, &test->write_set))
			{
				result = sp->snd(sp);
				
				if (test->protocol == Pudp)
				{
					if(delayns > 0)
						delay(delayns);
					
					if(gettimeofday(&after, 0) < 0)
						perror("gettimeofday");
										
					adjustns = dtargns;
					adjustns += (before.tv_sec - after.tv_sec) * SEC_TO_NS;
					adjustns += (before.tv_usec - after.tv_usec) * uS_TO_NS;
					
					if( adjustns > 0 || delayns > 0) {
						//printf("%lld adj %lld delay\n", adjustns, delayns);
						delayns += adjustns;
					}
					memcpy(&before, &after, sizeof before);
				}
				
				if(sp->next==NULL)
					break;
				sp=sp->next;
				
			}// FD_ISSET
		}// for
		
		if((test->stats_interval!=0) && interval->expired(interval))
		{
			// call the reporter for interval
			test->stats_callback(test);
			//reset the interval timer
			interval = new_timer(test->stats_interval,0);
		}
		
	}// while outer timer
	
	Display(test);	
    /* XXX: report */
    sp = test->streams;
	np = sp;
	
    do {
		sp = np;
		send(sp->socket, buf, 0, 0);
		printf("%llu bytes sent\n", sp->result->bytes_sent);
		np = sp->next;	
		close(sp->socket);
		iperf_free_stream(test, sp);
				
    } while (np);	
				
}	

int iperf_run(struct iperf_test *test)
{
			
	if(test->role == 's')
	{
		iperf_run_server(test);
		return 0;
	}
				
			
	else if ( test->role == 'c')
	{
		iperf_run_client(test);
		return 0;
	}
		
	return -1;
}

int
main(int argc, char **argv)
{
	char ch;
    struct iperf_test *test;
	struct sockaddr_in *addr_local, *addr_remote;
			
	addr_local = (struct sockaddr_in *)malloc(sizeof (struct sockaddr_in));
	addr_remote = (struct sockaddr_in *)malloc(sizeof (struct sockaddr_in));
	
	
	test= iperf_new_test();
	
	// need to set the defaults for default_settings too
	iperf_defaults(test);
		
	while( (ch = getopt_long(argc, argv, "c:p:st:uP:b:l:w:i:", longopts, NULL)) != -1 )
        switch (ch) {
            case 'c':
                test->role = 'c';	
				//remote_addr
				inet_pton(AF_INET, optarg, &addr_remote->sin_addr);
				addr_remote->sin_port = htons(5001);
                break;
				
            case 'p':
                addr_remote->sin_port = htons(atoi(optarg));
                break;
            case 's':
				test->role = 's';	
				addr_local->sin_port = htons(5001);
				test->listener_port = 5001;
                break;
            case 't':
                test->duration = atoi(optarg);
                break;
            case 'u':
                test->protocol = Pudp;
				test->default_settings->bufsize = DEFAULT_UDP_BUFSIZE;
                break;
            case 'P':
                test->num_streams = atoi(optarg);
                break;
            case 'b':
				test->default_settings->rate = atoi(optarg);
                break;
            case 'l':
                test->default_settings->bufsize = atol(optarg);
                break;
            case 'w':
                test->default_settings->socket_bufsize = atoi(optarg);
			case 'i':
				test->stats_interval = atoi(optarg);
				test->reporter_interval = atoi(optarg);
                break;
        }
	
	printf("role = %s\n", (test->role == 's') ? "Server" : "Client");
	printf("duration = %d\n", test->duration);
	printf("protocol = %s\n", (test->protocol == Ptcp) ? "TCP" : "UDP");
	printf("interval = %d\n", test->stats_interval);	
	printf("Parallel streams = %d\n", test->num_streams);
	
	
	test->local_addr = (struct sockaddr_storage *) addr_local;	
	test->remote_addr = (struct sockaddr_storage *) addr_remote;	
	test->stats_callback = iperf_stats_callback;	

    switch(test->protocol)
    {
        case Ptcp:
            test->accept = iperf_tcp_accept;
            test->new_stream = iperf_new_tcp_stream;

            break;
        case Pudp:
            test->accept = iperf_udp_accept;
            test->new_stream = iperf_new_udp_stream;
            break;
    }
	
	iperf_init_test(test);
	
	iperf_run(test);
	
	return 0;
}