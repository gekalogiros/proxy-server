#include <limits.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <poll.h>
#include "utils.h"

#define MAX_REQS      100000
#define MAX_HEADER     20480
#define MAX_LINE        2560
#define BSIZ            8192
#define GET                0
#define POST               1
#define NO_DESCRIPTOR     -1

char *methods[2] = {"GET", "POST"};

// mutex used for maintain the order of the requests
pthread_mutex_t checkOrder = PTHREAD_MUTEX_INITIALIZER;
// mutex used when i create client threads
pthread_mutex_t  accessCLI = PTHREAD_MUTEX_INITIALIZER;

struct req{
	pthread_t reqThread;         // This holds the id of the thread
	pthread_cond_t cond;         // This holds the id of condidional var
	int  requestID;              // The Index of the request
	int  adaptedID;		     // the index of the request in the array
	int  socketDescriptor;       // The id of the socket
	int  method;		     // POST or GET
	int  portNumber;             // The port number	
	int  served;		     // Handles the state of the request
	int *breakCliConn;	     // Used for Closing Persistent connection
	char reqhead[MAX_HEADER];    // The header of the request
	char hostName[MAX_HEADER];   // The host name
	char pathName[MAX_HEADER];   // The path Name

	struct req *arrayAddress;    // address of head position of the array
};

typedef struct req REQUEST;
typedef struct arg ARGUMENTS;

int contentlength(char *header)
{
  int len=INT_MAX;
  char line[MAX_LINE];

  if (HTTPheadervalue_case(header, "Content-Length", line))
    sscanf(line, "%d", &len);
  return len;
}

void parserequest(char *request, int *method, char *host, int *pn, char *path)
{
	char url[MAX_LINE] = "";
	char m[MAX_LINE] = "";
	char hostport[MAX_LINE] = "";
	char port[MAX_LINE] = "";
	int i;

	sscanf(request, "%[^ ] %[^ ] HTTP/1.1", m, url);
	for (i=0; i<2; i++)
	{
		if (strcmp(m, methods[i]) == 0)
		*method = i;
	}

	sscanf(url, "http://%[^\n\r/]%[^\n\r]", hostport, path);
	sscanf(hostport, "%[^:]:%[^\n\r]", host, port);

	if (*port == '\0')
	{
		*pn = 80;
	}
	else
	{
		*pn = atoi(port);
	}
}

int copydata(int from, int to, int len)
{
	char tbuff[BSIZ];
	int n;

	while (len > 0) 
	{
		if ((n = read(from, tbuff, BSIZ)) <= 0)
		{
			return -1;
		}
		if (write(to, tbuff, n) < n)
		{
			return -1;
		}
		len -= n;
	}
	return 0;	
}


int closeClientConn(char *reqHeader, char *resHeader){

	char value[MAX_HEADER];

	bool closeConn1 = false;
	bool closeConn2 = false;
        bool closeConn;

	
	if ( HTTPheadervalue_case(resHeader, "Content-Length", value) == 0 )
	{		
		closeConn1 = true;
	}
	
	if ( HTTPheadervalue_case(reqHeader, "Connection: ", value) == 1)
	{
		if ( strcmp(value, "close") == 0 || strcmp(value, "close\r") == 0 )
		{
			closeConn2 = true;
		}
	}
	
	closeConn = (closeConn1 || closeConn2);

	return (int)closeConn;
}

void *processRequest(void *args){

	char resline[MAX_LINE];
	char reqhead[MAX_HEADER], reqhead1[MAX_HEADER], reshead[MAX_HEADER];
	char host[MAX_LINE], path[MAX_LINE];	
	int srv, v, status;
	
	// currentReq is the struct which contains details for this request
	REQUEST currentReq=*(REQUEST*)args;
	// arrayHead is the address of the first position of the requests array
	REQUEST *arrayHead = currentReq.arrayAddress;
	
	int   reqID = currentReq.requestID;
	int adaptID = currentReq.adaptedID;
	int     cli = currentReq.socketDescriptor;
	int  method = currentReq.method;
	int  portno = currentReq.portNumber;
	strcpy(reqhead, currentReq.reqhead);
	strcpy(host, currentReq.hostName);
	strcpy(path, currentReq.pathName);
	printf("%s", reqhead);
	 
	if ((srv = activesocket(host, portno)) < 0) 
	{
		sprintf(reshead, "HTTP/1.1 503\r\nContent-Length: 12\r\nConnection: close\r\n\r\nNon-existent");
		if ( write(cli, reshead, strlen(reshead)) == -1 )
		{
			printf("Asynchronous closed by the client\n");
			//close the connection
			if (adaptID==MAX_REQS-1){
			pthread_cond_signal(&(arrayHead[0].cond));		
			}else{
				pthread_cond_signal(&(arrayHead[adaptID+1].cond));
			}
			*(arrayHead[adaptID].breakCliConn) = 1;
			return NULL;
		}
	}
	else 
	{
		sprintf(reqhead1, "%s %s HTTP/1.1\r\n", methods[method], path);
		strcat(reqhead1, "Connection: close\r\n");
		strcat(reqhead1, reqhead);
		write(srv, reqhead1, strlen(reqhead1));
		printf("%s", reqhead1);

		if (method == POST)
		{
			if ( copydata(cli, srv, contentlength(reqhead)) < 0 )
			{
				printf("Asynchronous closed by the client\n");
				//close connection
				if (adaptID==MAX_REQS-1){
					pthread_cond_signal(&(arrayHead[0].cond));		
				}else{
					pthread_cond_signal(&(arrayHead[adaptID+1].cond));
				}
				close(srv);
				*(arrayHead[adaptID].breakCliConn) = 1;
				return NULL;

			}
		}

		TCPreadline(srv, resline, MAX_LINE);
		sscanf(resline, "HTTP/1.%d %d ", &v, &status);		
		HTTPreadheader(srv, reshead, MAX_HEADER);
		HTTPheaderremove_case(reshead, "Connection");

		pthread_mutex_lock(&checkOrder);	
		/*************************************************************
		 *          Each request is represented by a struct.         *  
		 *            All structs are stored in an array.            *
		 *       "arrayHead" is the first position of this array.    *
		 *              We Set to wait all the requests              *
		 *    (apart from the first one)so that they follow a FIFO   *
		 *    logic. The first request is going to be served first   *
		 *************************************************************/
		// We decide if the struct which is in the first position of the
		// array is the first request of the client or the size of the 
		// array has been overcame. 
		if( reqID==0 && (arrayHead[MAX_REQS-1].served) == 0 && reqID>MAX_REQS)
		{
			pthread_cond_wait( &(arrayHead[adaptID].cond), &checkOrder);
		}
		// if its not the request stored in the first position, set it 
		// to wait mode directly.
		else if( reqID!=0 && (arrayHead[adaptID-1].served)==0)
		{	
			pthread_cond_wait(&(arrayHead[adaptID].cond), &checkOrder);	
		}		
		pthread_mutex_unlock(&checkOrder);

		// Execute write and check for errors.
		if ( write(cli, resline, strlen(resline)) == -1 )
		{
			printf("Asynchronous closed by the client.\n");
			// close connection
			if (adaptID==MAX_REQS-1){
			pthread_cond_signal(&(arrayHead[0].cond));		
			}else{
				pthread_cond_signal(&(arrayHead[adaptID+1].cond));
			}
			close(srv);
			*(arrayHead[adaptID].breakCliConn) = 1;
			return NULL;
		}			
		printf("%s", resline);

		/*******************************************************
		*            PERSISTENT CONNECTION - PART 2            *
		*  CHECK IF THE PERSISTENT CONNECTION SHOULD BE CUT.   *
		*            WE DISABLE THE CONNECTION WHEN            *
		* A. THE CLIENT RECEIVES A CONNECTION CLOSE STATEMENT  *
		* B. THE RESPONSE HAS NOT A SELF-DEFINED LENGTH        *
		*THESE CHECKS ARE MADE IN THE closeClientConn function *
		*******************************************************/ 
		if ( closeClientConn(reqhead, reshead) == 1)
		{
			strcpy(resline, "Connection: close\r\n");
			// close connection
			*(arrayHead[adaptID].breakCliConn) = 1;
		}
		else
		{
			strcpy(resline, "Connection: keep-alive\r\n");
		}				
		
		if ( write(cli, resline, strlen(resline)) == -1)
		{
			printf("Asynchronous closed by the client\n");
			// close connection
			if (adaptID==MAX_REQS-1){
			pthread_cond_signal(&(arrayHead[0].cond));		
			}else{
				pthread_cond_signal(&(arrayHead[adaptID+1].cond));
			}
			close(srv);
			*(arrayHead[adaptID].breakCliConn) = 1;
			return NULL;
		}
		if( write(cli, reshead, strlen(reshead)) == -1)
		{
			printf("Asynchronous closed by the client\n");
			// close connection
			if (adaptID==MAX_REQS-1){
			pthread_cond_signal(&(arrayHead[0].cond));		
			}else{
				pthread_cond_signal(&(arrayHead[adaptID+1].cond));
			}
			close(srv);
			*(arrayHead[adaptID].breakCliConn) = 1;
			return NULL;
		}
		printf("%s%s", resline, reshead);

		if (status != 204 && status != 304)
		{
			// Execute copydata and check for errors.
			if ( copydata(srv, cli, contentlength(reshead)) < 0 ){
				printf("Asynchronous closed by the client.\n");
				// close Connection
				if (adaptID==MAX_REQS-1){
				pthread_cond_signal(&(arrayHead[0].cond));		
				}else{
					pthread_cond_signal(&(arrayHead[adaptID+1].cond));
				}
				close(srv);
				*(arrayHead[adaptID].breakCliConn) = 1;

				return NULL;				
			}
		}
		close(srv);
		// This request has been served.
		arrayHead[adaptID].served = 1;
		pthread_mutex_lock(&checkOrder);		
		/*****************************************************
		*         MAINTAIN THE ORDER OF THE REQUESTS         *
		*             WHEN A REQUEST GETS SERVED,            *
		*      IT SIGNALS THE NEXT REQUEST IN THE QUEUE!     *
		******************************************************/

		/* Check if this request is stored in the last position
		   of the array. If yes, then signal the request that is
		   stored in the first position of the array */
		if (adaptID==MAX_REQS-1)
		{
			// Signal the conditional variable of the next thread
			// that waits to be served.
			pthread_cond_signal(&(arrayHead[0].cond));		
		}
		else
		{
			// Signal the conditional variable of the next thread
			// that waits to be served.
			pthread_cond_signal(&(arrayHead[adaptID+1].cond));
		}
		pthread_mutex_unlock(&checkOrder);
	}
	return NULL;
}

void *serviceconn(void *socketDescriptor)
{
	int cli = *(int*)socketDescriptor;
	char reqline[MAX_LINE];
	char reqhead[MAX_HEADER];
	char host[MAX_LINE], path[MAX_LINE];	

	int closeCliConn = 0, requestIndex=0;
	int method, portno, adaptedIndex;	

	/**********************************************************
	*                CREATE the reqThreadArray                *
	* THIS ARRAY CONTAINS ONE REQUEST STRUCT IN EACH POSITION *
	* 
	***********************************************************/
        REQUEST *reqThreadArray;
	reqThreadArray=(REQUEST*)malloc(sizeof(REQUEST)*MAX_REQS);	

	/**********************************************************
	*   MULTIPLEXING INPUT/OUTPUT  OVER SOCKET DESCRIPTOR     *
	**********************************************************/
	// this variables holds the timeout time in msec.
	int timeOutSecs=10000;

	struct pollfd fds[1];
	// Opens the stream device, it is a socket in our case.
	fds[0].fd  = cli;
	fds[0].events = POLLRDNORM | POLLRDBAND;

	/**********************************************************
	*           PERSISTENT CLIENT CONNECTION - PART 2         *
	***********************************************************/
	while(closeCliConn == 0)
	{
		/**********************************************************
		*                   TIMEOUT - PART 4                      *
		*         CHECK THE RESULT OF THE POLL FUNCTION           *
		* if poll() <= 0 Timeout or error in the function occured *
		*  For the both of the situations i print a timeout error *
		**********************************************************/
		if ( poll(fds, 1, timeOutSecs) <= 0 )
		{
			printf("Connection Timeout\n");
			break;
		}
		/***********************************************************
		 *             ASYNCHRONOUS CLOSING - PART 3               *
		 *     MONITORING THE RESULT OF TCPreadline FUNCTION       *
		 *          IF IT RETURNS <=0 AN ERROR OCCURED.            *
		 *     CLOSE THE CURRENT CONNECTION WHEN ERROR OCCURS      *
		 **********************************************************/
		if ( TCPreadline(cli, reqline, MAX_LINE) == 0 )
		{
			printf("Asynchronous closed by the client\n");
			break;
		}
		parserequest(reqline, &method, host, &portno, path);
		/**********************************************************
		*             ASYNCHRONOUS CLOSING - PART 3               *
		*    MONITORING THE RESULT OF HTTPreadheader FUNCTION     *
		*          IF IT RETURNS <=0 AN ERROR OCCURED.            *
		*     CLOSE THE CURRENT CONNECTION WHEN ERROR OCCURS      *
		**********************************************************/
		if (HTTPreadheader(cli, reqhead, MAX_HEADER) == 0)
		{
			printf("Asynchronous closed by the client\n");
			break;
		}	
		printf("%s%s", reqline, reqhead);
		HTTPheaderremove_case(reqhead, "Connection");
		/******************************************************
		 *             PIPELINE REQUESTS - PART 5
		 *           HERE I PIPELINE THE REQUESTS             *
		 * I INITIALISE A STRUCT WITH THE APPROPRIATE DETAILS *
		 *     AND START A NEW THREAD WITH THIS STRUCT AS     * 
		 *           ARGUMENT. EACH NEW REQUEST               *
		 *              IS HANDLED CONCCURENTLY.      	      *
		 * STORE THE STRUCT OF EACH REQUEST IN THE ARRAY req. *
		 ******************************************************/
		/* By using the adaptedIndex, each time the array req
		   is full, the index counter starts from the
		   beggining. So the problem of the fixed Length of the
		   req array is solved. */
		adaptedIndex=requestIndex%MAX_REQS;
		/****************************************************
		 *               FILL IN THE STRUCT WITH            *
 		 *         THE DETAILS OF THE CURRENT REQUEST       *
		 ****************************************************/ 
		// The actual number of the request
		reqThreadArray[adaptedIndex].requestID=requestIndex;
		// The index of this request in the array
		// adaptedIndex=requestIndex % MAX_REQS
		reqThreadArray[adaptedIndex].adaptedID=adaptedIndex;
		// The id of the socket
		reqThreadArray[adaptedIndex].socketDescriptor=cli;
		// method is GET OR POST
		reqThreadArray[adaptedIndex].method=method;
		// I initialize the state to NOT SERVED
		reqThreadArray[adaptedIndex].served=0;
		// Port Number
		reqThreadArray[adaptedIndex].portNumber=portno;
		/* This is the address of the closeCliConn
		   I use this address  to break the persistent connection 
		   or to break the connection in case of an asynchronous
		   closing. */ 
		reqThreadArray[adaptedIndex].breakCliConn=&closeCliConn;
		/* arrayAddress contains the address of the first position
		   of the reqThreadArray array. */
		reqThreadArray[adaptedIndex].arrayAddress=reqThreadArray;
		// initialization the conditional variable of this request
		// this is used in order to maintain the order of the requests.
		pthread_cond_init( &(reqThreadArray[adaptedIndex].cond), NULL);

		strcpy(reqThreadArray[adaptedIndex].reqhead, reqhead);
		strcpy(reqThreadArray[adaptedIndex].hostName, host);
		strcpy(reqThreadArray[adaptedIndex].pathName, path);
		// Creation of a new thread in order to handle the request.
		pthread_create(&(reqThreadArray[adaptedIndex].reqThread), NULL, processRequest, &reqThreadArray[adaptedIndex]);
		printf("I created the %d request in order to serve the client %d \n", requestIndex, cli);
		requestIndex++;				
	}
	close(cli);
	return 0;
}

int main(int argc, char *argv[])
{
	int socketDescriptor;
	/*********************************************************
	* THE SERVER OPENS A PASSIVE SOCKET ATTACHED ON THE PORT *
	*                DEFINED BY THE USER.                    *
	**********************************************************/
	int s = passivesocket(atoi(argv[1]));
	signal(SIGPIPE, SIG_IGN);

	while(1){
		socketDescriptor= acceptconnection(s);
		/**********************************************************
		*        CREATE A THREAD FOR EACH CLIENT - PART 1         *
		***********************************************************/
		if (socketDescriptor != NO_DESCRIPTOR)
		{
			pthread_t *client;
			client=(pthread_t*)malloc(sizeof(pthread_t));
			pthread_mutex_lock(&accessCLI);
			pthread_create(client, NULL, serviceconn, &socketDescriptor);
			pthread_mutex_unlock(&accessCLI);
		}
	}
}
