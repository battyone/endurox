/**
 * @brief Conversational server, multi-threaded. This also tests ability of
 *  multi-threaded servers to receive tpbroadcast notifications...
 *
 * @file atmisv75_conv.c
 */
/* -----------------------------------------------------------------------------
 * Enduro/X Middleware Platform for Distributed Transaction Processing
 * Copyright (C) 2009-2016, ATR Baltic, Ltd. All Rights Reserved.
 * Copyright (C) 2017-2019, Mavimax, Ltd. All Rights Reserved.
 * This software is released under one of the following licenses:
 * AGPL (with Java and Go exceptions) or Mavimax's license for commercial use.
 * See LICENSE file for full text.
 * -----------------------------------------------------------------------------
 * AGPL license:
 * 
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License, version 3 as published
 * by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU Affero General Public License, version 3
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * -----------------------------------------------------------------------------
 * A commercial use license is available from Mavimax, Ltd
 * contact@mavimax.com
 * -----------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <ndebug.h>
#include <atmi.h>
#include <ndrstandard.h>
#include <ubf.h>
#include <test.fd.h>
#include <string.h>
#include <unistd.h>
#include <thlock.h>
#include "test75.h"
#include "ubf_int.h"
#include <exassert.h>

/*---------------------------Externs------------------------------------*/
/*---------------------------Macros-------------------------------------*/
#define TEST_THREADS            1   /**< number of threads used         */
/*---------------------------Enums--------------------------------------*/
/*---------------------------Typedefs-----------------------------------*/
/*---------------------------Globals------------------------------------*/
/*---------------------------Statics------------------------------------*/
exprivate __thread int M_notifs = 0;
/*---------------------------Prototypes---------------------------------*/

/**
 * Set notification handler 
 * @return 
 */
void notification_callback (char *data, long len, long flags)
{
    M_notifs++;
}

/**
 * Run client test to CONV2
 */
int run_clt_test(void)
{
    int i, j;
    int ret=EXSUCCEED;
    int cd[TEST_THREADS];
    long revent, len;
    for (i=0; i<10; i++)
    {
        char *buf[TEST_THREADS];
        
        for (j=0; j<TEST_THREADS; j++)
        {
            buf[j]= tpalloc("STRING", NULL, 1024);
            NDRX_ASSERT_TP_OUT( (NULL!=buf[j]), "tpalloc failed %d", j);
        }
        
        /* connect to the server */
        for (j=0; j<TEST_THREADS; j++)
        {
            strcpy(buf[j], "HELLO");
            cd[j]=tpconnect("CONVSV2", buf[j], 0, TPSENDONLY);
            NDRX_ASSERT_TP_OUT( (EXFAIL!=cd[j]), "Failed to connect to CONVSV2!");
        }
        
        /* give control  */
        for (j=0; j<TEST_THREADS; j++)
        {
            strcpy(buf[j], "CLWAIT");
            revent=0;
            
            NDRX_ASSERT_TP_OUT( (EXSUCCEED==tpsend(cd[j], buf[j], 0, TPRECVONLY, &revent)), 
                    "Failed send CLWAIT");
        }
        
        /* now do broadcast... */
        for (j=0; j<9; j++)
        {
            NDRX_ASSERT_UBF_OUT((0==tpbroadcast("", "", "atmi.sv75_conv2", NULL, 0, 0)), 
                    "Failed to broadcast");
        }
        
        /* now receive data... */
        for (j=0; j<TEST_THREADS; j++)
        {
            revent=0;
            NDRX_ASSERT_TP_OUT( (EXSUCCEED==tprecv(cd[j], &buf[j], &len, 0, &revent)), 
                    "Failed to get counts");
            NDRX_ASSERT_VAL_OUT( (0==revent),  "Invalid revent");
            NDRX_ASSERT_VAL_OUT( (0==strcmp(buf[j], "9")),  
                    "Invalid count received but got: %s", buf[j]);
        }
        
        /* expect to finish the conv */
        for (j=0; j<TEST_THREADS; j++)
        {
            revent=0;
            NDRX_ASSERT_TP_OUT( (EXFAIL==tprecv(cd[j], &buf[j], &len, 0, &revent)), 
                    "Failed to recv");
            
            NDRX_ASSERT_TP_OUT( (TPEEVENT==tperrno),  "Expected TPEEVENT");
            NDRX_ASSERT_TP_OUT( (TPEV_SVCSUCC==revent),  "Expected TPEV_SVCSUCC");
        }
        
        for (j=0; j<TEST_THREADS; j++)
        {
            if (NULL!=buf[j])
            {
                tpfree(buf[j]);
            }
        }
    }
        
out:
    return ret;
    
}

/**
 * Conversational server
 */
void CONVSV1 (TPSVCINFO *p_svc)
{
    int ret=EXSUCCEED;
    char *buf = p_svc->data;
    long revent, len;
    int i;
    NDRX_LOG(log_debug, "%s got call", __func__);
    M_notifs = 0;
    
    buf = tprealloc(buf, 1024);
    NDRX_ASSERT_TP_OUT((NULL!=buf), "Failed to realloc");
    
    /* client: connect 4x (4x threads):  Using string buffer ... */
    NDRX_ASSERT_VAL_OUT((strcmp(buf, "HELLO")==0), "Expected HELLO at connection point");
    
    /* we shall receive some stuff... */
    NDRX_ASSERT_TP_OUT((EXFAIL==tprecv(p_svc->cd, &buf, &len, 0, &revent)), "Expected failure");
    /* test that we are able to connect as clients to other server */
    NDRX_ASSERT_VAL_OUT((EXSUCCEED==run_clt_test()), "Failed to run server side client tests");
    NDRX_ASSERT_TP_OUT( (TPEEVENT==tperrno),  "Expected TPEEVENT");
    NDRX_ASSERT_TP_OUT( (TPEV_SENDONLY==revent),  "Expected TPEV_SENDONLY");
    NDRX_ASSERT_VAL_OUT((strcmp(buf, "CLWAIT")==0), "Expected CLWAIT at connection point");
    
    /* Wait for notification... */
    tperrno = 0;
    while (EXFAIL!=tpchkunsol() && M_notifs < 9)
    {
        usleep(1000);
    }
    
    NDRX_ASSERT_TP_OUT( (0==tperrno), "Expected no error");

    /* send number back... */
    
    buf = tprealloc(buf, 1024);
    NDRX_ASSERT_TP_OUT((NULL!=buf), "Failed to realloc");
    sprintf(buf, "%d", M_notifs);
    
    NDRX_ASSERT_TP_OUT( (EXSUCCEED==tpsend(p_svc->cd, buf, 0, 0, &revent)), 
                    "Failed send nr of notifs...");
    /* OK finish off... */
out:
    tpreturn(  ret==EXSUCCEED?TPSUCCESS:TPFAIL,
                0L,
                (char *)buf,
                0L,
                0L);
}

/**
 * Do initialisation
 */
int tpsvrinit(int argc, char **argv)
{
    int ret = EXSUCCEED;
    NDRX_LOG(log_debug, "tpsvrinit called");

    NDRX_ASSERT_TP_OUT((EXSUCCEED==tpadvertise("CONVSV1", CONVSV1)), 
            "Failed to advertise");
    
out:
        
    return ret;
}

/**
 * Do de-initialisation
 * After the server, thread pool is stopped
 */
void tpsvrdone(void)
{
    NDRX_LOG(log_debug, "tpsvrdone called");
}

/**
 * Do initialisation
 */
int tpsvrthrinit(int argc, char **argv)
{
    int ret = EXSUCCEED;
    
    NDRX_ASSERT_TP_OUT((NULL==tpsetunsol(notification_callback)), 
            "Invalid previous unsol handler");
    
out:
    return ret;
}

/**
 * Do de-initialisation
 */
void tpsvrthrdone(void)
{
    NDRX_LOG(log_debug, "tpsvrthrdone called");
}

/* Auto generated system advertise table */
expublic struct tmdsptchtbl_t ndrx_G_tmdsptchtbl[] = {
    { NULL, NULL, NULL, 0, 0 }
};
/*---------------------------Prototypes---------------------------------*/

/**
 * Main entry for tmsrv
 */
int main( int argc, char** argv )
{
    _tmbuilt_with_thread_option=EXTRUE;
    struct tmsvrargs_t tmsvrargs =
    {
        &tmnull_switch,
        &ndrx_G_tmdsptchtbl[0],
        0,
        tpsvrinit,
        tpsvrdone,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        tpsvrthrinit,
        tpsvrthrdone
    };
    
    return( _tmstartserver( argc, argv, &tmsvrargs ));
    
}


/* vim: set ts=4 sw=4 et smartindent: */
