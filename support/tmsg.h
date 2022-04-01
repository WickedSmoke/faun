#ifndef TMSG_H
#define TMSG_H

struct MsgPort;

#ifdef __cplusplus
extern "C" {
#endif

struct MsgPort* tmsg_create(int msgSize, int capacity);
void   tmsg_destroy(struct MsgPort*);
int    tmsg_used(struct MsgPort*);
int    tmsg_push(struct MsgPort*, const void* msg);
int    tmsg_pop(struct MsgPort*, void* msg);
int    tmsg_pushTimeout(struct MsgPort*, const void* msg, int msec);
int    tmsg_popTimeout(struct MsgPort*, void* msg, int msec);

#ifdef __cplusplus
}
#endif

#endif  // TMSG_H
