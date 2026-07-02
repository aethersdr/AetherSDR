///*    \file traffic_cop.c
// *    \brief TCP Communications Server
// *
// *    \copyright  Copyright 2011-2013 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 31-AUG-2014
// *    \author Stephen Hicks, N5AC
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2014 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/tcp.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <ifaddrs.h>
#include <sys/prctl.h>

#include "common.h"
#include "traffic_cop.h"
#include "status_processor.h"

static pthread_t _tc_thread_id;
static pthread_t _keepalive_thread_id;

static __thread receive_data __local;
//! address of the host to connect to -- defaults to 127.0.0.1
//! but this is generally provided by discovery
static char _hostname[32] = "127.0.0.1";
static char _api_port[32] = SMARTSDR_API_PORT;

//const char* gai_strerror(int ecode);
static BOOL _abort = FALSE;
static int _socket;
static BOOL _abort_keepalive = FALSE;
static const char* _abort_reason = "unspecified";
static uint32 _sequence = 0;
static Command _root;
static pthread_mutex_t _commandList_mutex;

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

static void _tc_openSocket(void)
{
    struct addrinfo hints, *servinfo, *p;
    int rv;
    char s[INET6_ADDRSTRLEN];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(_hostname, SMARTSDR_API_PORT, &hints, &servinfo)) != 0) {
        output(ANSI_RED "getaddrinfo: %s\n", gai_strerror(rv));
        // this is fatal
        exit(1);
    }

    // loop through all the results and connect to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((_socket = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            output(ANSI_RED "Traffic Cop: socket error\n");
            continue;
        }

        if (connect(_socket, p->ai_addr, p->ai_addrlen) == -1) {
            close(_socket);
            output(ANSI_RED "Traffic Cop: connect error\n");
            continue;
        }

        break;
    }

    if (p == NULL) {
        output(ANSI_RED "Traffic Cop: failed to connect\n");
        exit(2);
    }

    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
            s, sizeof s);
    output(ANSI_GREEN "Traffic Cop: connecting to %s\n", s);

    freeaddrinfo(servinfo); // all done with this structure
}

static BOOL _have_terminate_char(void)
{
    if (__local.recv_buf[0] == 3) return TRUE;  // ctrl-c
    if (__local.recv_buf[0] == 4) return TRUE;  // ctrl-d
    if (__local.recv_buf[0] == 26) return TRUE; // ctrl-z
    return FALSE;
}

static void _eat_characters(int n)
{
    //output("eating %u characters\n",n);
    if (n >= __local.buf_len)
    {
        // if we are trying to eat more than is in the buffer, just clear the buffer
        memset(__local.recv_buf,  0, n+1);
        //output("zeroing buffer\n");
        __local.read_ptr = __local.recv_buf;
        __local.buf_len = 0;
    }
    else if (n > 0)
    {
        char* to = __local.recv_buf;
        char* from = __local.recv_buf + n;

        // move the rest of the buffer over the stuff we are eating
        int i;
        for (i = 0; i < (__local.buf_len - n); i++)
        {
            //output("moving a 0x%02X '%c'\n",*from,*from);
            *to++ = *from;
            *from++ = 0;
        }
        // correct length to show we ate some characters
        __local.buf_len -= n;
        //output("new buf len = %u\n",__local.buf_len);
        // set where we will read in the next data
        __local.read_ptr = (__local.recv_buf + __local.buf_len);
        //output("new read ptr = 0x%08X\n",__local.read_ptr);
        //output("next character = 0x%02X '%c'\n",*__local.recv_buf, *__local.recv_buf);
    }
}

static void _eat_crlf(void)
{
    BOOL done_eating = FALSE;
    // check for tasty morsels
    while (!done_eating)
    {
        // don't overeat -- it's bad for you
        if (__local.buf_len == 0)
        {
            done_eating = TRUE;
        }
        else if (__local.recv_buf[0] == '\n' || __local.recv_buf[0] == '\r' || __local.recv_buf[0] == 0x0)
        {
            _eat_characters(1);
        }
        else
        {
            done_eating = TRUE;
        }
    }
}

static void _skip_esc_sequences(void)
{
    // is the first character a telnet escape character (see RFC 2877)
    while (__local.recv_buf[0] == 0xFF)
    {
        _eat_characters(3);
    }
}

static BOOL _check_for_timeout(void)
{
    // if keepalive is not turned on, just say everything is AOK
    if (!__local.keepalive_enabled) return TRUE;
    // bail if we're uninitialized
    if (__local.last_ping.tv_sec == 0) return TRUE;

    // find out how long it has been since the last ping
    uint32 since = usSince(__local.last_ping);

    // 500% margin -- we should get a ping once per second
    if (since > 5000000) return FALSE;
    return TRUE;
}

static uint32 _read_more_data(void)
{
    int read_len, errsv;

    BOOL done = FALSE;
    // loop ignoring any timeout errors that occur
    while (!done)
    {
        read_len = recv(_socket, __local.read_ptr, RECV_BUF_SIZE_TO_GET, 0);
        errsv = errno;
        // if read_len is zero and er got EAGAIN, it means the client closed the socket
        if ( read_len == 0 && (errsv == EAGAIN || errsv == EPIPE || errsv == EBADF ) )
        {
//          debug(LOG_DEV, TRUE, "socket error: read_len = %d, errno = %d",read_len,errsv);
            return SL_CLOSE_CLIENT;
        }

        // if we got new text from the socket OR we have an error other than timeout, exit the loop
        if (read_len > 0) done = TRUE;
        // EINTR seems to get asserted with breakpoint adds when debugging, so let's just
        // read again if we see that too
        if (read_len == -1 && errsv != EAGAIN && errsv != EINTR)
        {
//          debug(LOG_DEV, TRUE, "socket error: read_len = %d, errno = %d",read_len,errsv);
            return SL_CLOSE_CLIENT;
        }
        // see if we have keepalive enabled and if so and we've not received a ping, terminate
        // this client
        if (!_check_for_timeout())
        {
            output(ANSI_RED "\nTraffic Cop has failed keepalive test; terminating\n");
//            hal_listen_abort = TRUE;
        }

        // if someone wants to shutdown the system, we will terminate this thread
        if (_abort) return SL_TERMINATE;
    }

    if (read_len <= 0 || read_len > RECV_BUF_SIZE_TO_GET)
    {
        // ok we're done here -- time to go
        return SL_CLOSE_CLIENT;
    }
    else
    {
        __local.buf_len += read_len;
        //output("total text length = %u\n",__local.buf_len);
        return SUCCESS;
    }
}

static uint32 _get_string_len(void)
{
    uint32 len;
    BOOL found_terminator = FALSE;
    for (len = 0; len < __local.buf_len; len++)
    {
        if (*(__local.recv_buf+len) == '\r' ||
            *(__local.recv_buf+len) == '\n' ||
            *(__local.recv_buf+len) == 0)
        {
            found_terminator = TRUE;
            break;
        }
    }
    if (found_terminator)
    {
        return len;
    }
    else
    {
        // prevent tight CPU loops waiting on data
        usleep(500);
        return 0;
    }
}

static uint32 _get_command(void)
{
    BOOL got_command = FALSE;
    while (!got_command && !_abort)
    {
        // if there something in the buffer
        if (__local.buf_len != 0)
        {
            if (_have_terminate_char()) return SL_TERMINATE;
            _skip_esc_sequences();
            _eat_crlf();

            uint32 len = _get_string_len();
            if (len != 0)
            {

                __local.command = (char*)safe_malloc(len+1);
                if(!__local.command)
                {
                    //output("Error allocating command (size=%u)\n", len+1);
                    __local.command = NULL;
                    return SL_OUT_OF_MEMORY;
                }

                // clear the newly allocated memory
                memset(__local.command, 0, len+1);

                memcpy(__local.command, __local.recv_buf, len);

                _eat_characters(len);
                got_command = TRUE;
            }
        }
        if (!got_command)
        {
            uint32 ret_val = _read_more_data();
            if (ret_val != SUCCESS) return ret_val;
        }
    }
    return SUCCESS;
}

void process_status(char* string)
{
#ifdef DEBUG
    output(ANSI_GREEN "Traffic Cop: received \033[m'%s'\n",string);
#endif
    status_processor(string);
}


//! main traffic cop receiver thread
static void* _tc_thread(void* arg)
{
    uint32 result;

    prctl(PR_SET_NAME, "DV-TrafficCop");

    memset(&__local, 0, sizeof(receive_data));
    __local.last_ping.tv_sec = 0;
    __local.recv_buf = safe_malloc(RECV_BUF_SIZE);
    memset(__local.recv_buf, 0, RECV_BUF_SIZE);
    __local.buf_len = RECV_BUF_SIZE;

    // make a connection to SmartSDR
    // if this fails, the program just exits
    _tc_openSocket();

    result = register_mode();
    if (result != SUCCESS)  {
		output("** Could not register mode **\n");
        _abort_reason = "register mode failed";
		tc_abort();
    }

    tc_sendSmartSDRcommand("sub slice all", FALSE, NULL);

    /* Initialize UDP connections for TX */
    vita_output_Init(_hostname);

    tc_startKeepalive();

    // loop receiving data from SmartSDR and sending it where it should go
    while (!_abort)
    {
        result = _get_command();
        if (result == SUCCESS)
        {
            if(__local.command != NULL)
            {
                process_status(__local.command);
                safe_free(__local.command);
                __local.command = NULL;
            }
        }
        else if (result == SL_TERMINATE)
        {
            output("Traffic Cop: terminate requested by byte 0x%02X\n",
                   (unsigned char)__local.recv_buf[0]);
            _abort = TRUE;
            // close(client->sd); -- it's actually closed after the end: label
            //debug(LOG_DEV,TRUE,"Client asked to close connection with a termination character");
        }
        else if (result == SL_CLOSE_CLIENT)
        {
            output("Traffic Cop: radio closed TCP API socket\n");
            _abort = TRUE;
            //close(client->sd); -- it's actually closed after the end: label
            //debug(LOG_DEV,TRUE,"An error on the port has forced the client to close");
        }
    }

    close(_socket);
    safe_free(__local.recv_buf);

    return NULL;
}

void _commandList_LinkHead(Command cmd)
{
    pthread_mutex_lock(&_commandList_mutex);
    cmd->prev = _root;
    cmd->next = _root->next;
    _root->next->prev = cmd;
    _root->next = cmd;
    pthread_mutex_unlock(&_commandList_mutex);
}

void _commandList_LinkTail(Command cmd)
{
    pthread_mutex_lock(&_commandList_mutex);
    cmd->next = _root;
    cmd->prev = _root->prev;
    _root->prev->next = cmd;
    _root->prev = cmd;
    pthread_mutex_unlock(&_commandList_mutex);
}

void _commandList_Unlink(Command cmd)
{
    // list should already be locked with entering!
    // ensure not root
    if(cmd == _root) return;

    // make sure cmd exists and is actually linked
    if(!cmd || !cmd->prev || !cmd->next) return;

    cmd->next->prev = cmd->prev;
    cmd->prev->next = cmd->next;
    cmd->next = NULL;
    cmd->prev = NULL;
}

void _commandList_Init()
{
    _root = (Command)safe_malloc(sizeof(command_type));
    memset(_root, 0, sizeof(command_type));
    _root->next = _root;
    _root->prev = _root;
    pthread_mutex_init(&_commandList_mutex, NULL);
}

Command tc_commandList_respond(uint32 sequence, char* response)
{
#ifdef DEBUG
    output("response for %d: '%s'\n",sequence,response);
#endif
    pthread_mutex_lock(&_commandList_mutex);
    Command iterator = _root;
    while(iterator->next != _root)
    {
        iterator = iterator->next;
        if (iterator->sequence != sequence) continue;

        uint32 len = strlen(response);
        char* resp = safe_malloc(len+1);
        strncpy(resp, response, len+1);
        iterator->response = resp;

#ifdef DEBUG
        // let the thread blocking on this know that we now have it
        output("posting %d...\n",sequence);
#endif
        sem_post(&iterator->semaphore);
        break;
    }
    pthread_mutex_unlock(&_commandList_mutex);
    return NULL;
}

char* _tc_commandList_getResponse(uint32 sequence)
{
    pthread_mutex_lock(&_commandList_mutex);
    Command iterator = _root;
    while(iterator->next != _root)
    {
        iterator = iterator->next;
        if (iterator->sequence != sequence) continue;

        pthread_mutex_unlock(&_commandList_mutex);
        sem_wait(&iterator->semaphore);
#ifdef DEBUG
        output("received post %d...\n",sequence);
#endif
        sem_destroy(&iterator->semaphore);
        _commandList_Unlink(iterator);
        char* response = iterator->response;
        free(iterator);
        return response;
    }
    pthread_mutex_unlock(&_commandList_mutex);
    return NULL;
}

static void _tc_commandList_add(uint32 sequence)
{
    Command cmd = safe_malloc(sizeof(command_type));
    memset(cmd, 0, sizeof(command_type));
    sem_init(&cmd->semaphore,0,0);
    cmd->sequence = sequence;
    _commandList_LinkTail(cmd);
}

static uint32 _sendAPIcommand(char* command, uint32* sequence, BOOL block)
{
    int result;
    uint32 ret_val;

    char* mpointer = NULL;
    char* message = safe_malloc(MAX_API_COMMAND_SIZE);
    memset(message, 0, MAX_API_COMMAND_SIZE);

    _sequence++;
    *sequence = _sequence;

    if (block) _tc_commandList_add(*sequence);

    // first, we need to put the status header on the front of the string
    int len = snprintf(message, MAX_API_COMMAND_SIZE, "C%d|", *sequence);
    mpointer = message + len;

    len += strlen(command) + 1;

    strncat(mpointer, command, MAX_API_COMMAND_SIZE);
    strncat(mpointer, "\n", MAX_API_COMMAND_SIZE);

    errno = 0;
    result = write(_socket, message, len);
    *(message+len-1) = 0;
    // output what we're sending as long as it is not a ping
    // if (strstr(message, "ping") == 0)
        //output(ANSI_GREEN "-> SmartSDR: \033[33m%s\033[m\n",command);
    if (result == len)
    {
        ret_val = SUCCESS;
    }
    else
    {
        output(ANSI_RED "Traffic Cop: error writing to TCP API socket: %s\n",strerror(errno));
        ret_val = SL_ERROR_BASE;
        _abort_reason = "TCP API write failed";
        tc_abort();
    }
    safe_free(message);
    return ret_val;
}

//! send a command to the SmartSDR API (radio) and wait for a response
//! this is a blocking call on the radio and will not return if the SmartSDR
//! process is not running or not responding
uint32 tc_sendSmartSDRcommand(char* command, BOOL block, char** response)
{
    if (response) *response = NULL;
    uint32 sequence = 0;

   // if (strcmp(command, "ping") != 0)
   //     output(ANSI_GREEN "sending command: \033[m%s\n",command);
    uint32 result = _sendAPIcommand(command, &sequence, block);

    // if we're not waiting for a response, just return
    if (!block) return SUCCESS;

    // if the send wasn't successful, let's not wait for the result ;-)
    if (result != SUCCESS && response) *response = NULL;

    // wait for result here for sequence
    if (response) *response = _tc_commandList_getResponse(sequence);

    return SUCCESS;
}

static void* _keepalive_thread(void* param)
{
    char* response;

    prctl(PR_SET_NAME, "DV-KeepAlive");

    /* Sleep 2 seconds */
    usleep(2000000);

    // enable the keepalive mechanism in SmartSDR
    uint32 ret_val = tc_sendSmartSDRcommand("keepalive enable", TRUE, &response);
    if (ret_val != SUCCESS)
    {
        _abort_reason = "keepalive enable failed";
        tc_abort();
        return NULL;
    }
    if (response) free(response);

    while (!_abort_keepalive)
    {
        // wait a second
        usleep(1000000);
        uint32 ret_val = tc_sendSmartSDRcommand("ping", FALSE, &response);
        // must free the response if we got one
        if (response) free (response);
        // if we can't send a ping, all is lost and we must exit
        if (ret_val != SUCCESS)
        {
            _abort_reason = "ping write failed";
            tc_abort();
            break;
        }
    }
    output("Keep thread closing\n");
    return NULL;
}

void tc_startKeepalive(void)
{
    // Start the keepalive thread
    pthread_create(&_keepalive_thread_id, NULL, &_keepalive_thread, NULL);
}

void tc_abort(void)
{
    output(ANSI_RED "stopping Traffic Cop: %s ...\n", _abort_reason);
    // stop the keepalive thread
    _abort_keepalive = TRUE;
    // stop the main TC thread
    _abort = TRUE;
    usleep(1000000);
    exit(1);
}

void tc_Init(const char * hostname, const char * api_port)
{
    _commandList_Init();
    output("\033[32mStarting Traffic Cop...\n\033[m");

    if ( hostname == NULL || api_port == NULL) {
		output("NULL Hostname - tc_setHostname()\n");
		return;
	}
    struct ifaddrs *ifaddr, *ifa;
    int family, s, n;
    char host[NI_MAXHOST];
    BOOL use_loopback = FALSE;

    if ( getifaddrs(&ifaddr) == -1 ) {
		output("Error getting local interface addrs. Using ip supplied in discovery packet\n");
    } else {
		/* Walk through linked list of interfaces. */
		ifa = ifaddr;
		for ( n = 0; ifa != NULL; ifa = ifa->ifa_next, n++ ) {
			if ( ifa->ifa_addr == NULL )
				continue;

			family = ifa->ifa_addr->sa_family;

			/* Only care about IPV4 adresses */
			if ( family == AF_INET ) {
				s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
				if ( s != 0 ) {
					output("getnameinfo() failed: %s\n", gai_strerror(s));
					continue;
				} else {

					/* We are on the same IP in the waveform and the radio hence we'll use local loopback interface instead
					 * of the radios IP
					 */
					if ( strncmp(hostname, host, NI_MAXHOST) == 0 ) {
						use_loopback = TRUE;
						output("We are on the same IP as the radio. Using loopback interface\n");
						break;
					}
				}
			} else {
				continue;
			}
		}
    }

    if ( !use_loopback )
		strncpy(_hostname, hostname, 31);
    else
		strncpy(_hostname, "127.0.0.1", 31);

	strncpy(_api_port, api_port, 31);


    uint32 ret_val = pthread_create(&_tc_thread_id, NULL, &_tc_thread, NULL);
    if (ret_val != 0) output("failed to start Traffic Cop thread\n");
}
