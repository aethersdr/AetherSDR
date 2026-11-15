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
#ifndef _WIN32
#include <ifaddrs.h>
#endif
#include <sys/prctl.h>

#include "common.h"
#include "aether_smartsdr_command.h"
#include "traffic_cop.h"
#include "status_processor.h"

static pthread_t _tc_thread_id;
static pthread_t _keepalive_thread_id;
static pthread_t _registration_thread_id;

static __thread receive_data __local;
//! address of the host to connect to -- defaults to 127.0.0.1
//! but this is generally provided by discovery
static char _hostname[32] = "127.0.0.1";
static char _api_port[32] = SMARTSDR_API_PORT;

//const char* gai_strerror(int ecode);
static BOOL _abort = FALSE;
#ifndef _WIN32
typedef int aether_socket_t;
#define AETHER_INVALID_SOCKET (-1)
#endif

static aether_socket_t _socket;
static BOOL _abort_keepalive = FALSE;
static const char* _abort_reason = "unspecified";
static uint32 _sequence = 0;
static BOOL _setup_diagnostics_enabled = FALSE;
static Command _root;
static pthread_mutex_t _commandList_mutex;
static pthread_mutex_t _send_mutex = PTHREAD_MUTEX_INITIALIZER;
static const time_t COMMAND_RESPONSE_TIMEOUT_SECONDS = 5;

static void _tc_closeSocket(aether_socket_t socket_handle)
{
#ifdef _WIN32
    aether_closesocket(socket_handle);
#else
    close(socket_handle);
#endif
}

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
                p->ai_protocol)) == AETHER_INVALID_SOCKET) {
            output(ANSI_RED "Traffic Cop: socket error\n");
            continue;
        }

        if (connect(_socket, p->ai_addr, p->ai_addrlen) == -1) {
            _tc_closeSocket(_socket);
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
    unsigned char incoming[RECV_BUF_SIZE_TO_GET];
    while (!_abort) {
        errno = 0;
        const int read_len = recv(_socket, incoming, sizeof(incoming), 0);
        const int socket_error = errno;
        if (read_len > 0) {
            if (aether_tcp_frame_buffer_append(
                    &__local.frame_buffer, incoming, (size_t)read_len) != 0) {
                output(ANSI_RED
                       "Traffic Cop: incoming API frame exceeds %u bytes\n",
                       (unsigned int)RECV_BUF_SIZE);
                return SL_CLOSE_CLIENT;
            }
            return SUCCESS;
        }
        if (read_len == 0) {
            return SL_CLOSE_CLIENT;
        }
        if (socket_error != EAGAIN && socket_error != EWOULDBLOCK
            && socket_error != EINTR) {
            return SL_CLOSE_CLIENT;
        }
        if (!_check_for_timeout()) {
            output(ANSI_RED
                   "\nTraffic Cop has failed keepalive test; terminating\n");
            return SL_CLOSE_CLIENT;
        }
    }
    return SL_TERMINATE;
}

static uint32 _get_command(void)
{
    char frame[RECV_BUF_SIZE];
    while (!_abort) {
        size_t frame_size = 0U;
        const aether_tcp_frame_result frame_result =
            aether_tcp_frame_buffer_next(
                &__local.frame_buffer,
                frame,
                sizeof(frame),
                &frame_size);
        if (frame_result == AETHER_TCP_FRAME_READY) {
            __local.command = (char*)safe_malloc(frame_size + 1U);
            if (__local.command == NULL) {
                return SL_OUT_OF_MEMORY;
            }
            memcpy(__local.command, frame, frame_size + 1U);
            return SUCCESS;
        }
        if (frame_result == AETHER_TCP_FRAME_TERMINATE) {
            return SL_TERMINATE;
        }
        if (frame_result == AETHER_TCP_FRAME_OVERFLOW) {
            output(ANSI_RED
                   "Traffic Cop: rejecting oversized or malformed API frame\n");
            return SL_CLOSE_CLIENT;
        }
        const uint32 read_result = _read_more_data();
        if (read_result != SUCCESS) {
            return read_result;
        }
    }
    return SL_TERMINATE;
}

void process_status(char* string)
{
#ifdef DEBUG
    output(ANSI_GREEN "Traffic Cop: received \033[m'%s'\n",string);
#endif
    status_processor(string);
}

static void* _registration_thread(void* arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "DV-Register");

    const uint32 result = register_mode();
    if (result != SUCCESS) {
        output("** Could not register digital-voice mode (0x%08X) **\n", result);
        _abort_reason = "register mode failed";
        tc_abort();
        return NULL;
    }

    tc_sendSmartSDRcommand("sub slice all", FALSE, NULL);
    vita_output_Init(_hostname);
    tc_startKeepalive();
    return NULL;
}


//! main traffic cop receiver thread
static void* _tc_thread(void* arg)
{
    uint32 result;

    prctl(PR_SET_NAME, "DV-TrafficCop");

    memset(&__local, 0, sizeof(receive_data));
    __local.last_ping.tv_sec = 0;
    __local.recv_buf = safe_malloc(RECV_BUF_SIZE);
    if (__local.recv_buf == NULL) {
        _abort_reason = "TCP receive buffer allocation failed";
        return NULL;
    }
    aether_tcp_frame_buffer_init(
        &__local.frame_buffer, __local.recv_buf, RECV_BUF_SIZE);

    // make a connection to SmartSDR
    // if this fails, the program just exits
    _tc_openSocket();

    result = pthread_create(
        &_registration_thread_id, NULL, &_registration_thread, NULL);
    if (result != 0) {
        output("** Could not start digital-voice registration thread **\n");
        _abort_reason = "registration thread failed";
        tc_abort();
    }

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
            output("Traffic Cop: terminate requested\n");
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

    _tc_closeSocket(_socket);
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
    const char* safe_response = response != NULL ? response : "";
#ifdef DEBUG
    output("response for %d: '%s'\n",sequence,safe_response);
#endif
    pthread_mutex_lock(&_commandList_mutex);
    Command iterator = _root;
    while(iterator->next != _root)
    {
        iterator = iterator->next;
        if (iterator->sequence != sequence) continue;

        const size_t len = strlen(safe_response);
        char* resp = safe_malloc(len+1);
        if (resp == NULL) {
            break;
        }
        memcpy(resp, safe_response, len+1U);
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
        struct timespec deadline;
        deadline.tv_sec = time(NULL) + COMMAND_RESPONSE_TIMEOUT_SECONDS;
        deadline.tv_nsec = 0;
        int wait_result;
        do {
            wait_result = sem_timedwait(&iterator->semaphore, &deadline);
        } while (wait_result != 0 && errno == EINTR);
#ifdef DEBUG
        output("received post %d...\n",sequence);
#endif
        pthread_mutex_lock(&_commandList_mutex);
        _commandList_Unlink(iterator);
        pthread_mutex_unlock(&_commandList_mutex);
        sem_destroy(&iterator->semaphore);
        char* response = iterator->response;
        free(iterator);
        if (wait_result != 0) {
            free(response);
            output("Traffic Cop: timed out waiting for command response %u\n",
                   sequence);
            return NULL;
        }
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
    if (command == NULL || sequence == NULL) {
        return SL_BAD_COMMAND;
    }

    char message[MAX_API_COMMAND_SIZE];

    pthread_mutex_lock(&_send_mutex);
    _sequence++;
    *sequence = _sequence;

    const int length = aether_smartsdr_frame_command(
        message, sizeof(message), *sequence, command);
    if (length < 0) {
        output(ANSI_RED
               "Traffic Cop: rejected invalid or oversized API command\n");
        pthread_mutex_unlock(&_send_mutex);
        return SL_BAD_COMMAND;
    }

    if (_setup_diagnostics_enabled)
    {
        const size_t command_len = strcspn(command, "\r\n");
        output("AETHER_DV_DIAG setup_tx sequence=%u command=\"%.*s\"\n",
               *sequence,
               (int)command_len,
               command);
    }

    if (block) _tc_commandList_add(*sequence);

    size_t sent_total = 0U;
    while (sent_total < (size_t)length) {
        errno = 0;
        const int sent = send(_socket,
                              message + sent_total,
                              (size_t)length - sent_total,
                              0);
        if (sent > 0) {
            sent_total += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        output(ANSI_RED
               "Traffic Cop: error writing to TCP API socket: %s\n",
               strerror(errno));
        pthread_mutex_unlock(&_send_mutex);
        _abort_reason = "TCP API write failed";
        tc_abort();
        return SL_ERROR_BASE;
    }
    pthread_mutex_unlock(&_send_mutex);
    return SUCCESS;
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

    // if we're not waiting for a response, return the actual send result
    if (!block) return result;

    // If the write failed, no response can arrive for this command.
    if (result != SUCCESS) {
        if (response) *response = NULL;
        return result;
    }

    char* received = _tc_commandList_getResponse(sequence);
    if (received == NULL) {
        return SL_ERROR_BASE;
    }
    if (response != NULL) {
        *response = received;
    } else {
        free(received);
    }

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

BOOL tc_setupDiagnosticsEnabled(void)
{
    return _setup_diagnostics_enabled;
}

void tc_Init(const char * hostname, const char * api_port)
{
    const char* setup_diag = getenv("AETHER_DV_DIAG_SETUP");
    _setup_diagnostics_enabled =
        setup_diag != NULL && strcmp(setup_diag, "1") == 0;

    _commandList_Init();
    output("\033[32mStarting Traffic Cop...\n\033[m");

    if ( hostname == NULL || api_port == NULL) {
		output("NULL Hostname - tc_setHostname()\n");
		return;
	}
    BOOL use_loopback = FALSE;

#ifndef _WIN32
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    int family;
    int s;
    char host[NI_MAXHOST];

    if ( getifaddrs(&ifaddr) == -1 ) {
		output("Error getting local interface addrs. Using ip supplied in discovery packet\n");
    } else {
		/* Walk through linked list of interfaces. */
		ifa = ifaddr;
		for ( ; ifa != NULL; ifa = ifa->ifa_next ) {
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
		freeifaddrs(ifaddr);
    }
#endif

    if ( !use_loopback )
		strncpy(_hostname, hostname, 31);
    else
		strncpy(_hostname, "127.0.0.1", 31);

	strncpy(_api_port, api_port, 31);


    uint32 ret_val = pthread_create(&_tc_thread_id, NULL, &_tc_thread, NULL);
    if (ret_val != 0) output("failed to start Traffic Cop thread\n");
}
