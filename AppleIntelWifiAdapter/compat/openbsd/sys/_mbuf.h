//
//  mbuf.h
//  AppleIntelWifiAdapter
//
//  Created by 钟先耀 on 2020/1/22.
//  Copyright © 2020 钟先耀. All rights reserved.
//

#ifndef _mbuf_h
#define _mbuf_h

#include <sys/mbuf.h>
#include <sys/kpi_mbuf.h>
#include <IOKit/network/IOPacketQueue.h>
#include "../../../IWLDebug.h"

#define PACKET_TAG_DLT            0x0100 /* data link layer type */
#define IPL_NET        6

#define    mtod(x,t)    ((t) mbuf_data(x))
#define    ml_len(_ml)        ((_ml)->ml_len)
#define    ml_empty(_ml)        ((_ml)->ml_len == 0)

#define MBUF_LIST_FIRST(_ml)    ((_ml)->ml_head)
#define MBUF_LIST_NEXT(_m)    (mbuf_nextpkt((_m)))

#define MBUF_LIST_FOREACH(_ml, _m)                    \
    for ((_m) = MBUF_LIST_FIRST(_ml);                \
        (_m) != NULL;                        \
        (_m) = MBUF_LIST_NEXT(_m))

#define    mq_len(_mq)        ml_len(&(_mq)->mq_list)
#define    mq_empty(_mq)        ml_empty(&(_mq)->mq_list)
#define    mq_full(_mq)        (mq_len((_mq)) >= (_mq)->mq_maxlen)
#define    mq_drops(_mq)        ((_mq)->mq_drops)
#define    mq_set_maxlen(_mq, _l)    ((_mq)->mq_maxlen = (_l))

//uipc_mbuf.c

struct mbuf_list {
    mbuf_t ml_head;
    mbuf_t ml_tail;
    u_int  ml_len;
};

struct mbuf_queue {
    IORecursiveLock*        mq_mtx;
    struct mbuf_list    mq_list;
    u_int            mq_maxlen;
    u_int            mq_drops;
};

/*
 * mbuf lists
 */

#define MBUF_LIST_INITIALIZER() { NULL, NULL, 0 }

#define MBUF_QUEUE_INITIALIZER(_maxlen, _ipl) \
{ MUTEX_INITIALIZER(_ipl), MBUF_LIST_INITIALIZER(), (_maxlen), 0 }

static inline void
ml_init(struct mbuf_list *ml)
{
    ml->ml_head = ml->ml_tail = NULL;
    ml->ml_len = 0;
}

static inline void
ml_enqueue(struct mbuf_list *ml, mbuf_t m)
{
    if (ml->ml_tail == NULL)
        ml->ml_head = ml->ml_tail = m;
    else {
        mbuf_setnextpkt(ml->ml_tail, m);
        ml->ml_tail = m;
    }

    mbuf_setnextpkt(m, NULL);
    ml->ml_len++;
}

static inline void
ml_enlist(struct mbuf_list *mla, struct mbuf_list *mlb)
{
    if (!ml_empty(mlb)) {
        if (ml_empty(mla))
            mla->ml_head = mlb->ml_head;
        else
            mbuf_setnextpkt(mla->ml_tail, mlb->ml_head);
        mla->ml_tail = mlb->ml_tail;
        mla->ml_len += mlb->ml_len;

        ml_init(mlb);
    }
}

static inline mbuf_t
ml_dequeue(struct mbuf_list *ml)
{
    mbuf_t m;

    m = ml->ml_head;
    if (m != NULL) {
        ml->ml_head = mbuf_nextpkt(m);
        if (ml->ml_head == NULL)
            ml->ml_tail = NULL;

        mbuf_setnextpkt(m, NULL);
        ml->ml_len--;
    }

    return (m);
}

static inline mbuf_t
ml_dechain(struct mbuf_list *ml)
{
    mbuf_t m0;

    m0 = ml->ml_head;

    ml_init(ml);

    return (m0);
}

static inline unsigned int
ml_purge(struct mbuf_list *ml)
{
    mbuf_t m, n;
    unsigned int len;

    for (m = ml->ml_head; m != NULL; m = n) {
        n = mbuf_nextpkt(m);
        mbuf_freem(m);
    }

    len = ml->ml_len;
    ml_init(ml);

    return (len);
}

/*
 * mbuf queues
 */

static inline void
mq_init(struct mbuf_queue *mq, u_int maxlen, int ipl)
{
    mq->mq_mtx = IORecursiveLockAlloc();
    ml_init(&mq->mq_list);
    mq->mq_maxlen = maxlen;
}

static inline int
mq_enqueue(struct mbuf_queue *mq, mbuf_t m)
{
    int dropped = 0;

    IORecursiveLockLock(mq->mq_mtx);
    
    if (mq_len(mq) < mq->mq_maxlen)
        ml_enqueue(&mq->mq_list, m);
    else {
        mq->mq_drops++;
        dropped = 1;
    }

    if (dropped) {
        mbuf_freem(m);
    }
    
    IORecursiveLockUnlock(mq->mq_mtx);

    return (dropped);
}

static inline mbuf_t
mq_dequeue(struct mbuf_queue *mq)
{
    mbuf_t m;

    IORecursiveLockLock(mq->mq_mtx);
    m = ml_dequeue(&mq->mq_list);
    IORecursiveLockUnlock(mq->mq_mtx);

    return (m);
}

static inline int
mq_enlist(struct mbuf_queue *mq, struct mbuf_list *ml)
{
    mbuf_t m;
    int dropped = 0;

    IORecursiveLockLock(mq->mq_mtx);
    if (mq_len(mq) < mq->mq_maxlen)
        ml_enlist(&mq->mq_list, ml);
    else {
        dropped = ml_len(ml);
        mq->mq_drops += dropped;
    }
    IORecursiveLockUnlock(mq->mq_mtx);

    if (dropped) {
        while ((m = ml_dequeue(ml)) != NULL) {
            mbuf_freem(m);
        }
    }

    return (dropped);
}

static inline void
mq_delist(struct mbuf_queue *mq, struct mbuf_list *ml)
{
    IORecursiveLockLock(mq->mq_mtx);
    *ml = mq->mq_list;
    ml_init(&mq->mq_list);
    IORecursiveLockUnlock(mq->mq_mtx);
}

static inline mbuf_t
mq_dechain(struct mbuf_queue *mq)
{
    mbuf_t m0;

    IORecursiveLockLock(mq->mq_mtx);
    m0 = ml_dechain(&mq->mq_list);
    IORecursiveLockUnlock(mq->mq_mtx);

    return (m0);
}

static inline unsigned int
mq_purge(struct mbuf_queue *mq)
{
    struct mbuf_list ml;
    
    mq_delist(mq, &ml);

    return (ml_purge(&ml));
}

/*
 * Concatenate mbuf chain n to m.
 * n might be copied into m (when n->m_len is small), therefore data portion of
 * n could be copied into an mbuf of different mbuf type.
 * Therefore both chains should be of the same type (e.g. MT_DATA).
 * Any m_pkthdr is not updated.
 */
static inline void
m_cat(mbuf_t m, mbuf_t n)
{
    while (mbuf_next(m))
        m = mbuf_next(m);
    while (n) {
        if (mbuf_len(n) > mbuf_trailingspace(m)) {
            /* just join the two chains */
            mbuf_setnext(m, n);
            return;
        }
        /* splat the data from one into the other */
        memcpy(mtod(m, caddr_t) + mbuf_len(m), mtod(n, caddr_t),
            mbuf_len(n));
        mbuf_setlen(m, mbuf_len(m) + mbuf_len(n));
        n = mbuf_free(n);
    }
}

#define M_EXTWR        0x0008    /* external storage is writable */
#define    MAXMCLBYTES    (64 * 1024)        /* largest cluster from the stack */

/*
 * mbuf chain defragmenter. This function uses some evil tricks to defragment
 * an mbuf chain into a single buffer without changing the mbuf pointer.
 * This needs to know a lot of the mbuf internals to make this work.
 */
static inline int
m_defrag(mbuf_t m, int how)
{
    mbuf_t m0;

    if (mbuf_next(m) == NULL)
        return (0);

//    KASSERT(m->m_flags & M_PKTHDR);
    mbuf_gethdr(how, mbuf_type(m), &m0);
    if (m0 == NULL)
        return (ENOBUFS);
    if (mbuf_pkthdr_len(m) > mbuf_get_mhlen()) {
        mbuf_getcluster(how, mbuf_type(m), mbuf_pkthdr_len(m), &m0);
//        if (!(m0->m_flags & M_EXT)) {
//            m_free(m0);
//            return (ENOBUFS);
//        }
    }
    mbuf_copydata(m, 0, mbuf_pkthdr_len(m), mtod(m0, void*));
    mbuf_pkthdr_setlen(m0, mbuf_pkthdr_len(m));
    mbuf_setlen(m0, mbuf_pkthdr_len(m));

    /* free chain behind and possible ext buf on the first mbuf */
    mbuf_freem(mbuf_next(m));
    mbuf_setnext(m, NULL);
//    if (m->m_flags & M_EXT)
//        m_extfree(m);

    /*
     * Bounce copy mbuf over to the original mbuf and set everything up.
     * This needs to reset or clear all pointers that may go into the
     * original mbuf chain.
     */
    if (mbuf_flags(m0) & MBUF_EXT) {
//        memcpy(&m->m_ext, &m0->m_ext, sizeof(struct mbuf_ext));
//        MCLINITREFERENCE(m);
//        m->m_flags |= m0->m_flags & (M_EXT|M_EXTWR);
//        m->m_data = m->m_ext.ext_buf;
    } else {
        mbuf_setdata(m, mbuf_pkthdr_header(m), mbuf_pkthdr_len(m));
        memcpy(mbuf_data(m), mbuf_data(m0), mbuf_len(m0));
    }
    mbuf_pkthdr_setlen(m, mbuf_len(m0));
    mbuf_setlen(m, mbuf_len(m0));

    mbuf_setflags(m0, mbuf_flags(m0) & ~(MBUF_EXT | M_EXTWR));    /* cluster is gone */
    mbuf_free(m0);

    return (0);
}

/*
 * Duplicate mbuf pkthdr from from to to.
 * from must have M_PKTHDR set, and to must be empty.
 */
static inline int
m_dup_pkthdr(mbuf_t to, mbuf_t from, int wait)
{
    int error;
    mbuf_copy_pkthdr(to, from);
    mbuf_pkthdr_setlen(to, mbuf_pkthdr_len(from));
    mbuf_setlen(to, mbuf_pkthdr_len(from));
    return (0);
    
//    int error;

//    mbuf_setflags(to, mbuf_flags(to) & (MBUF_EXT | M_EXTWR));
////    to->m_flags = (to->m_flags & (M_EXT | M_EXTWR));
//    mbuf_setflags(to, mbuf_flags(to) | (mbuf_flags(from)));
////    to->m_flags |= (from->m_flags & M_COPYFLAGS);
//    mbuf_pkthdr_setheader(to, mbuf_pkthdr_header(from));
////    to->m_pkthdr = from->m_pkthdr;
//
////#if NPF > 0
////    to->m_pkthdr.pf.statekey = NULL;
////    pf_mbuf_link_state_key(to, from->m_pkthdr.pf.statekey);
////    to->m_pkthdr.pf.inp = NULL;
////    pf_mbuf_link_inpcb(to, from->m_pkthdr.pf.inp);
////#endif    /* NPF > 0 */
//
////    SLIST_INIT(&to->m_pkthdr.ph_tags);
//
////    if ((error = m_tag_copy_chain(to, from, wait)) != 0)
////        return (error);
//
//    if ((mbuf_flags(to) & MBUF_EXT) == 0)
//        mbuf_setdata(to, mbuf_pkthdr_header(to) , mbuf_pkthdr_len(to));
//
//    return (0);
}

static inline mbuf_t
m_dup_pkt(mbuf_t m0, unsigned int adj, int wait)
{
    mbuf_t m;
    int len;

    len = mbuf_pkthdr_len(m0) + adj;
    
    IOLog("itlwm: m_dup_pkt start, len=%lu\n", len);
    
    if (len > MAXMCLBYTES) /* XXX */
        return (NULL);

    mbuf_get((mbuf_how_t)wait, mbuf_type(m0), &m);
    if (m == NULL)
        return (NULL);

    if (m_dup_pkthdr(m, m0, wait) != 0)
        goto fail;

    if (len > mbuf_get_mhlen()) {
//        MCLGETI(m, wait, NULL, len);
        mbuf_mclget(wait, MBUF_TYPE_DATA, &m);
        if (!ISSET(mbuf_flags(m), MBUF_EXT))
            goto fail;
    }

    mbuf_setlen(m, len);
    mbuf_pkthdr_setlen(m, len);
//    m->m_len = m->m_pkthdr.len = len;
    mbuf_adj(m, adj);
    mbuf_copydata(m0, 0, mbuf_pkthdr_len(m0), mtod(m, caddr_t));

    return (m);

fail:
    IOLog("itlwm: m_dup_pkt fail!!!!\n");
    mbuf_freem(m);
    return (NULL);
}

static IORecursiveLock *inputLock = IORecursiveLockAlloc();

static inline int if_input(struct ifnet *ifq, struct mbuf_list *ml)
{
    IWL_ERR(0, "%s\n", __FUNCTION__);
    mbuf_t m;
    uint64_t packets;
    bool isEmpty = false;
    
    IORecursiveLockLock(inputLock);
    isEmpty = ml_empty(ml);
    if (isEmpty) {
        IORecursiveLockUnlock(inputLock);
        return (0);
    }
    MBUF_LIST_FOREACH(ml, m) {
        if (ifq->iface == NULL) {
            IWL_ERR(0, "%s ifq->iface == NULL!!!\n", __FUNCTION__);
            break;
        }
        if (m == NULL) {
            IWL_ERR(0, "%s m == NULL!!!\n", __FUNCTION__);
            continue;
        }
        IWL_ERR(0, "%s %d 啊啊啊啊 ifq->iface->inputPacket(m)\n", __FUNCTION__, __LINE__);
        ifq->iface->inputPacket(m);
        if (ifq->netStat != NULL) {
            ifq->netStat->inputPackets++;
        }
    }
    if (!isEmpty) {
        ifq->iface->flushInputQueue();
    }
    IORecursiveLockUnlock(inputLock);
    return 1;
}

extern int TX_TYPE_MGMT;
extern int TX_TYPE_FRAME;

static inline int if_enqueue(struct ifnet *ifq, mbuf_t m)
{
//    ifq->output_queue->enqueue(m, &TX_TYPE_FRAME);
    IWL_ERR(0, "%s 啊啊啊啊 if_enqueue!!\n", __FUNCTION__);
    ifq->if_snd->enqueue(m);
    return 0;
}

#endif /* _mbuf_h */
