/*
 * SWO Trace Feeder by Bennet Krause
 * =========================================================
 *
 * Based on orbcat.c by: 
 *
 * Copyright (C) 2017, 2019  Dave Marples  <dave@marples.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the names Orbtrace, Orbuculum nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#if defined OSX
    #include <libusb.h>
#else
    #if defined LINUX
        #include <libusb-1.0/libusb.h>
    #else
        #error "Unknown OS"
    #endif
#endif
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <termios.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "git_version_info.h"
#include "generics.h"
#include "tpiuDecoder.h"
#include "itmDecoder.h"
#include "msgDecoder.h"

#define SERVER_PORT 3443                  /* Server port definition */

#define TRANSFER_SIZE (4096)
#define NUM_CHANNELS  32
#define HW_CHANNEL    (NUM_CHANNELS)      /* Make the hardware fifo on the end of the software ones */

#define MAX_STRING_LENGTH (100)           /* Maximum length that will be output from a fifo for a single event */

// Record for options, either defaults or from command line
struct
{
    /* Config information */
    bool useTPIU;
    uint32_t tpiuITMChannel;
    bool forceITMSync;
    uint32_t hwOutputs;

    bool logOutput;

    /* Sink information */
    char *presFormat[NUM_CHANNELS + 1];

    /* Source information */
    int port;
    char *server;

    char *file;                                          /* File host connection */
    bool fileTerminate;                                  /* Terminate when file read isn't successful */
} options = {.logOutput = false, .hwOutputs = 0xFFFF, .forceITMSync = true, .tpiuITMChannel = 1, .port = SERVER_PORT, .server = "localhost"};

struct
{
    /* The decoders and the packets from them */
    struct ITMDecoder i;
    struct ITMPacket h;
    struct TPIUDecoder t;
    struct TPIUPacket p;
    enum timeDelay timeStatus;           /* Indicator of if this time is exact */
    uint64_t timeStamp;                  /* Latest received time */
} _r;
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Handler for individual message types from SWO
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _handleException( struct excMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_EXCEPTION );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_EXCEPTION ) ) )
    {
        return;
    }

    const char *exNames[] = {"Thread", "Reset", "NMI", "HardFault", "MemManage", "BusFault", "UsageFault", "UNKNOWN_7",
                             "UNKNOWN_8", "UNKNOWN_9", "UNKNOWN_10", "SVCall", "Debug Monitor", "UNKNOWN_13", "PendSV", "SysTick"
                            };
    const char *exEvent[] = {"Enter", "Exit", "Resume"};
    fprintf( stdout, "%d,%s,%s" EOL, HWEVENT_EXCEPTION, exEvent[m->eventType], exNames[m->exceptionNumber] );
}
// ====================================================================================================
void _handleDWTEvent( struct dwtMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_DWT_EVENT );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_DWT ) ) )
    {
        return;
    }

    const char *evName[] = {"CPI", "Exc", "Sleep", "LSU", "Fold", "Cyc"};

    for ( uint32_t i = 0; i < 6; i++ )
    {
        if ( m->event & ( 1 << i ) )
        {
            fprintf( stdout, "%d,%s" EOL, HWEVENT_DWT, evName[m->event] );
        }
    }
}
// ====================================================================================================
void _handlePCSample( struct pcSampleMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_PC_SAMPLE );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_PCSample ) ) )
    {
        return;
    }

    fprintf( stdout, "%d,0x%08x" EOL, HWEVENT_PCSample, m->pc );
}
// ====================================================================================================
/* 
 * DWT memory read/write events are handled here. This function was modified to reduce the footprint of the output.
 */
void _handleDataRWWP( struct watchMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_DATA_RWWP );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_RWWT ) ) )
    {
        return;
    }

    fprintf( stdout, "d,%d,%s,%x" EOL, m->comp, m->isWrite ? "w" : "r", m->data ); // "d" for data
}
// ====================================================================================================
void _handleDataAccessWP( struct wptMsg *m, struct ITMDecoder *i )


{
    assert( m->msgtype == MSG_DATA_ACCESS_WP );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_AWP ) ) )
    {
        return;
    }

    fprintf( stdout, "%d,%d,0x%08x" EOL, HWEVENT_AWP, m->comp, m->data );
}
// ====================================================================================================
void _handleDataOffsetWP(  struct oswMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_OSW );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_OFS ) ) )
    {
        return;
    }

    fprintf( stdout, "%d,%d,0x%04x" EOL, HWEVENT_OFS, m->comp, m->offset );
}
// ====================================================================================================
/* 
 * DWT memory read/write events are handled here. This function was modified to reduce the footprint of the output.
 */
void _handleSW( struct swMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_SOFTWARE );

    if ( options.logOutput ) 
    {
        if ( m->srcAddr == 10 ) // trace: log message
        {
            fprintf( stdout, "%c", m->value );
        }

        // only output log messages, for normal trace a second instance should be started without -l option
        return;
    }

    if ( m->srcAddr == 1 ) // trace: func enter packet #1
    {
        fprintf( stdout, "f,1,%x" EOL, m->value);
    }
    else if ( m->srcAddr == 2 ) // trace: func enter packet #2
    {
        fprintf( stdout, "f,2,%x" EOL, m->value);
    }
    else if ( m->srcAddr == 3 ) // trace: func exit packet #1
    {
        fprintf( stdout, "f,3,%x" EOL, m->value);
    }
    else if ( m->srcAddr == 4 ) // trace: func exit packet #2
    {
        fprintf( stdout, "f,4,%x" EOL, m->value);
    }
    else if ( m->srcAddr == 5 ) // trace: msg send
    {
        fprintf( stdout, "m,1,%x" EOL, m->value );
    }
    else if ( m->srcAddr == 6 ) // trace: msg receive
    {
        fprintf( stdout, "m,2,%x" EOL, m->value );
    }
}
// ====================================================================================================
void _handleTS( struct TSMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_TS );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_TS ) ) )
    {
        return;
    }

    _r.timeStamp += m->timeInc;

    fprintf( stdout, "%d,%d,%" PRIu64 EOL, HWEVENT_TS, _r.timeStatus, _r.timeStamp );
}
// ====================================================================================================
void _itmPumpProcess( char c )

{
    struct msg decoded;

    typedef void ( *handlers )( void *decoded, struct ITMDecoder * i );

    /* Handlers for each complete message received */
    static const handlers h[MSG_NUM_MSGS] =
    {
        /* MSG_UNKNOWN */         NULL,
        /* MSG_RESERVED */        NULL,
        /* MSG_ERROR */           NULL,
        /* MSG_NONE */            NULL,
        /* MSG_SOFTWARE */        ( handlers )_handleSW,
        /* MSG_NISYNC */          NULL,
        /* MSG_OSW */             ( handlers )_handleDataOffsetWP,
        /* MSG_DATA_ACCESS_WP */  ( handlers )_handleDataAccessWP,
        /* MSG_DATA_RWWP */       ( handlers )_handleDataRWWP,
        /* MSG_PC_SAMPLE */       ( handlers )_handlePCSample,
        /* MSG_DWT_EVENT */       ( handlers )_handleDWTEvent,
        /* MSG_EXCEPTION */       ( handlers )_handleException,
        /* MSG_TS */              ( handlers )_handleTS
    };

    switch ( ITMPump( &_r.i, c ) )
    {
        case ITM_EV_NONE:
            break;

        case ITM_EV_UNSYNCED:
            genericsReport( V_INFO, "ITM Unsynced" EOL );
            break;

        case ITM_EV_SYNCED:
            genericsReport( V_INFO, "ITM Synced" EOL );
            break;

        case ITM_EV_OVERFLOW:
            //genericsReport( V_WARN, "ITM Overflow" EOL );
	    fprintf( stdout, "ITM_OVERFLOW" EOL );
            break;

        case ITM_EV_ERROR:
            genericsReport( V_WARN, "ITM Error" EOL );
            break;

        case ITM_EV_PACKET_RXED:
            ITMGetDecodedPacket( &_r.i, &decoded );

            /* See if we decoded a dispatchable match. genericMsg is just used to access */
            /* the first two members of the decoded structs in a portable way.           */
            if ( h[decoded.genericMsg.msgtype] )
            {
                ( h[decoded.genericMsg.msgtype] )( &decoded, &_r.i );
            }

            break;

        default:
            break;
    }
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Protocol pump for decoding messages
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _protocolPump( uint8_t c )

{
    if ( options.useTPIU )
    {
        switch ( TPIUPump( &_r.t, c ) )
        {
            case TPIU_EV_NEWSYNC:
            case TPIU_EV_SYNCED:
                ITMDecoderForceSync( &_r.i, true );
                break;

            case TPIU_EV_RXING:
            case TPIU_EV_NONE:
                break;

            case TPIU_EV_UNSYNCED:
                ITMDecoderForceSync( &_r.i, false );
                break;

            case TPIU_EV_RXEDPACKET:
                if ( !TPIUGetPacket( &_r.t, &_r.p ) )
                {
                    genericsReport( V_WARN, "TPIUGetPacket fell over" EOL );
                }

                for ( uint32_t g = 0; g < _r.p.len; g++ )
                {
                    if ( _r.p.packet[g].s == options.tpiuITMChannel )
                    {
                        _itmPumpProcess( _r.p.packet[g].d );
                        continue;
                    }

                    if  ( _r.p.packet[g].s != 0 )
                    {
                        genericsReport( V_WARN, "Unknown TPIU channel %02x" EOL, _r.p.packet[g].s );
                    }
                }

                break;

            case TPIU_EV_ERROR:
                genericsReport( V_WARN, "****ERROR****" EOL );
                break;
        }
    }
    else
    {
        _itmPumpProcess( c );
    }
}
// ====================================================================================================
void _printHelp( char *progName )

{
    fprintf( stdout, "Usage: %s <htv> <-i channel> <-p port> <-s server>" EOL, progName );
    fprintf( stdout, "       l: log message output stream (otherwise trace output)" EOL );
    fprintf( stdout, "       e: When reading from file, terminate at end of file rather than waiting for further input" EOL );
    fprintf( stdout, "       f: <filename> Take input from specified file" EOL );
    fprintf( stdout, "       h: This help" EOL );
    fprintf( stdout, "       i: <channel> Set ITM Channel in TPIU decode (defaults to 1)" EOL );
    fprintf( stdout, "       n: Enforce sync requirement for ITM (i.e. ITM needsd to issue syncs)" EOL );
    fprintf( stdout, "       s: <Server>:<Port> to use" EOL );
    fprintf( stdout, "       t: Use TPIU decoder" EOL );
    fprintf( stdout, "       v: <level> Verbose mode 0(errors)..3(debug)" EOL );
}
// ====================================================================================================
int _processOptions( int argc, char *argv[] )

{
    int c;
    
#define DELIMITER ','

    while ( ( c = getopt ( argc, argv, "c:ef:hi:ns:tv:l" ) ) != -1 )
        switch ( c )
        {
            // ------------------------------------
            case 'h':
                _printHelp( argv[0] );
                return false;

            // ------------------------------------
            case 'e':
                options.fileTerminate = true;
                break;

            // ------------------------------------
            case 'f':
                options.file = optarg;
                break;

            // ------------------------------------
            case 'i':
                options.tpiuITMChannel = atoi( optarg );
                break;

            // ------------------------------------
            case 'n':
                options.forceITMSync = false;
                break;

            // ------------------------------------
            case 's':
                options.server = optarg;

                // See if we have an optional port number too
                char *a = optarg;

                while ( ( *a ) && ( *a != ':' ) )
                {
                    a++;
                }

                if ( *a == ':' )
                {
                    *a = 0;
                    options.port = atoi( ++a );
                }

                if ( !options.port )
                {
                    options.port = SERVER_PORT;
                }

                break;

            // ------------------------------------
            case 't':
                options.useTPIU = true;
                break;

            // ------------------------------------
            case 'v':
                genericsSetReportLevel( atoi( optarg ) );
                break;

            // ------------------------------------
            case 'l':
                options.logOutput = true;
		options.hwOutputs = 0;
                break;

            // ------------------------------------
            case '?':
                if ( optopt == 'b' )
                {
                    genericsReport( V_ERROR, "Option '%c' requires an argument." EOL, optopt );
                }
                else if ( !isprint ( optopt ) )
                {
                    genericsReport( V_ERROR, "Unknown option character `\\x%x'." EOL, optopt );
                }

                return false;

            // ------------------------------------
            default:
                return false;
                // ------------------------------------
        }

    if ( ( options.useTPIU ) && ( !options.tpiuITMChannel ) )
    {
        genericsReport( V_ERROR, "TPIU set for use but no channel set for ITM output" EOL );
        return false;
    }

    genericsReport( V_INFO, "trace V" VERSION " (Git %08X %s, Built " BUILD_DATE EOL, GIT_HASH, ( GIT_DIRTY ? "Dirty" : "Clean" ) );

    genericsReport( V_INFO, "Server     : %s:%d" EOL, options.server, options.port );
    genericsReport( V_INFO, "ForceSync  : %s" EOL, options.forceITMSync ? "true" : "false" );

    if ( options.file )
    {

        genericsReport( V_INFO, "Input File : %s", options.file );

        if ( options.fileTerminate )
        {
            genericsReport( V_INFO, " (Terminate on exhaustion)" EOL );
        }
        else
        {
            genericsReport( V_INFO, " (Ongoing read)" EOL );
        }
    }

    if ( options.useTPIU )
    {
        genericsReport( V_INFO, "Using TPIU : true (ITM on channel %d)" EOL, options.tpiuITMChannel );
    }
    else
    {
        genericsReport( V_INFO, "Using TPIU : false" EOL );
    }

    return true;
}
// ====================================================================================================

int fileFeeder( void )

{
    int f;
    unsigned char cbw[TRANSFER_SIZE];
    ssize_t t;

    if ( ( f = open( options.file, O_RDONLY ) ) < 0 )
    {
        genericsExit( -4, "Can't open file %s" EOL, options.file );
    }

    while ( ( t = read( f, cbw, TRANSFER_SIZE ) ) >= 0 )
    {

        if ( !t )
        {
            if ( options.fileTerminate )
            {
                break;
            }
            else
            {
                // Just spin for a while to avoid clogging the CPU
                usleep( 100000 );
                continue;
            }
        }

        unsigned char *c = cbw;

        while ( t-- )
        {
            _protocolPump( *c++ );
        }
    }

    if ( !options.fileTerminate )
    {
        genericsReport( V_INFO, "File read error" EOL );
    }

    close( f );
    return true;
}

// ====================================================================================================
int main( int argc, char *argv[] )

{
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    unsigned char cbw[TRANSFER_SIZE];
    ssize_t t;
    int flag = 1;

    if ( !_processOptions( argc, argv ) )
    {
        exit( -1 );
    }

    /* Reset the TPIU handler before we start */
    TPIUDecoderInit( &_r.t );
    ITMDecoderInit( &_r.i, options.forceITMSync );

    sockfd = socket( AF_INET, SOCK_STREAM, 0 );
    setsockopt( sockfd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof( flag ) );

    if ( sockfd < 0 )
    {
        genericsReport( V_ERROR, "Error creating socket" EOL );
        return -1;
    }

    if ( options.file )
    {
        exit( fileFeeder() );
    }

    /* Now open the network connection */
    bzero( ( char * ) &serv_addr, sizeof( serv_addr ) );
    server = gethostbyname( options.server );

    if ( !server )
    {
        genericsReport( V_ERROR, "Cannot find host" EOL );
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    bcopy( ( char * )server->h_addr,
           ( char * )&serv_addr.sin_addr.s_addr,
           server->h_length );
    serv_addr.sin_port = htons( options.port );

    if ( connect( sockfd, ( struct sockaddr * ) &serv_addr, sizeof( serv_addr ) ) < 0 )
    {
        genericsReport( V_ERROR, "Could not connect" EOL );
        return -1;
    }

    while ( ( t = read( sockfd, cbw, TRANSFER_SIZE ) ) > 0 )
    {
        unsigned char *c = cbw;

        while ( t-- )
        {
            _protocolPump( *c++ );
        }

        fflush( stdout );
    }

    genericsReport( V_ERROR, "Read failed" EOL );

    close( sockfd );
    return -2;
}
// ====================================================================================================
