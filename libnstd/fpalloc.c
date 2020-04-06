/**
 * @brief Feedback Pool Allocator
 *
 * @file fpalloc.c
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

/*---------------------------Includes-----------------------------------*/
#include <ndrx_config.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys_primitives.h> /**< spin locks for MacOS */
#include <ndrstandard.h>
#include <fpalloc.h>
#include <thlock.h>
#include <atmi.h>
#include <errno.h>
#include <nstdutil.h>
#include <userlog.h>
/*---------------------------Externs------------------------------------*/
/*---------------------------Macros-------------------------------------*/

#define NDRX_FP_SIZE_SYSBUF   -2    /**< settings are for system buffer */
#define NDRX_FP_SIZE_DEFAULT   -1   /**< these are default settings     */

/* states: */            
#define NDRX_FP_GETSIZE         1 
#define NDRX_FP_GETMODE         2
#define NDRX_FP_USEMALLOC       3
#define NDRX_FP_USEMIN          4
#define NDRX_FP_USEMAX          5
#define NDRX_FP_USEHITS         6

/*---------------------------Enums--------------------------------------*/
/*---------------------------Typedefs-----------------------------------*/
/*---------------------------Globals------------------------------------*/

/**
 * List of malloc pools with different sizes and min/max settings
 */
exprivate volatile ndrx_fpastack_t M_fpa_stacks[NDRX_FPA_MAX] =
{  
    /* size                 bmin   bmax  flags          */
    {1*1024,                10,     20,  NDRX_FPA_FNOFLAG}      /* 1 */
    ,{2*1024,               10,     20,  NDRX_FPA_FNOFLAG}      /* 2 */
    ,{4*1024,               10,     20,  NDRX_FPA_FNOFLAG}      /* 3 */
    ,{8*1024,               10,     20,  NDRX_FPA_FNOFLAG}      /* 4 */
    ,{16*1024,              10,     20,  NDRX_FPA_FNOFLAG}      /* 5 */
    ,{32*1024,              10,     20,  NDRX_FPA_FNOFLAG}      /* 6 */
    ,{64*1024,              10,     20,  NDRX_FPA_FNOFLAG}      /* 7 */
    ,{NDRX_FP_SIZE_SYSBUF,  10,     20,  NDRX_FPA_FSYSBUF}      /* 8 */
};

/*---------------------------Statics------------------------------------*/
MUTEX_LOCKDECL(M_initlock);
exprivate volatile int M_init_first = EXTRUE;   /**< had the init ?     */
exprivate int M_malloc_all = EXFALSE;           /**< do sys malloc only */
exprivate char *M_opts = NULL;                  /**< env options        */
/*---------------------------Prototypes---------------------------------*/

/**
 * Init the feedback allocator. This will parse CONF_NDRX_FPAOPTS env
 * variable, if provided.
 * @return EXSUCCEED/EXFAIL
 */
exprivate int ndrx_finit(void)
{
    int ret = EXSUCCEED;
    int i, state, blocksz, bmin, bmax, hits, found;
    char settings[1024];
    char *bconf, *bopt, *saveptr1, *saveptr2;
    /* load defaults */
    for (i=0; i<N_DIM(M_fpa_stacks); i++)
    {
        M_fpa_stacks[i].blocks = 0;
        M_fpa_stacks[i].hits = 0;
        M_fpa_stacks[i].stack = NULL;
        pthread_spin_init(&M_fpa_stacks[i].spinlock, 0);
    }
    
    /* setup the options if any... */
    M_opts=getenv(CONF_NDRX_FPAOPTS);
    
    if (NULL!=M_opts)
    {
        NDRX_STRCPY_SAFE(settings, M_opts);
        
        /* start parse. 
         * format:
         * [<D|S|KB>:{M|<MIN>{[:<MAX>]|[:<MAX>,<HITS>]}}][,..,[<D|S|KB>:{M|<MIN>{[:<MAX>]|[:<MAX>:<HITS>]}}]]
         * where:
         * 'D' - Defaults, change all allocation stacks
         * 'S' - System buffer 
         * 'KB' - Block size, currently const 1,2,4,5,16,32,64
         * 'M' - use system malloc
         * 'MIN' - min blocks in pool
         * 'MAX' - max blocks in pool
         * 'HITS' - number of gotten.
         * This will not remove any spaces, etc.... In case of parse failure,
         * ulog will be written and defaults will be used.
         */
        for (bconf=strtok_r(settings, ",", &saveptr1); NULL!=bconf; bconf=strtok_r(NULL, ",", &saveptr1))
        {   
            /* got block config, parse each */
            state = NDRX_FP_GETSIZE;
            bmin=EXFAIL;
            bmax=EXFAIL;
            for (bopt=strtok_r(bconf, ",", &saveptr2); NULL!=bopt; bopt=strtok_r(NULL, ",", &saveptr2))
            {
                switch (state)
                {
                    case NDRX_FP_GETSIZE:
                        
                        if (0==strcmp(bopt, "D"))
                        {
                            blocksz = NDRX_FP_SIZE_DEFAULT;
                        }
                        else if (0==strcmp(bopt, "S"))
                        {
                            blocksz = NDRX_FP_SIZE_SYSBUF;
                        }
                        else
                        {
                            /* parse number - check first? */
                            if (ndrx_is_numberic(bopt))
                            {
                                userlog("%s: Invalid 'size' token [%s] in [%s]", 
                                        CONF_NDRX_FPAOPTS, bopt, M_opts);
                                EXFAIL_OUT(ret);
                            }
                            blocksz = atoi(bopt) * 1024;
                        }
                        
                        state = NDRX_FP_GETMODE;
                        break;
                    case NDRX_FP_GETMODE:
                        /* mode is either "M" or number (as min) */
                        if (0==strcmp(bopt, "M"))
                        {
                            state = NDRX_FP_USEMALLOC;
                        }
                        else
                        {
                            if (ndrx_is_numberic(bopt))
                            {
                                userlog("%s: Invalid 'min' token [%s] in [%s]", 
                                        CONF_NDRX_FPAOPTS, bopt, M_opts);
                                EXFAIL_OUT(ret);
                            }
                            bmin = atoi(bopt);
                            state=NDRX_FP_USEMIN;
                        }
                        break;
                    case NDRX_FP_USEMALLOC:
                        /* err ! not more settings! */
                        userlog("%s: Invalid argument for 'M' token [%s] in [%s]", 
                                        CONF_NDRX_FPAOPTS, bopt, M_opts);
                        EXFAIL_OUT(ret);
                        break;
                    case NDRX_FP_USEMIN:
                        /* got min, now get max */
                        if (ndrx_is_numberic(bopt))
                        {
                            userlog("%s: Invalid 'max' token [%s] in [%s]", 
                                    CONF_NDRX_FPAOPTS, bopt, M_opts);
                            EXFAIL_OUT(ret);
                        }
                        bmax = atoi(bopt);
                        state=NDRX_FP_USEMAX;
                        break;
                    case NDRX_FP_USEMAX:
                        /* got min, now get hits */
                        if (ndrx_is_numberic(bopt))
                        {
                            userlog("%s: Invalid 'hits' token [%s] in [%s]", 
                                    CONF_NDRX_FPAOPTS, bopt, M_opts);
                            EXFAIL_OUT(ret);
                        }
                        hits = atoi(bopt);
                        state = NDRX_FP_USEHITS;
                        break;
                    case NDRX_FP_USEHITS:
                        /* err ! not more settings! */
                        userlog("%s: Unexpected argument after 'hits' token [%s] in [%s]", 
                                        CONF_NDRX_FPAOPTS, bopt, M_opts);
                        EXFAIL_OUT(ret);
                        break;
                } /* case state */
            } /* for bopt */
            
            if (NDRX_FP_USEMALLOC > state)
            {
                userlog("%s: Missing mode/min for block size %d in [%s]", 
                        CONF_NDRX_FPAOPTS, (int)blocksz, M_opts);
                EXFAIL_OUT(ret);
            }
            
            /* not the best search, but only once for startup */
            found=EXFAIL;
            for (i=0; i<N_DIM(M_fpa_stacks); i++)
            {
                if (NDRX_FP_SIZE_DEFAULT==blocksz || 
                        blocksz==M_fpa_stacks[i].bsize)
                {
                    /* Setup the block */
                    if (NDRX_FP_USEMALLOC==state)
                    {
                        M_fpa_stacks[i].flags|=NDRX_FPA_FNOPOOL;
                        found=EXTRUE;
                    }
                    else 
                    {
                        if (EXFAIL!=bmin)
                        {
                            M_fpa_stacks[i].bmin=bmin;
                        }
                        
                        if (EXFAIL!=bmax)
                        {
                            M_fpa_stacks[i].bmax=bmax;
                        }
                        
                        if (EXFAIL!=hits)
                        {
                            M_fpa_stacks[i].hits=hits;
                        }
                        
                        found=EXTRUE;
                    }
                } /* check block size */
            } /* for stack */
            
            if (!found)
            {
                userlog("%s: Block size %d not supported in [%s]", 
                        CONF_NDRX_FPAOPTS, bopt, M_opts);
            }
        } /* for block config */
        
        /* check if all default -> set global flag to default malloc. */
        M_malloc_all = EXTRUE;
        for (i=0; i<N_DIM(M_fpa_stacks); i++)
        {
            if (!(M_fpa_stacks[i].flags & NDRX_FPA_FNOPOOL))
            {
                M_malloc_all = EXFALSE;
                break;
            }
        }
    } /* if has NDRX_FPAOPTS */
    
    M_init_first=EXTRUE;
out:
    errno=EINVAL;

    return ret;
}

/**
 * Malloc the memory block
 * @param size bytes to alloc
 * @param flags any special flag
 * @return for error = NULL  (errno set), or ptr to alloc'd block
 */
expublic NDRX_API void *ndrx_fmalloc(size_t size, int flags)
{
    void *ret = NULL;
    
    /* do the init. */
    if (!M_init_first)
    {
        MUTEX_LOCK_V(M_initlock);
        
        if (!M_init_first)
        {
            if (EXSUCCEED!=ndrx_finit())
            {
                MUTEX_UNLOCK_V(M_initlock);
                goto out; /* terminate malloc */
            }
        }
        MUTEX_UNLOCK_V(M_initlock);
    }
    
    /* run all malloc */
    if (M_malloc_all)
    {
        return malloc(size);
    }
    
    /* bin search... for the descriptor */
    
out:
                        
    return ret;
    
}

expublic NDRX_API void *ndrx_ffree(void *ptr)
{
    
}


/* vim: set ts=4 sw=4 et smartindent: */
