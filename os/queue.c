#include "queue.h"
#include "defs.h"
//#include "proc.h"

struct proc;
extern struct proc pool[];

void init_queue(struct queue *q)
{
	q->front = q->tail = 0;
	q->empty = 1;
}

void push_queue(struct queue *q, int value)
{
	if (!q->empty && q->front == q->tail) {
		panic("queue shouldn't be overflow");
	}

	// Project 3: instead of a plain FIFO push, insert and bubble leftward so
	// the queue stays sorted by stride ascending. pop_queue then always returns
	// the front element, which is guaranteed to be the minimum-stride process
	q->empty = 0;
    int pos = q->tail;
    q->data[pos] = value;
    q->tail = (q->tail + 1) % QUEUE_SIZE;

    // Bubble the new element leftward until it is >= its predecessor,
	// restoring the ascending-stride invariant after each insertion
    int cur = (q->tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
    int prev = (cur - 1 + QUEUE_SIZE) % QUEUE_SIZE;

    while (cur != q->front) {
        if (pool[q->data[cur]].stride < pool[q->data[prev]].stride) {
            // Swap
            int tmp = q->data[cur];
            q->data[cur] = q->data[prev];
            q->data[prev] = tmp;
            cur = prev;
            prev = (cur - 1 + QUEUE_SIZE) % QUEUE_SIZE;
        } else {
            break;
        }
    }
}

int pop_queue(struct queue *q)
{
	if (q->empty)
		return -1;
	int value = q->data[q->front];
	q->front = (q->front + 1) % QUEUE_SIZE; // PROJECT 3: use QUEUE_SIZE to match actual buffer capacity
	if (q->front == q->tail)
		q->empty = 1;
	return value;
}
